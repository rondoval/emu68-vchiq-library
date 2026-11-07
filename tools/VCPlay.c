/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * VCPlay - the smallest player on videocodec.library: an H.264 or H.265
 * elementary stream in a window on the Workbench.  The example to copy from.
 *
 *   VCPlay FILE/A
 *
 * A name ending in .h265, .hevc or .265 is H.265, any other H.264.
 *
 * One decoder session.  The session hands out its input packets; each is
 * filled from the file and sent back.  When the decoder has told the
 * picture's size, a window opens at that size (scaled down to fit the
 * screen) and the session's output takes the window's size and the screen's
 * pixel format: from then on the frames come back full of RGB.  A timer ticks
 * at the stream's frame rate; at each tick the frame waiting is copied into
 * the window with p96WritePixelArray() and released, while the decoder fills
 * its other frames.  The decoding, the scaling and the colour conversion all
 * happen in the VPU.  The H.265 decoder writes a format of its own that no
 * screen shows: the screen's comes from its row's ChainOutputFormats, and the
 * library puts a converter behind the decoder for it, inside the session.
 *
 * The close gadget, Ctrl-C or the end of the stream stops it.  Nothing here
 * blocks except the Wait(): a program with a frame loop of its own opens the
 * session the same way, calls VC_Poll() once per pass and shows the newest
 * frame.
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/Picasso96.h>
#include <proto/videocodec.h>

#include <exec/types.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/Picasso96.h>
#include <string.h>
#include <libraries/videocodec.h>

struct Library *VideoCodecBase;
struct Library *P96Base;
struct IntuitionBase *IntuitionBase;

unsigned long __stack = 16384; /* a window and Picasso96 want more than a shell's default */

#ifdef VERSTAG /* "$VER: VCPlay x.y (date)", given by the build */
static const char verstag[] __attribute__((used)) = VERSTAG;
#endif

/* The RGB layouts the backend may write, by their Picasso96 names. */
static const struct pixel_format
{
	RGBFTYPE rgb;
	ULONG vcf;
} formats[] = {
    {RGBFB_R5G6B5PC, VCF_RGB16}, {RGBFB_R8G8B8, VCF_RGB24},     {RGBFB_B8G8R8, VCF_BGR24},
    {RGBFB_B8G8R8A8, VCF_BGRA32}, {RGBFB_R8G8B8A8, VCF_RGBA32},
};
#define FORMATS_NUM (sizeof(formats) / sizeof(formats[0]))

/* The codecs by the end of a file's name; any other name is H.264. */
static const struct stream_codec
{
	const char *suffix;
	ULONG codec;
	const char *name;
} codecs[] = {
    {".h265", VCC_H265, "H.265"},
    {".hevc", VCC_H265, "H.265"},
    {".265", VCC_H265, "H.265"},
};
#define CODECS_NUM (sizeof(codecs) / sizeof(codecs[0]))
static const struct stream_codec codec_h264 = {"", VCC_H264, "H.264"};

struct player
{
	struct VCSession *s;
	BYTE sig; /* the session signals it on every return */
	BPTR file;
	const struct stream_codec *codec;
	BOOL eof_sent;
	const struct pixel_format *pf;
	struct Window *window;
	struct VCFrame *next; /* the frame waiting for the next tick, or NULL */
	ULONG frame_us;       /* the tick */
	ULONG shown;
	BOOL eos, done, failed;
	struct MsgPort *timer_port;
	struct timerequest *timer;
	BOOL timer_ok; /* the device is open */
};

/* ---- the clock ---------------------------------------------------------------- */

static BOOL timer_open(struct player *p)
{
	p->timer_port = CreateMsgPort();
	if (p->timer_port == NULL)
		return FALSE;
	p->timer = (struct timerequest *)CreateIORequest(p->timer_port, sizeof(*p->timer));
	p->timer_ok = p->timer != NULL && OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)p->timer, 0) == 0;
	return p->timer_ok;
}

static void timer_close(struct player *p)
{
	if (p->timer_ok)
		CloseDevice((struct IORequest *)p->timer);
	if (p->timer != NULL)
		DeleteIORequest((struct IORequest *)p->timer);
	if (p->timer_port != NULL)
		DeleteMsgPort(p->timer_port);
}

static void timer_arm(struct player *p, ULONG us)
{
	p->timer->tr_node.io_Command = TR_ADDREQUEST;
	p->timer->tr_time.tv_secs = us / 1000000u;
	p->timer->tr_time.tv_micro = us % 1000000u;
	SendIO((struct IORequest *)p->timer);
}

/* The tick in flight, stopped and collected. */
static void timer_cancel(struct player *p)
{
	if (CheckIO((struct IORequest *)p->timer) == NULL)
		AbortIO((struct IORequest *)p->timer);
	WaitIO((struct IORequest *)p->timer);
}

/* ---- the stream ----------------------------------------------------------------- */

/* A packet back from the session takes the next piece of the file, or the end mark once. */
static void packet_fill(struct player *p, struct VCPacket *pk)
{
	if (p->eof_sent)
		return;
	LONG n = Read(p->file, pk->Data, (LONG)pk->Capacity);
	if (n < 0)
	{
		PrintFault(IoErr(), (CONST_STRPTR) "read");
		p->failed = TRUE;
		return;
	}
	pk->Length = (ULONG)n;
	pk->Flags = n == 0 ? VCPF_EndOfStream : 0;
	pk->Pts = VC_TIME_UNKNOWN;
	p->eof_sent = n == 0;
	LONG rc = VC_SendPacket(p->s, pk);
	if (rc != 0)
	{
		Printf((CONST_STRPTR) "send: %ld\n", rc);
		p->failed = TRUE;
	}
}

/* The codec of a file, by the end of its name in any case. */
static const struct stream_codec *codec_of_file(const char *name)
{
	size_t length = strlen(name);
	for (ULONG i = 0; i < CODECS_NUM; i++)
	{
		size_t suffix_length = strlen(codecs[i].suffix);
		if (length >= suffix_length && strcasecmp(name + length - suffix_length, codecs[i].suffix) == 0)
			return &codecs[i];
	}
	return &codec_h264;
}

/* ---- the window ------------------------------------------------------------------ */

/*
 * One of formats[] that a mask holds, the best for the screen: the screen's
 * own format when the mask holds it, so that a frame reaches the window with
 * a plain copy; else the first of formats[] the mask holds, which
 * p96WritePixelArray() converts to the screen's as it copies.  The mask's
 * other formats (YUV, the H.265 decoder's columns) are never chosen:
 * Picasso96 cannot draw them.  NULL when the mask holds none of formats[].
 *
 * mask:       VCF_MASK()s of the formats a session can write
 * screen_rgb: the Workbench screen's pixel format (RGBFB_*)
 */
static const struct pixel_format *format_in(ULONG mask, RGBFTYPE screen_rgb)
{
	const struct pixel_format *first = NULL;
	for (ULONG f = 0; f < FORMATS_NUM; f++)
	{
		if (!(mask & VCF_MASK(formats[f].vcf)))
			continue;
		if (formats[f].rgb == screen_rgb)
			return &formats[f];
		if (first == NULL)
			first = &formats[f];
	}
	return first;
}

/*
 * The pixel format to ask the session for, from the first decoder row of the
 * file's codec.  A session writes two sets of formats: the decoder's own
 * (OutputFormats), and those of a converter the library puts behind the
 * decoder when the session asks for one of them (ChainOutputFormats; 0 from
 * a library older than the field).  In this order:
 *
 * 1. the screen's own format, from either set: a plain copy into the window;
 * 2. another format the decoder writes itself: one pass through the hardware,
 *    and Picasso96 converts it as it copies;
 * 3. another format the converter adds.
 *
 * The H.264 decoder writes the RGB formats itself.  The H.265 decoder writes
 * only its own column format, so its frames always come through the
 * converter.
 */
static BOOL format_choose(struct player *p)
{
	struct Screen *scr = LockPubScreen(NULL);
	if (scr == NULL)
		return FALSE;
	RGBFTYPE screen_rgb = (RGBFTYPE)p96GetBitMapAttr(scr->RastPort.BitMap, P96BMA_RGBFORMAT);
	UnlockPubScreen(NULL, scr);

	for (ULONG i = 0;; i++)
	{
		struct VCCodecInfo ci;
		ci.Size = sizeof(ci);
		if (VC_QueryCodec(i, &ci) != 0)
			break;
		if (ci.Codec != p->codec->codec || ci.Kind != VCK_Decoder)
			continue;
		ULONG chained = VC_HAS_FIELD(ci.Size, struct VCCodecInfo, ChainOutputFormats) ? ci.ChainOutputFormats : 0;
		const struct pixel_format *own = format_in(ci.OutputFormats, screen_rgb);           /* the decoder's set */
		const struct pixel_format *any = format_in(ci.OutputFormats | chained, screen_rgb); /* both sets */
		if (any != NULL && any->rgb == screen_rgb)
			p->pf = any; /* 1: the screen's format, from whichever set has it */
		else
			p->pf = own != NULL ? own : any; /* 2, else 3 */
		if (p->pf != NULL)
			return TRUE;
	}
	Printf((CONST_STRPTR) "no backend decodes %s to an RGB format Picasso96 knows\n", p->codec->name);
	return FALSE;
}

/*
 * The decoder told the picture's size: a window of that size, or smaller
 * with the aspect kept when the screen is smaller, and the session's output
 * takes the window's inner size.  Rows are whole groups of 32 pixels, as the
 * backend asks.
 */
static BOOL window_open(struct player *p, const struct VCStreamInfo *si)
{
	struct Screen *scr = LockPubScreen(NULL);
	if (scr == NULL)
		return FALSE;
	ULONG w = (ULONG)si->Crop.Width, h = (ULONG)si->Crop.Height;
	ULONG room_w = (ULONG)scr->Width - 16u, room_h = (ULONG)scr->Height - (ULONG)scr->BarHeight - 32u;
	if (w > room_w)
	{
		h = h * room_w / w;
		w = room_w;
	}
	if (h > room_h)
	{
		w = w * room_h / h;
		h = room_h;
	}
	w &= ~31u;
	h &= ~1u;
	p->window = OpenWindowTags(NULL, WA_PubScreen, (ULONG)scr, WA_Title, (ULONG) "VCPlay", WA_InnerWidth, w, WA_InnerHeight, h,
	                           WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_SmartRefresh, TRUE, WA_Activate,
	                           TRUE, WA_IDCMP, IDCMP_CLOSEWINDOW, TAG_DONE);
	UnlockPubScreen(NULL, scr);
	if (p->window == NULL || w == 0 || h == 0)
	{
		Printf((CONST_STRPTR) "no window for a %ldx%ld picture\n", w, h);
		return FALSE;
	}
	LONG rc = VC_SetAttrsTags(p->s, VCA_OutputWidth, w, VCA_OutputHeight, h, TAG_DONE);
	if (rc != 0)
	{
		Printf((CONST_STRPTR) "output size %ldx%ld: %ld\n", w, h, rc);
		return FALSE;
	}
	if (si->FrameRate.Num > 0 && si->FrameRate.Den > 0) /* else the 30 fps it started with */
		p->frame_us = (ULONG)(1000000ull * (ULONG)si->FrameRate.Den / (ULONG)si->FrameRate.Num);
	Printf((CONST_STRPTR) "%ldx%ld, %ld fps, in a %ldx%ld window\n", si->Crop.Width, si->Crop.Height, 1000000u / p->frame_us, w, h);
	return TRUE;
}

/* A frame into the window: Picasso96 copies it, clipped by the layers; then back to the session to be filled again. */
static void frame_show(struct player *p, struct VCFrame *f)
{
	struct RenderInfo ri;
	ri.Memory = f->Data;
	ri.BytesPerRow = (WORD)f->Pitch[0];
	ri.pad = 0;
	ri.RGBFormat = p->pf->rgb;
	p96WritePixelArray(&ri, (UWORD)f->Crop.X, (UWORD)f->Crop.Y, p->window->RPort, (UWORD)p->window->BorderLeft, (UWORD)p->window->BorderTop,
	                   (UWORD)f->Crop.Width, (UWORD)f->Crop.Height);
	VC_ReleaseFrame(p->s, f);
	p->shown++;
}

static void window_check(struct player *p)
{
	struct IntuiMessage *im;
	while (p->window != NULL && (im = (struct IntuiMessage *)GetMsg(p->window->UserPort)) != NULL)
	{
		ULONG cls = im->Class;
		ReplyMsg((struct Message *)im);
		if (cls == IDCMP_CLOSEWINDOW)
			p->done = TRUE;
	}
}

/* ---- the loop ----------------------------------------------------------------------- */

static void event(struct player *p, const struct VCEvent *ev)
{
	switch (ev->Event)
	{
	case VCEV_StreamInfo:
		/* the first one opens the window; a later change of shape is scaled into it by the backend */
		if (p->window == NULL && !window_open(p, &ev->Stream))
			p->failed = TRUE;
		break;
	case VCEV_EndOfStream:
		p->eos = TRUE;
		break;
	case VCEV_Error:
		Printf((CONST_STRPTR) "session error %ld\n", ev->Status);
		p->failed = TRUE;
		break;
	default:
		break;
	}
}

/* Sleep until one of @mask, the next tick, the window or Ctrl-C calls. */
static void sleep_until(struct player *p, ULONG mask)
{
	mask |= (1UL << p->timer_port->mp_SigBit) | SIGBREAKF_CTRL_C;
	if (p->window != NULL)
		mask |= 1UL << p->window->UserPort->mp_SigBit;
	if (Wait(mask) & SIGBREAKF_CTRL_C)
		p->done = TRUE;
	window_check(p);
}

/*
 * A timer ticks once a frame; each tick shows the frame waiting, if one is.
 * One item per pass, Ctrl-C looked at every time: a decoder that hands its
 * input straight back cannot keep us from it.  While a frame waits nothing
 * more is taken; what the session has ready keeps.
 */
static void play(struct player *p)
{
	timer_arm(p, p->frame_us);
	while (!p->done && !p->failed && !p->eos)
	{
		if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
			break;
		if (CheckIO((struct IORequest *)p->timer) != NULL)
		{
			/* a tick: the next one armed first, so the copy stays out of the period */
			WaitIO((struct IORequest *)p->timer);
			timer_arm(p, p->frame_us);
			if (p->next != NULL)
				frame_show(p, p->next);
			p->next = NULL;
		}
		struct VCItem it;
		if (p->next != NULL || !VC_Poll(p->s, &it))
			sleep_until(p, p->next != NULL ? 0 : 1UL << p->sig);
		else if (it.Type == VCIT_PacketReturned)
			packet_fill(p, it.u.Packet);
		else if (it.Type == VCIT_Frame)
			p->next = it.u.Frame;
		else if (it.Type == VCIT_Event)
			event(p, &it.u.Event);
	}
	timer_cancel(p);
}

/* ---- main ---------------------------------------------------------------------------- */

int main(void)
{
	LONG args[1] = {0};
	struct RDArgs *rda = ReadArgs((CONST_STRPTR) "FILE/A", args, NULL);
	if (rda == NULL)
	{
		PrintFault(IoErr(), (CONST_STRPTR) "VCPlay");
		return RETURN_FAIL;
	}

	int rc = RETURN_ERROR;
	static struct player p;
	p.sig = -1;
	p.frame_us = 1000000u / 30u; /* until the stream tells its rate */
	IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR) "intuition.library", 39);
	P96Base = OpenLibrary((CONST_STRPTR)P96NAME, 2);
	VideoCodecBase = OpenLibrary((CONST_STRPTR)VIDEOCODEC_NAME, 0);
	if (IntuitionBase == NULL || P96Base == NULL || VideoCodecBase == NULL)
	{
		Printf((CONST_STRPTR) "missing: %s%s%s\n", IntuitionBase == NULL ? "intuition.library " : "", P96Base == NULL ? "Picasso96API.library " : "",
		       VideoCodecBase == NULL ? "videocodec.library" : "");
		goto out;
	}
	p.sig = AllocSignal(-1);
	if (p.sig == -1 || !timer_open(&p))
	{
		Printf((CONST_STRPTR) "no signal or timer\n");
		goto out;
	}
	p.codec = codec_of_file((const char *)args[0]);
	p.file = Open((CONST_STRPTR)args[0], MODE_OLDFILE);
	if (p.file == 0)
	{
		PrintFault(IoErr(), (CONST_STRPTR)args[0]);
		goto out;
	}
	if (!format_choose(&p))
		goto out;

	/* the output format now, its size once the stream's is known: the backend waits for it after the first event */
	LONG err = 0;
	p.s = VC_OpenSessionTags(VCA_Codec, p.codec->codec, VCA_Kind, VCK_Decoder, VCA_OutputFormat, p.pf->vcf, VCA_SigTask, (ULONG)FindTask(NULL),
	                         VCA_SigMask, 1UL << p.sig, VCA_ErrorPtr, (ULONG)&err, TAG_DONE);
	if (p.s == NULL)
	{
		Printf((CONST_STRPTR) "no decoder session: %ld\n", err);
		goto out;
	}
	play(&p);
	VC_CloseSession(p.s); /* before the window goes: the frames are the session's */
	Printf((CONST_STRPTR) "%ld frames shown\n", p.shown);
	rc = p.failed ? RETURN_FAIL : RETURN_OK;

out:
	if (p.window != NULL)
		CloseWindow(p.window);
	if (p.file != 0)
		Close(p.file);
	timer_close(&p);
	if (p.sig != -1)
		FreeSignal(p.sig);
	if (VideoCodecBase != NULL)
		CloseLibrary(VideoCodecBase);
	if (P96Base != NULL)
		CloseLibrary(P96Base);
	if (IntuitionBase != NULL)
		CloseLibrary((struct Library *)IntuitionBase);
	FreeArgs(rda);
	return rc;
}
