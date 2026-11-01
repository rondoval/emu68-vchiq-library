/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * VPUPlay - plays an H.264 elementary stream on an RTG screen through
 * videocodec.library (the display path).
 *
 *   VPUPlay FILE/A [FPS/N] [FAST/S] [FRAMES/N] [DEPTH/N] [WIDTH/N] [HEIGHT/N] [VERBOSE/S] [WINDOW/S] [LOOP/S]
 *           [WINDOWTEST/S] [TRACE/S]
 *
 * The measurement rig of the display path: its facts come out as key=value
 * lines (kv.h: play.*, stream.N.*, screen.*, output.N.*, window.*), the
 * verdict last; the test suite (tests/) reads them.  TRACE echoes every
 * line to Emu68's debug log, where a hang can be read from when the screen
 * is in front.  VCPlay is the small example to copy from.
 *
 * One decoder session with an RGB output: the backend decodes and scales
 * inside the VPU and writes each picture by DMA straight into a Picasso96
 * screen's bitmap in VRAM.  The screen is opened three picture heights tall
 * on one bitmap, its parts imported as the session's output frames: a frame
 * lands in a hidden part and the viewport is scrolled to it (the RTG card's
 * panning changes the display start address at the next frame).  A part
 * that left the display is not released for one refresh, so the display
 * never scans what the VPU writes; after that it is released while the
 * frame on display waits out its time.  Per frame the 68k releases one
 * frame and gets it back full: it never touches a pixel.  The pointer is
 * blanked over the picture.
 *
 * WINDOW plays in a window on the default public screen instead (WIDTH and
 * HEIGHT then give its inner size; default half the screen).  A window
 * shares its screen's bitmap with everything else on it, so the VPU does
 * not write there: the session's own frames in Fast RAM take the pictures,
 * and each is copied into the window with p96WritePixelArray(), clipped by
 * the layers like any drawing.  Resizing the window rescales the picture
 * (the session's output geometry is changed; the decoder runs on).
 * WINDOWTEST makes the window move and change size by itself while it plays,
 * as a user dragging it would, for a test script (developer-guide.md 8.3).
 *
 * LOOP starts the file over at its end, without an end-of-stream mark: an
 * Annex-B stream that opens with its parameter sets and a keyframe just
 * goes on as one long stream, so the decoder never stops and the seam
 * costs nothing.  Until a key, Ctrl-C or FRAMES n.
 *
 * Pacing: a frame is shown no earlier than its due time (FPS, else the
 * stream's rate, else 30), the clock starting with the first picture; FAST
 * shows frames as soon as they are ready.  Any key or mouse button, Ctrl-C,
 * FRAMES n or the end of the stream stops it.
 *
 * Return codes: 0 PASS, 5 stopped before a picture (fail=), 10 a library or
 * the screen is missing, 20 failed.
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/dos_protos.h>
#include <clib/intuition_protos.h>
#include <clib/graphics_protos.h>
#include <clib/Picasso96_protos.h>
#include <clib/videocodec_protos.h>
#else
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/Picasso96.h>
#include <proto/videocodec.h>
#endif

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <dos/rdargs.h>
#include <dos/dosextens.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <graphics/view.h>
#include <graphics/displayinfo.h>
#include <libraries/Picasso96.h>

#include <timing.h>
#include <libraries/videocodec.h>

#include <args.h>
#include <kv.h>
#include <pixel_format.h>
#include <waiter.h>

struct Library *VideoCodecBase;
struct Library *P96Base;
struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;

unsigned long __stack = 32768;

static const char verstag[] __attribute__((used)) = VERSTAG;

#define IDLE_MS 5000  /* nothing moved for this long: stuck */
#define COOL_US 25000 /* a part left the display this long ago is no longer scanned (one refresh at 50 Hz, with margin) */
#define MAX_PARTS 3   /* picture heights on the screen bitmap, or frames in window mode */
#define POLL_BATCH 16 /* items taken per pass: a decoder that hands input straight back must not keep us from the checks */

/* What the session writes: @width x @height pixels of @pf, rows @pitch bytes apart; the picture fitted in or filling it. */
struct target
{
	const struct pixel_format *pf;
	ULONG width, height, pitch;
	BOOL keep_aspect;
	ULONG num;                /* frames */
	BOOL imported;            /* the bitmap's parts, @payloads; else the session's own frames */
	APTR payloads[MAX_PARTS];
	const char *what;
};

struct play
{
	struct waiter waiter;
	BOOL verbose, fast;
	ULONG max_frames;

	/* the stream */
	BPTR file;
	BOOL eof_sent;
	ULONG in_bytes;
	BOOL loop;   /* at the end, start over */
	ULONG loops; /* times started over */

	/* the session */
	struct VCSession *s;
	ULONG video_w, video_h; /* the visible picture */
	ULONG fps;
	ULONG streams, outputs; /* stream.N.* and output.N.* printed */
	ULONG isp_formats; /* RGBFF_* the backend can write */
	BOOL display_ready;

	/* the screen: one bitmap, @parts picture heights tall, scrolled */
	struct Screen *screen;
	struct Window *window;
	UWORD *blank_pointer; /* Chip RAM sprite data, all transparent */
	struct RenderInfo ri; /* from a p96LockBitMap() held only to read it: a displayed bitmap stays in VRAM */
	ULONG parts;
	struct VCFrame *frame[MAX_PARTS]; /* the session's frame in each part: ours whenever not released */
	ULONG screen_w, screen_h;
	LONG shown;            /* part on display */
	LONG ready[MAX_PARTS]; /* parts holding frames not shown yet, oldest first */
	ULONG ready_n;
	BOOL at_session[MAX_PARTS]; /* released, not back yet */
	BOOL cooling[MAX_PARTS]; /* left the display less than a refresh ago: may still be scanned */
	u32 free_at[MAX_PARTS];  /* when a cooling part may be written again */

	/* pacing and statistics */
	u32 frame_us;
	u32 t_start, next_due;
	u32 t_wait, t_read; /* the player's time in Wait() and in Read() */

	/* the flipper: a process of its own one priority below, since ScrollVPort() waits for the vertical blank */
	struct Process *flipper;
	volatile LONG flip_to;     /* the part to show; the flipper sets it to -1 when shown */
	volatile u32 flip_done_at; /* when the last flip returned */
	volatile u32 t_flip;       /* the flipper's time in ScrollVPort() */
	LONG flip_old;             /* the part the pending flip takes off the display, -1 when none */
	BOOL flipper_used;

	/* window mode: the session writes its own frames in Fast RAM, each copied into the window when due */
	BOOL windowed;
	struct target target; /* what the session writes now */
	LONG pic_x, pic_y;    /* the picture's place in the window */
	u32 t_copy;           /* time in p96WritePixelArray() */
	ULONG copies;
	ULONG moves, resizes, refreshes; /* the window's events, for a script to check its own drags */
	BOOL window_test;                /* WINDOWTEST: the window moves and resizes by itself */
	ULONG window_test_done;          /* its steps taken */
	ULONG frames, displayed, late;
	BOOL eos, failed, stop;
};

/* ---- waiting ------------------------------------------------------------------ */

/**
 * wait_any() - Sleep until something happens: the session, the window, the pacing timer or Ctrl-C.
 * @p:          the player
 * @timeout_ms: the pacing timer
 * @timed_out:  where TRUE goes when nothing but the timer happened
 *
 * Return: TRUE unless Ctrl-C.
 */
static BOOL wait_any(struct play *p, ULONG timeout_ms, BOOL *timed_out)
{
	ULONG extra = p->window != NULL ? 1UL << p->window->UserPort->mp_SigBit : 0;
	u32 t0 = get_time();
	ULONG seen = waiter_wait(&p->waiter, extra, timeout_ms);
	p->t_wait += get_time() - t0;
	*timed_out = seen == 0;
	return (seen & SIGBREAKF_CTRL_C) == 0;
}

/* ---- the stream ------------------------------------------------------------------ */

/* A packet the session handed back takes the next chunk of the file, or the end mark once. */
static void fill_input(struct play *p, struct VCPacket *pk)
{
	if (p->eof_sent || p->failed)
		return;
	u32 t0 = get_time();
	LONG n = Read(p->file, pk->Data, (LONG)pk->Capacity);
	if (n == 0 && p->loop && p->in_bytes != 0)
	{
		/* the end: from the top again, the stream simply continuing */
		if (Seek(p->file, 0, OFFSET_BEGINNING) == -1)
			n = -1;
		else
		{
			p->loops++;
			if (p->verbose)
				note("loop %lu\n", p->loops);
			n = Read(p->file, pk->Data, (LONG)pk->Capacity);
		}
	}
	p->t_read += get_time() - t0;
	if (n < 0)
	{
		PrintFault(IoErr(), (CONST_STRPTR) "read");
		kv_str("fail", "reading the stream failed");
		p->failed = TRUE;
		return;
	}
	pk->Length = (ULONG)n;
	pk->Flags = n == 0 ? VCPF_EndOfStream : 0;
	pk->Pts = VC_TIME_UNKNOWN;
	if (n == 0)
		p->eof_sent = TRUE;
	LONG rc = VC_SendPacket(p->s, pk);
	if (rc != 0)
	{
		kv_num("packet.error", rc);
		kv_str("fail", "VC_SendPacket failed");
		p->failed = TRUE;
		return;
	}
	p->in_bytes += (ULONG)n;
}

/* ---- pixel formats ---------------------------------------------------------------- */

/* The first format of @mask, in table order. */
static const struct pixel_format *first_format(ULONG mask)
{
	for (const struct pixel_format *pf = pixel_formats; pf->name != NULL; pf++)
		if (mask & pixel_format_bit(pf))
			return pf;
	return NULL;
}

/* The names of the formats in @mask, comma separated: a kv list, and a message. */
static const char *format_names(ULONG mask, char *out, ULONG room)
{
	ULONG at = 0;
	for (const struct pixel_format *pf = pixel_formats; pf->name != NULL; pf++)
	{
		if ((mask & pixel_format_bit(pf)) == 0)
			continue;
		const char *s = pf->name;
		if (at != 0 && at < room - 1u)
			out[at++] = ',';
		while (*s != 0 && at < room - 1u)
			out[at++] = *s++;
	}
	out[at] = 0;
	return at != 0 ? out : "none";
}

/* Which of the Picasso96 formats the backend writes for @codec: its decoder's format list. */
static BOOL query_formats(struct play *p)
{
	for (ULONG i = 0;; i++)
	{
		struct VCCodecInfo ci;
		ci.Size = sizeof(ci);
		if (VC_QueryCodec(i, &ci) != 0)
			break;
		if (ci.Codec != VCC_H264 || ci.Kind != VCK_Decoder)
			continue;
		for (const struct pixel_format *pf = pixel_formats; pf->name != NULL; pf++)
			if (ci.Formats & VCF_MASK(pf->vcf))
				p->isp_formats |= pixel_format_bit(pf);
		char names[128];
		kv_str("backend.name", (const char *)ci.Backend);
		kv_str("backend.formats", format_names(p->isp_formats, names, sizeof(names)));
		if (p->isp_formats == 0)
			kv_str("fail", "the backend writes no Picasso96 format");
		return p->isp_formats != 0;
	}
	kv_str("fail", "no backend decodes H.264");
	return FALSE;
}

/* ---- parts ------------------------------------------------------------------------- */

/* Part @k back to the session, to be filled. */
static void part_release(struct play *p, LONG k)
{
	if (p->frame[k] == NULL)
		return;
	VC_ReleaseFrame(p->s, p->frame[k]);
	p->at_session[k] = TRUE;
}

static void parts_forget(struct play *p)
{
	for (ULONG k = 0; k < MAX_PARTS; k++)
	{
		p->frame[k] = NULL;
		p->at_session[k] = FALSE;
		p->cooling[k] = FALSE;
	}
	p->ready_n = 0;
}

/* ---- the geometry ---------------------------------------------------------------------- */

/* The session's output takes @t; the bitmap's parts are imported as its frames, each told its part number. */
static BOOL target_apply(struct play *p, const struct target *t)
{
	LONG rc = VC_SetAttrsTags(p->s, VCA_OutputFormat, t->pf->vcf, VCA_OutputWidth, t->width, VCA_OutputHeight, t->height, VCA_OutputPitch,
	                          t->pitch, t->keep_aspect ? VCA_KeepAspect : TAG_IGNORE, TRUE, TAG_DONE);
	if (rc != 0)
	{
		kv_num("output.error", rc);
		kv_str("fail", "VC_SetAttrs refused the output geometry");
		return FALSE;
	}
	kv_str(kv_at("output", p->outputs, "format"), t->pf->name);
	kv_unum(kv_at("output", p->outputs, "width"), t->width);
	kv_unum(kv_at("output", p->outputs, "height"), t->height);
	kv_unum(kv_at("output", p->outputs, "pitch"), t->pitch);
	kv_bool(kv_at("output", p->outputs, "fitted"), t->keep_aspect);
	kv_unum(kv_at("output", p->outputs, "frames"), t->num);
	kv_str(kv_at("output", p->outputs, "kind"), t->imported ? "part" : "frame");
	p->outputs++;
	parts_forget(p);
	p->parts = t->num;
	p->target = *t;
	for (ULONG k = 0; t->imported && k < t->num; k++)
	{
		p->frame[k] = VC_ImportFrame(p->s, VCS_Output, t->payloads[k]);
		if (p->frame[k] == NULL)
		{
			kv_str("fail", "VC_ImportFrame refused a bitmap part");
			note("the session took no frame over %s %lu\n", t->what, k);
			return FALSE;
		}
		p->frame[k]->UserData = (APTR)(k + 1u); /* its part, read back when it comes full */
	}
	return TRUE;
}

/* ---- the screen ----------------------------------------------------------------- */

/* The Workbench's depth: the one the user has screen modes for. */
static ULONG workbench_depth(void)
{
	struct Screen *wb = LockPubScreen(NULL);
	if (wb == NULL)
		return 0;
	ULONG depth = p96GetBitMapAttr(wb->RastPort.BitMap, P96BMA_DEPTH);
	UnlockPubScreen(NULL, wb);
	return depth;
}
/*
 * Show part @k: the viewport scrolls down the tall bitmap; the card pans the
 * display to it.  Picasso96 waits for the vertical blank in ScrollVPort(),
 * and the VideoCore card, having no vblank interrupt, busy-polls the scan
 * line for it: up to a refresh per call (19 ms on average measured).
 */
static void show_part(struct play *p, LONG k)
{
	u32 t0 = get_time();
	p->screen->ViewPort.RasInfo->RyOffset = (WORD)((ULONG)k * p->screen_h);
	ScrollVPort(&p->screen->ViewPort);
	u32 t1 = get_time();
	p->t_flip += t1 - t0;
	p->flip_done_at = t1;
}

/*
 * The flipper process runs the flips, so that the wait inside them never
 * holds up the player.  One below the player's priority: the player always
 * gets in when the VPU or the clock calls, and the poll eats only time the
 * player does not want.  CTRL_F: show p->flip_to; CTRL_C: leave (CTRL_E
 * back to the player, under Forbid() so the process is gone before it runs).
 */
static struct play *flipper_play;

static void flipper_main(void)
{
	struct play *p = flipper_play;
	for (;;)
	{
		ULONG sig = Wait(SIGBREAKF_CTRL_F | SIGBREAKF_CTRL_C);
		if ((sig & SIGBREAKF_CTRL_F) && p->flip_to >= 0)
		{
			show_part(p, p->flip_to);
			p->flip_to = -1;
			Signal(p->waiter.task, waiter_sigmask(&p->waiter));
		}
		if (sig & SIGBREAKF_CTRL_C)
			break;
	}
	Forbid();
	Signal(p->waiter.task, SIGBREAKF_CTRL_E);
}

static void flipper_start(struct play *p)
{
	flipper_play = p;
	p->flip_to = -1;
	p->flip_old = -1;
	SetSignal(0, SIGBREAKF_CTRL_E);
	p->flipper = CreateNewProcTags(NP_Entry, (ULONG)flipper_main, NP_Name, (ULONG) "VPUPlay flipper", NP_Priority,
	                               (ULONG)(LONG)(p->waiter.task->tc_Node.ln_Pri - 1), NP_StackSize, 8192, TAG_DONE);
	p->flipper_used = p->flipper != NULL;
	if (p->flipper == NULL)
		note("no flipper process: flipping inline\n");
}

static void flipper_stop(struct play *p)
{
	if (p->flipper == NULL)
		return;
	Signal(&p->flipper->pr_Task, SIGBREAKF_CTRL_C);
	Wait(SIGBREAKF_CTRL_E);
	p->flipper = NULL;
}

/* Ask for part @k on display, taking @old off it. */
static void flip_request(struct play *p, LONG k, LONG old)
{
	p->flip_old = old;
	if (p->flipper == NULL)
	{
		show_part(p, k);
		return;
	}
	p->flip_to = k;
	Signal(&p->flipper->pr_Task, SIGBREAKF_CTRL_F);
}

/* A flip done: the part it took off the display cools from then on. */
static void flip_settle(struct play *p)
{
	if (p->flip_old >= 0 && p->flip_to < 0)
	{
		p->cooling[p->flip_old] = TRUE;
		p->free_at[p->flip_old] = p->flip_done_at + COOL_US;
		p->flip_old = -1;
	}
}

static BOOL screen_setup(struct play *p, ULONG depth, ULONG want_w, ULONG want_h, struct target *t)
{
	ULONG w = want_w != 0 ? want_w : p->video_w, h = want_h != 0 ? want_h : p->video_h;
	char names[128];
	ULONG allowed = pixel_formats_of_depth(depth) & p->isp_formats;
	if (allowed == 0)
	{
		kv_str("fail", "the backend writes no Picasso96 format of this depth: another DEPTH");
		note("the backend writes %s\n", format_names(p->isp_formats, names, sizeof(names)));
		return FALSE;
	}
	ULONG id = p96BestModeIDTags(P96BIDTAG_NominalWidth, w, P96BIDTAG_NominalHeight, h, P96BIDTAG_Depth, depth,
	                             P96BIDTAG_FormatsAllowed, allowed, TAG_DONE);
	if (id == 0 || id == (ULONG)INVALID_ID)
	{
		kv_str("fail", "no RTG screen mode of this size and depth in a format the backend writes: Picasso96Mode, add one");
		note("wanted %lux%lu at %lu bits in %s\n", w, h, depth, format_names(allowed, names, sizeof(names)));
		return FALSE;
	}
	p->screen_w = p96GetModeIDAttr(id, P96IDA_WIDTH);
	p->screen_h = p96GetModeIDAttr(id, P96IDA_HEIGHT);
	ULONG mode_depth = p96GetModeIDAttr(id, P96IDA_DEPTH);
	kv_hex("screen.mode", id);
	kv_unum("screen.width", p->screen_w);
	kv_unum("screen.height", p->screen_h);
	kv_unum("screen.depth", mode_depth);
	kv_unum("screen.format", p96GetModeIDAttr(id, P96IDA_RGBFORMAT));

	/* as tall as the VRAM allows: three picture heights, else two, else one */
	for (p->parts = MAX_PARTS; p->parts >= 1 && p->screen == NULL; p->parts--)
	{
		p->screen = p96OpenScreenTags(P96SA_DisplayID, id, P96SA_Width, p->screen_w, P96SA_Height, p->screen_h * p->parts,
		                              P96SA_Depth, mode_depth, P96SA_Quiet, TRUE, P96SA_ShowTitle, FALSE, P96SA_AutoScroll,
		                              FALSE, P96SA_Title, (ULONG) "VPUPlay", TAG_DONE);
		if (p->screen != NULL)
			break;
	}
	if (p->screen == NULL)
	{
		kv_str("fail", "the screen did not open, not even one picture high");
		note("%lu MiB of VRAM for one picture at %lu bits\n",
		     (p->screen_w * p->screen_h * ((mode_depth + 7u) / 8u) + (1u << 20) - 1u) >> 20, mode_depth);
		return FALSE;
	}
	kv_unum("screen.parts", p->parts);
	note("screen open: %lu picture heights, %ldx%ld\n", p->parts, (LONG)p->screen->Width, (LONG)p->screen->Height);
	p96RectFill(&p->screen->RastPort, 0, 0, (UWORD)(p->screen_w - 1u), (UWORD)(p->screen_h * p->parts - 1u), 0);
	note("filled black\n");

	struct BitMap *bm = p->screen->RastPort.BitMap;
	LONG lock = p96LockBitMap(bm, (UBYTE *)&p->ri, sizeof(p->ri));
	if (lock != 0)
		p96UnlockBitMap(bm, lock);
	kv_hex("screen.bitmap", (ULONG)p->ri.Memory);
	kv_num("screen.pitch", (LONG)p->ri.BytesPerRow);
	kv_unum("screen.bitmap_format", (ULONG)p->ri.RGBFormat);
	kv_bool("screen.vram", p96GetBitMapAttr(bm, P96BMA_ISONBOARD) != 0);
	kv_bool("screen.locked", lock != 0);
	if (lock == 0 || !p96GetBitMapAttr(bm, P96BMA_ISONBOARD))
	{
		kv_str("fail", "the bitmap must be in VRAM for the VPU to write it");
		return FALSE;
	}
	const struct pixel_format *pf = pixel_format_of(p->ri.RGBFormat);
	ULONG pitch = (ULONG)p->ri.BytesPerRow;
	if (pf == NULL || (p->isp_formats & pixel_format_bit(pf)) == 0)
	{
		kv_str("fail", "the bitmap's format is none the backend writes");
		note("the backend writes %s\n", format_names(p->isp_formats, names, sizeof(names)));
		return FALSE;
	}
	if ((pitch / pf->bpp) % 32u != 0 || ((ULONG)p->ri.Memory & 3u) != 0)
	{
		kv_str("fail", "bitmap rows are not a multiple of 32 pixels");
		return FALSE;
	}
	/* the VPU writes a whole part, the picture scaled into it with its aspect kept, centred */
	t->pf = pf;
	t->width = p->screen_w;
	t->height = p->screen_h;
	t->pitch = pitch;
	t->keep_aspect = TRUE;
	t->num = p->parts;
	t->imported = TRUE;
	for (ULONG k = 0; k < p->parts; k++)
		t->payloads[k] = (UBYTE *)p->ri.Memory + k * pitch * p->screen_h;
	t->what = "bitmap part";

	p->window = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)p->screen, WA_Left, 0, WA_Top, 0, WA_Width, p->screen_w, WA_Height,
	                           p->screen_h, WA_Backdrop, TRUE, WA_Borderless, TRUE, WA_Activate, TRUE, WA_RMBTrap, TRUE,
	                           WA_IDCMP, IDCMP_RAWKEY | IDCMP_MOUSEBUTTONS, TAG_DONE);
	if (p->window == NULL)
		note("no window: stop with Ctrl-C\n");
	else
	{
		/* no pointer over the picture: the card would also move it with every flip */
		p->blank_pointer = AllocMem(6 * sizeof(UWORD), MEMF_CHIP | MEMF_CLEAR);
		if (p->blank_pointer != NULL)
			SetPointer(p->window, p->blank_pointer, 1, 16, 0, 0);
	}
	note("window open\n");
	p->shown = 0; /* the top part is on display */
	return TRUE;
}

/* Either mode: the window, the pointer, the screen if one was opened. */
static void display_teardown(struct play *p)
{
	if (p->window != NULL)
	{
		if (p->blank_pointer != NULL)
			ClearPointer(p->window);
		CloseWindow(p->window);
	}
	if (p->blank_pointer != NULL)
		FreeMem(p->blank_pointer, 6 * sizeof(UWORD));
	p->blank_pointer = NULL;
	p->window = NULL;
	if (p->screen != NULL)
		p96CloseScreen(p->screen);
	p->screen = NULL;
}

/* ---- the window ------------------------------------------------------------------ */

static void recycle_part(struct play *p, LONG k);

/*
 * The picture's size and place in the window's inner area, kept to the
 * video's aspect.  Its width is a whole number of 32-pixel groups, so the
 * VPU writes full rows of exactly that width and nothing depends on how it
 * treats a crop narrower than its rows.  FALSE when the window is too small.
 */
static BOOL window_geometry(struct play *p, struct target *t)
{
	struct Window *w = p->window;
	LONG inner_w = w->Width - w->BorderLeft - w->BorderRight;
	LONG inner_h = w->Height - w->BorderTop - w->BorderBottom;
	if (inner_w < 32 || inner_h < 16)
		return FALSE;
	ULONG fit_w = (ULONG)inner_w;
	if (p->video_h * fit_w / p->video_w > (ULONG)inner_h)
		fit_w = p->video_w * (ULONG)inner_h / p->video_h;
	fit_w &= ~31u;
	ULONG fit_h = (p->video_h * fit_w / p->video_w) & ~1u;
	if (fit_w == 0 || fit_h == 0)
		return FALSE;
	p->pic_x = w->BorderLeft + (inner_w - (LONG)fit_w) / 2;
	p->pic_y = w->BorderTop + (inner_h - (LONG)fit_h) / 2;
	t->width = fit_w;
	t->height = fit_h;
	t->pitch = fit_w * t->pf->bpp;
	t->keep_aspect = FALSE; /* exactly the picture's size */
	t->num = MAX_PARTS;
	t->imported = FALSE; /* the session's own frames, in Fast RAM */
	t->what = "frame";
	return TRUE;
}

/* Black around the picture (or everywhere, with no picture yet). */
static void window_fill_bars(struct play *p)
{
	struct Window *w = p->window;
	struct RastPort *rp = w->RPort;
	LONG x0 = w->BorderLeft, y0 = w->BorderTop;
	LONG x1 = w->Width - w->BorderRight - 1, y1 = w->Height - w->BorderBottom - 1;
	if (x1 < x0 || y1 < y0)
		return;
	if (p->shown < 0)
	{
		p96RectFill(rp, (UWORD)x0, (UWORD)y0, (UWORD)x1, (UWORD)y1, 0);
		return;
	}
	LONG px0 = p->pic_x, py0 = p->pic_y;
	LONG px1 = px0 + (LONG)p->target.width - 1, py1 = py0 + (LONG)p->target.height - 1;
	if (py0 > y0)
		p96RectFill(rp, (UWORD)x0, (UWORD)y0, (UWORD)x1, (UWORD)(py0 - 1), 0);
	if (py1 < y1)
		p96RectFill(rp, (UWORD)x0, (UWORD)(py1 + 1), (UWORD)x1, (UWORD)y1, 0);
	if (px0 > x0)
		p96RectFill(rp, (UWORD)x0, (UWORD)py0, (UWORD)(px0 - 1), (UWORD)py1, 0);
	if (px1 < x1)
		p96RectFill(rp, (UWORD)(px1 + 1), (UWORD)py0, (UWORD)x1, (UWORD)py1, 0);
}

/* Frame @k into the window: Picasso96 copies it, clipped by the layers (other windows stay on top). */
static void window_draw(struct play *p, LONG k)
{
	if (k < 0 || p->frame[k] == NULL)
		return;
	const struct target *t = &p->target;
	struct RenderInfo ri;
	ri.Memory = p->frame[k]->Data;
	ri.BytesPerRow = (WORD)t->pitch;
	ri.pad = 0;
	ri.RGBFormat = t->pf->rgb;
	p96WritePixelArray(&ri, 0, 0, p->window->RPort, (UWORD)p->pic_x, (UWORD)p->pic_y, (UWORD)t->width, (UWORD)t->height);
}

/* The frame in buffer @k is due: copied in; the buffer shown before goes back to the session, this one stays for redraws. */
static void window_present(struct play *p, LONG k)
{
	u32 t0 = get_time();
	window_draw(p, k);
	p->t_copy += get_time() - t0;
	p->copies++;
	LONG old = p->shown;
	p->shown = k;
	recycle_part(p, old);
}

/*
 * A window on the default public screen, the video's aspect, half the
 * screen's size unless WIDTH/HEIGHT give its inner size.  The session writes
 * the screen's own pixel format when it can, so the copy is a plain copy.
 */
static BOOL window_setup(struct play *p, ULONG want_w, ULONG want_h, struct target *t)
{
	struct Screen *scr = LockPubScreen(NULL);
	if (scr == NULL)
	{
		kv_str("fail", "no public screen");
		return FALSE;
	}
	struct BitMap *bm = scr->RastPort.BitMap;
	if (!p96GetBitMapAttr(bm, P96BMA_ISP96))
	{
		kv_str("fail", "the public screen is not a Picasso96 screen");
		UnlockPubScreen(NULL, scr);
		return FALSE;
	}
	RGBFTYPE rgb = (RGBFTYPE)p96GetBitMapAttr(bm, P96BMA_RGBFORMAT);
	ULONG bpp = p96GetBitMapAttr(bm, P96BMA_BYTESPERPIXEL);
	const struct pixel_format *pf = pixel_format_of(rgb);
	if (pf == NULL || (p->isp_formats & pixel_format_bit(pf)) == 0)
	{
		/* the backend's format of the same size per pixel, else any it writes: Picasso96 converts while copying */
		for (pf = pixel_formats; pf->name != NULL; pf++)
			if (pf->bpp == bpp && (p->isp_formats & pixel_format_bit(pf)) != 0)
				break;
		if (pf->name == NULL)
			pf = first_format(p->isp_formats);
		if (pf == NULL)
		{
			kv_str("fail", "the backend writes no format Picasso96 knows");
			UnlockPubScreen(NULL, scr);
			return FALSE;
		}
		note("the screen's pixel format %lu is none the backend writes: it writes %s, which Picasso96 converts while copying\n",
		     (ULONG)rgb, pf->name);
	}
	t->pf = pf;

	ULONG box_w = want_w != 0 ? want_w : (ULONG)scr->Width / 2u, box_h = want_h != 0 ? want_h : (ULONG)scr->Height / 2u;
	ULONG inner_w = box_w, inner_h = p->video_h * box_w / p->video_w;
	if (inner_h > box_h)
	{
		inner_h = box_h;
		inner_w = p->video_w * box_h / p->video_h;
	}
	p->window = OpenWindowTags(NULL, WA_PubScreen, (ULONG)scr, WA_Title, (ULONG) "VPUPlay", WA_InnerWidth, inner_w,
	                           WA_InnerHeight, inner_h, WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE,
	                           WA_SizeGadget, TRUE, WA_SizeBBottom, TRUE, WA_SimpleRefresh, TRUE, WA_Activate, TRUE,
	                           WA_AutoAdjust, TRUE, WA_MinWidth, 128, WA_MinHeight, 80, WA_MaxWidth, ~0UL, WA_MaxHeight, ~0UL,
	                           WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_NEWSIZE | IDCMP_REFRESHWINDOW | IDCMP_CHANGEWINDOW | IDCMP_RAWKEY,
	                           TAG_DONE);
	UnlockPubScreen(NULL, scr);
	if (p->window == NULL)
	{
		kv_str("fail", "the window did not open");
		return FALSE;
	}
	p->shown = -1;
	if (!window_geometry(p, t))
	{
		kv_str("fail", "the window is too small for a picture");
		return FALSE;
	}
	window_fill_bars(p);
	kv_num("window.inner_width", (LONG)(p->window->Width - p->window->BorderLeft - p->window->BorderRight));
	kv_num("window.inner_height", (LONG)(p->window->Height - p->window->BorderTop - p->window->BorderBottom));
	kv_num("window.screen_width", (LONG)scr->Width);
	kv_num("window.screen_height", (LONG)scr->Height);
	kv_str("window.format", pf->name);
	return TRUE;
}

/*
 * The window changed size: the session's output takes the new picture size
 * and makes new frames; the decoder runs on, and frames converted for the
 * old size are dropped with the old frames.
 */
static BOOL window_resize(struct play *p)
{
	struct target t = p->target;
	if (!window_geometry(p, &t))
		return TRUE; /* too small for a picture: the old one stays until it is bigger again */
	if (t.width == p->target.width && t.height == p->target.height)
	{
		window_fill_bars(p);
		window_draw(p, p->shown);
		return TRUE;
	}
	p->shown = -1;
	if (!target_apply(p, &t))
		return FALSE;
	window_fill_bars(p);
	return TRUE;
}

/* ---- the loop -------------------------------------------------------------------- */

/*
 * WINDOWTEST: the window moves, grows and shrinks by itself, one step every
 * six seconds of play, as a user's drags would.  Intuition reports each as it
 * reports a drag (IDCMP_CHANGEWINDOW, IDCMP_NEWSIZE), so the same code
 * follows it.  Each box is kept on the screen.
 */
static void window_test_step(struct play *p, u32 now)
{
	static const struct
	{
		WORD dx, dy, dw, dh;
	} steps[] = {{100, 50, 0, 0}, {0, 0, 320, 180}, {0, 0, -480, -270}};

	struct Window *w = p->window;
	if (w == NULL || p->window_test_done >= sizeof(steps) / sizeof(steps[0]) ||
	    now - p->t_start < (p->window_test_done + 1u) * 6000000u)
		return;
	LONG width = w->Width + steps[p->window_test_done].dw, height = w->Height + steps[p->window_test_done].dh;
	LONG left = w->LeftEdge + steps[p->window_test_done].dx, top = w->TopEdge + steps[p->window_test_done].dy;
	if (left + width > w->WScreen->Width)
		left = w->WScreen->Width - width;
	if (top + height > w->WScreen->Height)
		top = w->WScreen->Height - height;
	ChangeWindowBox(w, (LONG)(left < 0 ? 0 : left), (LONG)(top < 0 ? 0 : top), width, height);
	p->window_test_done++;
}

/* A part goes back to the session: never the one shown, nor one still cooling. */
static void recycle_part(struct play *p, LONG k)
{
	if (k >= 0 && k != (p->parts == 1 ? -1 : p->shown) && k != p->flip_old && !p->at_session[k] && !p->cooling[k] && !p->failed &&
	    !p->eos)
		part_release(p, k);
}

/*
 * Parts that left the display a refresh ago are not scanned any more: the
 * VPU may fill them while the frame on display waits out its time, so with
 * three parts it always has the next one to work on.
 */
static void release_cooled(struct play *p, u32 now)
{
	for (LONG k = 0; k < (LONG)p->parts; k++)
		if (p->cooling[k] && (s32)(now - p->free_at[k]) >= 0)
		{
			p->cooling[k] = FALSE;
			recycle_part(p, k);
		}
}

/*
 * A frame the session filled is a picture to show, in order of arrival.  Its
 * part number rides in UserData, given at the import; one of the session's
 * own frames takes the first free part when it first arrives.
 */
static void frame_arrived(struct play *p, struct VCFrame *f)
{
	LONG k = (LONG)(ULONG)f->UserData - 1;
	if (k < 0)
	{
		for (k = 0; k < (LONG)p->parts && p->frame[k] != NULL; k++)
			;
		if (k >= (LONG)p->parts)
		{
			kv_str("fail", "the session returned a frame over no part");
			p->failed = TRUE;
			return;
		}
		p->frame[k] = f;
		f->UserData = (APTR)((ULONG)k + 1u);
	}
	p->at_session[k] = FALSE;
	if (p->verbose)
		note("frame in part %ld, pts %lu, flags %lx\n", k, (ULONG)f->Pts, f->Flags);
	if (p->failed)
	{
		recycle_part(p, k);
		return;
	}
	p->frames++;
	p->ready[p->ready_n++] = k; /* at most parts - 1 can be out at once */
}

static void show_ready(struct play *p, u32 now)
{
	if (p->ready_n == 0 || p->flip_old >= 0) /* one flip at a time */
		return;
	if (p->displayed == 0)
		p->next_due = now; /* the clock starts with the first picture, not with the pipeline's start-up */
	if (!p->fast && (s32)(p->next_due - now) > 0)
		return;
	if (!p->fast && (s32)(now - p->next_due) > (s32)p->frame_us)
		p->late++;

	LONG k = p->ready[0];
	for (ULONG i = 1; i < p->ready_n; i++)
		p->ready[i - 1] = p->ready[i];
	p->ready_n--;
	p->displayed++;
	p->next_due = p->fast ? now : p->next_due + p->frame_us;
	if (p->windowed)
	{
		window_present(p, k);
		return;
	}
	if (p->parts == 1)
	{
		recycle_part(p, 0); /* the frame is on the visible part already: on to the next one */
		return;
	}
	if (p->displayed <= 3 || p->verbose)
		note("frame %lu: showing part %ld\n", p->displayed, k);
	LONG old = p->shown;
	p->shown = k;
	flip_request(p, k, old);
	flip_settle(p); /* at once when flipped inline */
}

static void check_window(struct play *p)
{
	if (p->window == NULL)
		return;
	struct IntuiMessage *im;
	while ((im = (struct IntuiMessage *)GetMsg(p->window->UserPort)) != NULL)
	{
		ULONG cls = im->Class;
		UWORD code = im->Code;
		ReplyMsg((struct Message *)im);
		if (cls == IDCMP_CLOSEWINDOW || cls == IDCMP_MOUSEBUTTONS || (cls == IDCMP_RAWKEY && (code & 0x80) == 0))
			p->stop = TRUE;
		else if (cls == IDCMP_CHANGEWINDOW)
			p->moves++; /* also comes with a resize */
		else if (cls == IDCMP_NEWSIZE)
		{
			p->resizes++;
			if (!window_resize(p))
				p->failed = TRUE;
		}
		else if (cls == IDCMP_REFRESHWINDOW)
		{
			p->refreshes++;
			BeginRefresh(p->window);
			window_fill_bars(p);
			window_draw(p, p->shown);
			EndRefresh(p->window, TRUE);
		}
	}
}

/**
 * stream_facts() - The stream's shape as stream.N.*, one group per StreamInfo event.
 * @p:  the player
 * @si: the event's stream
 */
static void stream_facts(struct play *p, const struct VCStreamInfo *si)
{
	kv_num(kv_at("stream", p->streams, "width"), si->Crop.Width);
	kv_num(kv_at("stream", p->streams, "height"), si->Crop.Height);
	kv_unum(kv_at("stream", p->streams, "mem_width"), si->Width);
	kv_unum(kv_at("stream", p->streams, "mem_height"), si->Height);
	kv_num(kv_at("stream", p->streams, "fps_num"), si->FrameRate.Num);
	kv_num(kv_at("stream", p->streams, "fps_den"), si->FrameRate.Den);
	p->streams++;
}

/*
 * The decoder told the stream's shape: the display opens for it, the
 * session's output takes the display's geometry, and the parts go to work.
 * Everything before the next poll, as the library asks.
 */
static BOOL display_setup(struct play *p, const struct VCStreamInfo *si, ULONG depth, ULONG want_w, ULONG want_h)
{
	p->video_w = (ULONG)si->Crop.Width;
	p->video_h = (ULONG)si->Crop.Height;
	if (p->fps == 0 && si->FrameRate.Den != 0)
		p->fps = (ULONG)(si->FrameRate.Num / si->FrameRate.Den);
	stream_facts(p, si);
	if (p->video_w == 0 || p->video_h == 0)
	{
		kv_str("fail", "no picture size");
		return FALSE;
	}
	if (p->fps == 0)
		p->fps = 30;
	p->frame_us = 1000000u / p->fps;

	note("decoder told the size; opening the display\n");
	struct target t;
	for (ULONG i = 0; i < sizeof(t); i++)
		((UBYTE *)&t)[i] = 0;
	if (!(p->windowed ? window_setup(p, want_w, want_h, &t) : screen_setup(p, depth, want_w, want_h, &t)))
		return FALSE;
	note("display ready; setting the output geometry\n");
	if (!target_apply(p, &t))
		return FALSE;
	/* full screen: the top part is on display, the others take the first frames (with one part, that one); a window: the session's own */
	if (t.imported)
		for (LONG k = p->parts == 1 ? 0 : 1; k < (LONG)p->parts; k++)
			part_release(p, k);
	kv_unum("play.fps_set", p->fps);
	kv_bool("play.fast", p->fast);
	note("playing at %lu fps%s, %lu %s; %s stops\n", p->fps, p->fast ? " (as fast as it goes)" : "", p->parts,
	     p->windowed ? "frames" : "bitmap parts", p->windowed ? "the close gadget or any key" : "any key or mouse button");
	if (!p->windowed)
		flipper_start(p);
	p->display_ready = TRUE;
	return TRUE;
}

static int play(struct play *p, ULONG depth, ULONG want_w, ULONG want_h)
{
	if (depth == 0)
		depth = workbench_depth(); /* the one the user has modes for */
	if (depth == 0)
		depth = 16;
	if (!query_formats(p))
		return RETURN_FAIL;
	/* a provisional output format of the depth wanted: the screen mode may pick another of its depth */
	const struct pixel_format *pf0 = first_format(pixel_formats_of_depth(depth) & p->isp_formats);
	if (pf0 == NULL)
		pf0 = first_format(p->isp_formats);
	LONG err = 0;
	p->s = VC_OpenSessionTags(VCA_Codec, VCC_H264, VCA_Kind, VCK_Decoder, VCA_OutputFormat, pf0->vcf, VCA_OutputCount, (ULONG)MAX_PARTS,
	                          VCA_SigTask, (ULONG)p->waiter.task, VCA_SigMask, waiter_sigmask(&p->waiter), VCA_InputCount, 4, VCA_ErrorPtr,
	                          (ULONG)&err, TAG_DONE);
	if (p->s == NULL)
	{
		kv_num("error", err);
		kv_str("fail", "no decoder session (see the debug log)");
		return RETURN_FAIL;
	}

	p->t_start = get_time();
	p->next_due = p->t_start;
	u32 t_idle = p->t_start;
	ULONG last_count = 0;
	BOOL eos_marked = TRUE;
	for (;;)
	{
		struct VCItem it;
		ULONG taken = 0;
		while (taken < POLL_BATCH && !p->failed && !p->stop && VC_Poll(p->s, &it))
		{
			taken++;
			switch (it.Type)
			{
			case VCIT_PacketReturned:
				fill_input(p, it.u.Packet);
				break;
			case VCIT_Frame:
				frame_arrived(p, it.u.Frame);
				break;
			case VCIT_Event:
				if (it.u.Event.Event == VCEV_StreamInfo)
				{
					if (p->display_ready)
					{
						/* the picture changed shape; the output geometry is fixed, so the frames stay */
						p->video_w = (ULONG)it.u.Event.Stream.Crop.Width;
						p->video_h = (ULONG)it.u.Event.Stream.Crop.Height;
						stream_facts(p, &it.u.Event.Stream);
					}
					else if (!display_setup(p, &it.u.Event.Stream, depth, want_w, want_h))
					{
						p->failed = TRUE;
						return RETURN_ERROR;
					}
					else
					{
						p->t_start = get_time();
						t_idle = p->t_start;
					}
				}
				else if (it.u.Event.Event == VCEV_EndOfStream)
					p->eos = TRUE;
				else if (it.u.Event.Event == VCEV_Error)
				{
					kv_num("session.error", it.u.Event.Status);
					kv_str("fail", "the session reported an error");
					p->failed = TRUE;
				}
				break;
			default:
				break;
			}
		}
		u32 now = get_time();
		if (p->display_ready)
		{
			flip_settle(p);
			show_ready(p, now);
			release_cooled(p, now);
			check_window(p);
			if (p->window_test)
				window_test_step(p, now);
		}
		if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) /* also between waits: a Ctrl-C must always get through */
			p->stop = TRUE;
		if (p->failed || p->stop)
			break;
		if (p->displayed >= p->max_frames)
			break;
		if (p->eos && p->ready_n == 0)
		{
			if (!p->display_ready)
			{
				kv_bool("play.no_picture", TRUE);
				kv_str("fail", "the stream ended without a picture");
			}
			break;
		}
		ULONG progress = p->frames + p->displayed + (p->display_ready ? 1u : 0u);
		if (progress != last_count)
		{
			last_count = progress;
			t_idle = now;
		}
		else if (now - t_idle > IDLE_MS * 1000u)
		{
			if (!p->display_ready)
			{
				kv_bool("play.idle", TRUE);
				kv_str("fail", "no picture size from the decoder (not an H.264 stream it takes: an .mp4, not .h264?)");
				p->failed = TRUE;
				break;
			}
			if (p->eof_sent && p->ready_n == 0)
			{
				eos_marked = FALSE; /* the stream is through, only its end mark never came */
				break;
			}
			kv_bool("play.idle", TRUE);
			kv_str("fail", "nothing happened");
			note("nothing happened for %lu ms: frames %lu, displayed %lu, ready %lu, in VPU %lu/%lu/%lu\n", (ULONG)IDLE_MS, p->frames,
			     p->displayed, p->ready_n, (ULONG)p->at_session[0], (ULONG)p->at_session[1], (ULONG)p->at_session[2]);
			p->failed = TRUE;
			break;
		}
		if (taken == POLL_BATCH)
			continue; /* more may be waiting: poll again before sleeping */
		/* wake for the next due frame or the next part to cool down, else in a second */
		u32 timeout_us = 1000000u;
		if (p->ready_n != 0 && !p->fast && (s32)(p->next_due - now) > 0 && p->next_due - now < timeout_us)
			timeout_us = p->next_due - now;
		for (ULONG k = 0; k < p->parts; k++)
			if (p->cooling[k] && (s32)(p->free_at[k] - now) > 0 && p->free_at[k] - now < timeout_us)
				timeout_us = p->free_at[k] - now;
		BOOL timed_out;
		if (!wait_any(p, (timeout_us + 999u) / 1000u, &timed_out))
		{
			p->stop = TRUE;
			break;
		}
	}
	u32 total = get_time() - p->t_start;
	flipper_stop(p);
	if (!p->display_ready)
	{
		if (!p->failed && p->stop)
			kv_str("fail", "stopped before a picture");
		return p->failed ? RETURN_FAIL : RETURN_WARN;
	}
	if (!eos_marked)
		total -= IDLE_MS * 1000u;
	ULONG ms = total / 1000u;
	kv_unum("play.shown", p->displayed);
	kv_unum("play.frames", p->frames);
	kv_unum("play.late", p->late);
	kv_unum("play.ms", ms);
	kv_fixed1("play.fps", p->displayed * 1000u, ms);
	kv_unum("play.in_bytes", p->in_bytes);
	kv_unum("play.parts", p->parts);
	kv_bool("play.eos", eos_marked);
	kv_unum("play.loops", p->loops);
	kv_unum("play.busy_pct", total != 0 ? (ULONG)(((u64)(total - p->t_wait)) * 100u / total) : 0);
	kv_unum("play.read_ms", p->t_read / 1000u);
	if (p->windowed)
	{
		kv_unum("play.copy_ms", p->t_copy / 1000u);
		kv_unum("play.copy_us", p->copies != 0 ? p->t_copy / p->copies : 0);
		kv_unum("window.moves", p->moves);
		kv_unum("window.resizes", p->resizes);
		kv_unum("window.refreshes", p->refreshes);
	}
	else
	{
		kv_unum("play.flip_ms", p->t_flip / 1000u);
		kv_unum("play.flip_us", p->displayed != 0 ? p->t_flip / p->displayed : 0);
		kv_bool("play.flipper", p->flipper_used);
	}
	if (p->stop && p->displayed == 0)
		kv_str("fail", "stopped before a picture");
	return p->failed ? RETURN_FAIL : p->stop && p->displayed == 0 ? RETURN_WARN : RETURN_OK;
}

/* ---- main ---------------------------------------------------------------------- */

enum
{
	ARG_FILE,
	ARG_FPS,
	ARG_FAST,
	ARG_FRAMES,
	ARG_DEPTH,
	ARG_WIDTH,
	ARG_HEIGHT,
	ARG_VERBOSE,
	ARG_WINDOW,
	ARG_LOOP,
	ARG_WINDOWTEST,
	ARG_TRACE,
	ARG_COUNT
};

int main(void)
{
	LONG args[ARG_COUNT];
	for (ULONG i = 0; i < ARG_COUNT; i++)
		args[i] = 0;
	struct RDArgs *rda =
	    ReadArgs((CONST_STRPTR) "FILE/A,FPS/N,FAST/S,FRAMES/N,DEPTH/N,WIDTH/N,HEIGHT/N,VERBOSE/S,WINDOW/S,LOOP/S,WINDOWTEST/S,TRACE/S", args,
	             NULL);
	if (rda == NULL)
	{
		PrintFault(IoErr(), (CONST_STRPTR) "VPUPlay");
		return RETURN_FAIL;
	}
	kv_init("VPUPlay", args[ARG_TRACE] != 0);
	kv_num("cli", ((struct Process *)FindTask(NULL))->pr_TaskNum);
	kv_str("play.file", (const char *)args[ARG_FILE]);
	kv_begin((const char *)GetArgStr());

	int rc = RETURN_ERROR;
	static struct play p;
	for (ULONG i = 0; i < sizeof(p); i++)
		((UBYTE *)&p)[i] = 0;
	p.shown = -1;
	p.flip_old = -1;
	p.waiter.sig = -1;

	IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR) "intuition.library", 39);
	GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR) "graphics.library", 39);
	P96Base = OpenLibrary((CONST_STRPTR)P96NAME, 2);
	VideoCodecBase = OpenLibrary((CONST_STRPTR)VIDEOCODEC_NAME, 0);
	if (IntuitionBase == NULL || GfxBase == NULL || P96Base == NULL || VideoCodecBase == NULL)
	{
		kv_str("fail", P96Base == NULL ? "Picasso96API.library is missing" : VideoCodecBase == NULL ? "videocodec.library is missing"
		                                                                                              : "intuition or graphics is missing");
		goto out;
	}
	kv_version("videocodec.version", VideoCodecBase->lib_Version, VideoCodecBase->lib_Revision);

	if (!waiter_open(&p.waiter))
	{
		kv_str("fail", "no signal or timer");
		goto out;
	}
	p.verbose = args[ARG_VERBOSE] != 0;
	p.fast = args[ARG_FAST] != 0;
	p.windowed = args[ARG_WINDOW] != 0;
	p.loop = args[ARG_LOOP] != 0;
	p.window_test = args[ARG_WINDOWTEST] != 0;
	p.fps = arg_num(args, ARG_FPS, 0);
	p.max_frames = arg_num(args, ARG_FRAMES, 0);
	if (p.max_frames == 0)
		p.max_frames = 0xffffffffu;
	p.file = Open((CONST_STRPTR)args[ARG_FILE], MODE_OLDFILE);
	if (p.file == 0)
	{
		PrintFault(IoErr(), (CONST_STRPTR)args[ARG_FILE]);
		kv_str("fail", "the stream did not open");
		goto out;
	}

	u32 run_start = get_time();
	rc = play(&p, arg_num(args, ARG_DEPTH, 0), arg_num(args, ARG_WIDTH, 0), arg_num(args, ARG_HEIGHT, 0));
	kv_unum("play.total_us", get_time() - run_start);

	/* teardown: the session first (its DMA into the bitmap stops), then the screen */
	if (p.s != NULL)
		VC_CloseSession(p.s);
	p.s = NULL;
	display_teardown(&p);

out:
	kv_verdict(rc);
	kv_end(rc);
	if (p.file != 0)
		Close(p.file);
	waiter_close(&p.waiter);
	if (VideoCodecBase != NULL)
		CloseLibrary(VideoCodecBase);
	if (P96Base != NULL)
		CloseLibrary(P96Base);
	if (GfxBase != NULL)
		CloseLibrary((struct Library *)GfxBase);
	if (IntuitionBase != NULL)
		CloseLibrary((struct Library *)IntuitionBase);
	FreeArgs(rda);
	return rc;
}
