/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * screen.c - SCREEN: a Picasso96 screen to record, until SECONDS or Ctrl-C.
 * The colour bars of the suite's pattern() (harness/pictures.py) over the
 * grey ramp, and a white block crossing the ramp, so that every picture
 * differs from the last and the bars can be judged.  NOISE is the opposite,
 * the worst an encoder and a disk can be asked: random bytes everywhere, a
 * band of them new at every step.  BACK n sends the screen behind the others
 * after n seconds, and returns it three seconds later.  NATIVE and DEPTH 8
 * are the screens VCGrab must refuse: open, in front, nothing drawn.
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/intuition_protos.h>
#include <clib/Picasso96_protos.h>
#else
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/Picasso96.h>
#endif

#include <dos/dos.h>
#include <graphics/displayinfo.h>
#include <graphics/modeid.h>
#include <graphics/rastport.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/Picasso96.h>

#include <args.h>
#include <kv.h>
#include <pixel_format.h>
#include <timing.h>

#include "commands.h"

struct Library *P96Base;
struct IntuitionBase *IntuitionBase;

const char screen_template[] = "WIDTH/N/A,HEIGHT/N/A,DEPTH/N,SECONDS/N,NOISE/S,BACK/N,NATIVE/S";

enum
{
	ARG_WIDTH,
	ARG_HEIGHT,
	ARG_DEPTH,
	ARG_SECONDS,
	ARG_NOISE,
	ARG_BACK,
	ARG_NATIVE
};

#define BLOCK_SIZE 64u
#define BLOCK_STEP 16u
#define STEP_MS 33u
#define BACK_SECONDS 3u
#define NOISE_BANDS 8u /* one of them new at a step */
#define NATIVE_DEPTH 3u

/* pattern()'s BAR_COLOURS, as 0xRRGGBB */
static const ULONG bar_colours[] = {0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x101010};
#define BARS (sizeof(bar_colours) / sizeof(bar_colours[0]))

/* The ramp's columns x0 to x1 - 1, rows y0 to y1 - 1. */
static void ramp_draw(struct RastPort *rp, ULONG width, ULONG x0, ULONG x1, ULONG y0, ULONG y1)
{
	for (ULONG x = x0; x < x1; x++)
	{
		ULONG grey = x * 255u / (width - 1u);
		p96RectFill(rp, (UWORD)x, (UWORD)y0, (UWORD)x, (UWORD)(y1 - 1u), (grey << 16) | (grey << 8) | grey);
	}
}

/* The bars and the ramp, as pattern() lays them out. */
static void pattern_draw(struct RastPort *rp, ULONG width, ULONG height, ULONG bar_h)
{
	ULONG bar_w = width / BARS / 16u * 16u;
	for (ULONG i = 0; i < BARS; i++)
		p96RectFill(rp, (UWORD)(i * bar_w), 0, (UWORD)((i + 1u) * bar_w - 1u), (UWORD)(bar_h - 1u), bar_colours[i]);
	if (BARS * bar_w < width)
		p96RectFill(rp, (UWORD)(BARS * bar_w), 0, (UWORD)(width - 1u), (UWORD)(bar_h - 1u), bar_colours[BARS - 1u]);
	ramp_draw(rp, width, 0, width, bar_h, height);
}

/* Rows @top to @end - 1 of the bitmap, random bytes: straight into it, a word at a time. */
static void noise_draw(struct Screen *screen, ULONG top, ULONG end, ULONG width, ULONG *seed)
{
	struct BitMap *bm = screen->RastPort.BitMap;
	struct RenderInfo ri;
	LONG lock = p96LockBitMap(bm, (UBYTE *)&ri, sizeof(ri));
	if (lock == 0)
		return;
	ULONG x = *seed;
	ULONG words = width * (ULONG)p96GetBitMapAttr(bm, P96BMA_BYTESPERPIXEL) / 4u;
	for (ULONG row = top; row < end; row++)
	{
		ULONG *at = (ULONG *)((UBYTE *)ri.Memory + row * (ULONG)ri.BytesPerRow);
		for (ULONG i = 0; i < words; i++)
		{
			x ^= x << 13;
			x ^= x >> 17;
			x ^= x << 5;
			at[i] = x;
		}
	}
	p96UnlockBitMap(bm, lock);
	*seed = x;
}

/* Up and showing: the block or the noise moves on until @seconds or Ctrl-C, the screen stepping back on the way if asked. */
static void screen_run(struct tool *t, struct Screen *screen, ULONG height, ULONG bar_h, ULONG seconds, BOOL noise, ULONG back)
{
	struct RastPort *rp = &screen->RastPort;
	ULONG width = (ULONG)screen->Width;
	ULONG top = bar_h + 8u, x = 0, band = 0, seed = 0x2545F491u;
	u32 start = get_time();
	ULONG steps = 0;
	BOOL behind = FALSE, returned = FALSE;
	for (;;)
	{
		ULONG elapsed = (get_time() - start) / 1000000u;
		if (seconds != 0 && elapsed >= seconds)
			break;
		if (back != 0 && !behind && elapsed >= back)
		{
			ScreenToBack(screen);
			behind = TRUE;
			note("behind the others\n");
		}
		if (behind && !returned && elapsed >= back + BACK_SECONDS)
		{
			ScreenToFront(screen);
			returned = TRUE;
			note("in front again\n");
		}
		if (noise)
		{
			noise_draw(screen, band * height / NOISE_BANDS, (band + 1u) * height / NOISE_BANDS, width, &seed);
			band = (band + 1u) % NOISE_BANDS;
		}
		else
		{
			ULONG next = x + BLOCK_STEP + BLOCK_SIZE <= width ? x + BLOCK_STEP : 0;
			ramp_draw(rp, width, x, x + BLOCK_SIZE, top, top + BLOCK_SIZE);
			p96RectFill(rp, (UWORD)next, (UWORD)top, (UWORD)(next + BLOCK_SIZE - 1u), (UWORD)(top + BLOCK_SIZE - 1u), 0xFFFFFF);
			x = next;
		}
		steps++;
		if (waiter_wait(&t->waiter, 0, STEP_MS) & SIGBREAKF_CTRL_C)
			break;
	}
	kv_unum("screen.steps", steps);
}

/* A screen with nothing to draw: until @seconds or Ctrl-C. */
static void plain_wait(struct tool *t, ULONG seconds)
{
	u32 start = get_time();
	while (seconds == 0 || (get_time() - start) / 1000000u < seconds)
		if (waiter_wait(&t->waiter, 0, STEP_MS) & SIGBREAKF_CTRL_C)
			break;
}

/**
 * do_screen() - SCREEN: open it, draw, keep the block moving, close it.
 * @t:    the tool
 * @args: screen_template's
 *
 * Return: RETURN_OK, RETURN_ERROR for a library or a mode missing.
 */
int do_screen(struct tool *t, LONG *args)
{
	ULONG width = arg_num(args, ARG_WIDTH, 0), height = arg_num(args, ARG_HEIGHT, 0);
	ULONG depth = arg_num(args, ARG_DEPTH, 16u), seconds = arg_num(args, ARG_SECONDS, 10u), back = arg_num(args, ARG_BACK, 0);
	BOOL noise = args[ARG_NOISE] != 0;
	int rc = RETURN_ERROR;
	IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR) "intuition.library", 39);
	P96Base = OpenLibrary((CONST_STRPTR)P96NAME, 2);
	if (IntuitionBase == NULL || P96Base == NULL)
	{
		kv_str("fail", "intuition or Picasso96API.library is missing");
		goto out;
	}
	if (args[ARG_NATIVE] != 0)
	{
		struct Screen *native = OpenScreenTags(NULL, SA_Width, width, SA_Height, height, SA_Depth, NATIVE_DEPTH, SA_DisplayID, HIRES_KEY,
		                                       SA_Quiet, TRUE, SA_ShowTitle, FALSE, TAG_DONE);
		if (native == NULL)
		{
			kv_str("fail", "the screen did not open");
			goto out;
		}
		plain_wait(t, seconds);
		CloseScreen(native);
		rc = RETURN_OK;
		goto out;
	}
	ULONG id = p96BestModeIDTags(P96BIDTAG_NominalWidth, width, P96BIDTAG_NominalHeight, height, P96BIDTAG_Depth, depth,
	                             P96BIDTAG_FormatsAllowed, depth == 8u ? RGBFF_CLUT : pixel_formats_of_depth(depth), TAG_DONE);
	if (id == 0 || id == (ULONG)INVALID_ID)
	{
		kv_str("fail", "no RTG screen mode of this size and depth");
		goto out;
	}
	ULONG mode_depth = p96GetModeIDAttr(id, P96IDA_DEPTH);
	struct Screen *screen = p96OpenScreenTags(P96SA_DisplayID, id, P96SA_Width, width, P96SA_Height, height, P96SA_Depth, mode_depth,
	                                          P96SA_Quiet, TRUE, P96SA_ShowTitle, FALSE, P96SA_AutoScroll, FALSE, P96SA_Title,
	                                          (ULONG) "VCTest", TAG_DONE);
	if (screen == NULL)
	{
		kv_str("fail", "the screen did not open");
		goto out;
	}
	struct RastPort *rp = &screen->RastPort;
	struct BitMap *bm = rp->BitMap;
	struct RenderInfo ri;
	LONG lock = p96LockBitMap(bm, (UBYTE *)&ri, sizeof(ri));
	if (lock != 0)
		p96UnlockBitMap(bm, lock);
	const struct pixel_format *pf = lock != 0 ? pixel_format_of(ri.RGBFormat) : NULL;
	kv_hex("screen.mode", id);
	kv_unum("screen.width", (ULONG)screen->Width);
	kv_unum("screen.height", (ULONG)screen->Height);
	kv_unum("screen.depth", mode_depth);
	kv_str("screen.format", pf != NULL ? pf->name : "none");
	kv_num("screen.pitch", lock != 0 ? (LONG)ri.BytesPerRow : 0);
	kv_hex("screen.bitmap", lock != 0 ? (ULONG)ri.Memory : 0);
	kv_bool("screen.vram", p96GetBitMapAttr(bm, P96BMA_ISONBOARD) != 0);
	ULONG bar_h = height * 3u / 4u / 16u * 16u;
	if (depth == 8u)
		plain_wait(t, seconds);
	else
	{
		if (noise)
		{
			ULONG seed = 0x9E3779B9u;
			noise_draw(screen, 0, height, width, &seed);
		}
		else
			pattern_draw(rp, width, height, bar_h);
		screen_run(t, screen, height, bar_h, seconds, noise, back);
	}
	p96CloseScreen(screen);
	rc = RETURN_OK;
out:
	if (P96Base != NULL)
		CloseLibrary(P96Base);
	if (IntuitionBase != NULL)
		CloseLibrary((struct Library *)IntuitionBase);
	return rc;
}
