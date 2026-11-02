/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * VCShow - the smallest picture viewer on videocodec.library: a JPEG, PNG,
 * GIF, BMP or PPM in a window on the Workbench.  The example of the image
 * calls, to copy from.
 *
 *   VCShow FILE/A
 *
 * The file is read into memory, and VC_GetImageInfo() reads the picture's
 * header: its size, without decoding it.  A window opens at that size
 * (scaled down to fit the screen), and one VC_DecodeImageTags() call has the
 * VideoCore decode the picture, scale it to the window and convert it to the
 * screen's pixel format, into a buffer of ours; p96WritePixelArray() copies
 * it into the window.
 *
 * What the hardware does not decode right (a progressive JPEG, a 16-bit
 * PNG, ...) is refused with VCERR_UNSUPPORTED before the hardware sees it:
 * a viewer decodes those pictures itself (datatypes.library, a decoder of
 * its own).  Here that fallback is a message.  A TGA has no signature: a
 * viewer that knows the file is one sets VCImageInfo.Codec and VCA_Codec to
 * VCC_TGA.
 *
 * The close gadget or Ctrl-C ends it.
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/Picasso96.h>
#include <proto/videocodec.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/Picasso96.h>
#include <libraries/videocodec.h>

struct Library *VideoCodecBase;
struct Library *P96Base;
struct IntuitionBase *IntuitionBase;

unsigned long __stack = 16384; /* a window and Picasso96 want more than a shell's default */

#ifdef VERSTAG /* "$VER: VCShow x.y (date)", given by the build */
static const char verstag[] __attribute__((used)) = VERSTAG;
#endif

/* The RGB layouts the backend may write, by their Picasso96 names, and their bytes per pixel. */
static const struct pixel_format
{
	RGBFTYPE rgb;
	ULONG vcf;
	ULONG bpp;
} formats[] = {
    {RGBFB_R5G6B5PC, VCF_RGB16, 2},  {RGBFB_R8G8B8, VCF_RGB24, 3},   {RGBFB_B8G8R8, VCF_BGR24, 3},
    {RGBFB_B8G8R8A8, VCF_BGRA32, 4}, {RGBFB_R8G8B8A8, VCF_RGBA32, 4},
};
#define FORMATS_NUM (sizeof(formats) / sizeof(formats[0]))

/* The screen's own pixel format when the codec's decoder writes it (a plain copy then), else the first one it writes. */
static const struct pixel_format *format_choose(struct Screen *scr, ULONG codec)
{
	RGBFTYPE screen_rgb = (RGBFTYPE)p96GetBitMapAttr(scr->RastPort.BitMap, P96BMA_RGBFORMAT);
	const struct pixel_format *pf = NULL;
	for (ULONG i = 0;; i++)
	{
		struct VCCodecInfo ci;
		ci.Size = sizeof(ci);
		if (VC_QueryCodec(i, &ci) != 0)
			break;
		if (ci.Codec != codec || ci.Kind != VCK_Decoder || (ci.Flags & VCCF_Image) == 0)
			continue;
		for (ULONG f = 0; f < FORMATS_NUM; f++)
			if ((ci.OutputFormats & VCF_MASK(formats[f].vcf)) && (pf == NULL || formats[f].rgb == screen_rgb))
				pf = &formats[f];
		if (pf != NULL)
			break;
	}
	return pf;
}

/* The whole file in memory, NULL with the cause printed. */
static UBYTE *file_load(const char *name, ULONG *length)
{
	BPTR fh = Open((CONST_STRPTR)name, MODE_OLDFILE);
	if (fh == 0)
	{
		PrintFault(IoErr(), (CONST_STRPTR)name);
		return NULL;
	}
	Seek(fh, 0, OFFSET_END);
	LONG size = Seek(fh, 0, OFFSET_BEGINNING);
	UBYTE *data = size > 0 ? AllocVec((ULONG)size, MEMF_ANY) : NULL;
	if (size <= 0)
		Printf((CONST_STRPTR) "%s: empty, or its size unknown\n", name);
	else if (data == NULL)
		Printf((CONST_STRPTR) "%s: no memory for %ld bytes\n", name, size);
	else if (Read(fh, data, size) != size)
	{
		PrintFault(IoErr(), (CONST_STRPTR)name);
		FreeVec(data);
		data = NULL;
	}
	Close(fh);
	*length = (ULONG)size;
	return data;
}

/* The picture's size, or smaller with the aspect kept when the screen is smaller; rows of whole 32 pixels, as the backend asks. */
static void window_size(struct Screen *scr, const struct VCImageInfo *info, ULONG *w, ULONG *h)
{
	ULONG room_w = (ULONG)scr->Width - 16u, room_h = (ULONG)scr->Height - (ULONG)scr->BarHeight - 32u;
	*w = info->Width;
	*h = info->Height;
	if (*w > room_w)
	{
		*h = *h * room_w / *w;
		*w = room_w;
	}
	if (*h > room_h)
	{
		*w = *w * room_h / *h;
		*h = room_h;
	}
	*w &= ~31u;
	*h &= ~1u;
}

static int show(const char *name, const UBYTE *data, ULONG length)
{
	struct VCImageInfo info;
	info.Size = sizeof(info);
	info.Codec = VCC_None; /* tell it from the bytes */
	if (VC_GetImageInfo((APTR)data, length, &info) != 0)
	{
		Printf((CONST_STRPTR) "%s: not a picture videocodec.library knows\n", name);
		return RETURN_FAIL;
	}

	struct Screen *scr = LockPubScreen(NULL);
	if (scr == NULL)
		return RETURN_FAIL;
	const struct pixel_format *pf = format_choose(scr, info.Codec);
	ULONG w, h;
	window_size(scr, &info, &w, &h);
	struct Window *window = NULL;
	if (pf != NULL && w != 0 && h != 0)
		window = OpenWindowTags(NULL, WA_PubScreen, (ULONG)scr, WA_Title, (ULONG)name, WA_InnerWidth, w, WA_InnerHeight, h, WA_DragBar,
		                        TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_SmartRefresh, TRUE, WA_Activate, TRUE, WA_IDCMP,
		                        IDCMP_CLOSEWINDOW, TAG_DONE);
	UnlockPubScreen(NULL, scr);
	if (pf == NULL)
	{
		Printf((CONST_STRPTR) "%s: no codec driver decodes it to an RGB format Picasso96 knows\n", name);
		return RETURN_FAIL;
	}
	if (window == NULL)
	{
		Printf((CONST_STRPTR) "%s: no window for %ldx%ld\n", name, w, h);
		return RETURN_FAIL;
	}

	/* Fast RAM: the VideoCore writes the picture by DMA; cleared, so what the picture does not cover is black */
	UBYTE *pixels = AllocVec(w * h * pf->bpp, MEMF_PUBLIC | MEMF_FAST | MEMF_CLEAR);
	LONG err = pixels != NULL ? VC_DecodeImageTags(VCA_Data, (ULONG)data, VCA_Length, length, VCA_Codec, info.Codec, VCA_Dest, (ULONG)pixels,
	                                               VCA_OutputFormat, pf->vcf, VCA_OutputWidth, w, VCA_OutputHeight, h,
	                                               VCA_KeepAspect, TRUE, TAG_DONE)
	                          : VCERR_NOMEM;
	int rc = RETURN_OK;
	if (err == 0)
	{
		Printf((CONST_STRPTR) "%s: %ldx%ld, in a %ldx%ld window\n", name, info.Width, info.Height, w, h);
		struct RenderInfo ri;
		ri.Memory = pixels;
		ri.BytesPerRow = (WORD)(w * pf->bpp);
		ri.pad = 0;
		ri.RGBFormat = pf->rgb;
		p96WritePixelArray(&ri, 0, 0, window->RPort, (UWORD)window->BorderLeft, (UWORD)window->BorderTop, (UWORD)w, (UWORD)h);
		BOOL done = FALSE;
		while (!done)
		{
			ULONG sig = Wait((1UL << window->UserPort->mp_SigBit) | SIGBREAKF_CTRL_C);
			struct IntuiMessage *im;
			while ((im = (struct IntuiMessage *)GetMsg(window->UserPort)) != NULL)
			{
				if (im->Class == IDCMP_CLOSEWINDOW)
					done = TRUE;
				ReplyMsg((struct Message *)im);
			}
			if (sig & SIGBREAKF_CTRL_C)
				done = TRUE;
		}
	}
	else if (err == VCERR_UNSUPPORTED)
	{
		/* the fallback: a real viewer decodes the picture itself here */
		Printf((CONST_STRPTR) "%s: the VideoCore does not decode this picture: a viewer decodes it itself\n", name);
		rc = RETURN_WARN;
	}
	else
	{
		Printf((CONST_STRPTR) "%s: decode error %ld\n", name, err);
		rc = RETURN_FAIL;
	}
	if (pixels != NULL)
		FreeVec(pixels);
	CloseWindow(window);
	return rc;
}

int main(void)
{
	LONG args[1] = {0};
	struct RDArgs *rda = ReadArgs((CONST_STRPTR) "FILE/A", args, NULL);
	if (rda == NULL)
	{
		PrintFault(IoErr(), (CONST_STRPTR) "VCShow");
		return RETURN_FAIL;
	}

	int rc = RETURN_ERROR;
	IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR) "intuition.library", 39);
	P96Base = OpenLibrary((CONST_STRPTR)P96NAME, 2);
	VideoCodecBase = OpenLibrary((CONST_STRPTR)VIDEOCODEC_NAME, 0);
	if (IntuitionBase == NULL || P96Base == NULL || VideoCodecBase == NULL)
		Printf((CONST_STRPTR) "missing: %s%s%s\n", IntuitionBase == NULL ? "intuition.library " : "", P96Base == NULL ? "Picasso96API.library " : "",
		       VideoCodecBase == NULL ? "videocodec.library" : "");
	else
	{
		ULONG length;
		UBYTE *data = file_load((const char *)args[0], &length);
		if (data != NULL)
		{
			rc = show((const char *)args[0], data, length);
			FreeVec(data);
		}
	}

	if (VideoCodecBase != NULL)
		CloseLibrary(VideoCodecBase);
	if (P96Base != NULL)
		CloseLibrary(P96Base);
	if (IntuitionBase != NULL)
		CloseLibrary((struct Library *)IntuitionBase);
	FreeArgs(rda);
	return rc;
}
