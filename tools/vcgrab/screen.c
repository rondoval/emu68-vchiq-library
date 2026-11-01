/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * screen.c - the screen to record: which one, and where its bitmap lies.
 */
#ifdef __INTELLISENSE__
#include <clib/dos_protos.h>
#include <clib/exec_protos.h>
#include <clib/intuition_protos.h>
#include <clib/Picasso96_protos.h>
#else
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/Picasso96.h>
#endif

#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <graphics/view.h>
#include <intuition/intuitionbase.h>
#include <libraries/Picasso96.h>
#include <string.h>

#include "vcgrab.h"

/**
 * front_screen() - Read the screen in front off Intuition's list.
 *
 * Return: the screen, NULL when none is open.
 */
static struct Screen *front_screen(void)
{
	ULONG lock = LockIBase(0);
	struct Screen *screen = IntuitionBase->FirstScreen;
	UnlockIBase(lock);
	return screen;
}

/**
 * source_read() - Take a screen and read what recording it needs.
 * @src:  where it goes, zeroed
 * @name: a public screen's name, NULL for the screen in front
 *
 * Only the bitmap on display is in VRAM (a hidden one would go by bulk, from
 * wherever Picasso96 keeps it), and a displayed bitmap stays where it is,
 * so the lock is held just to read the address.
 *
 * Return: NULL, or why not; the screen may be taken then.
 */
static const char *source_read(struct source *src, const char *name)
{
	src->screen = name != NULL ? LockPubScreen((CONST_STRPTR)name) : front_screen();
	if (src->screen == NULL)
		return name != NULL ? "no public screen of that name" : "no screen is open";
	src->locked = name != NULL;
	if (src->screen != front_screen())
		return "the screen is not in front";

	struct BitMap *bm = src->screen->ViewPort.RasInfo->BitMap;
	if (!p96GetBitMapAttr(bm, P96BMA_ISP96))
		return "not an RTG screen: native screens live in Chip RAM, which the VideoCore cannot reach";
	if (!p96GetBitMapAttr(bm, P96BMA_ISONBOARD))
		return "the screen's bitmap is not in VRAM";

	struct RenderInfo ri;
	LONG lock = p96LockBitMap(bm, (UBYTE *)&ri, sizeof(ri));
	if (lock == 0)
		return "the screen's bitmap did not lock";
	p96UnlockBitMap(bm, lock);

	src->format = pixel_format_of(ri.RGBFormat);
	if (src->format == NULL)
		return "the screen's pixel format has no VideoCore equivalent: 16-bit R5G6B5PC, 24 or 32 bits without ARGB";
	src->memory = ri.Memory;
	src->width = (ULONG)src->screen->Width;
	src->height = (ULONG)src->screen->Height;
	src->pitch = (ULONG)ri.BytesPerRow;
	return NULL;
}

/**
 * source_find() - Take the screen to record: it must be an RTG screen in front.
 * @src:  where it goes
 * @name: a public screen's name, NULL for the screen in front
 *
 * Return: TRUE, FALSE with the reason said and nothing held.
 */
BOOL source_find(struct source *src, const char *name)
{
	memset(src, 0, sizeof(*src));
	const char *why = source_read(src, name);
	if (why == NULL)
		return TRUE;
	grab_say("%s", why);
	source_release(src);
	return FALSE;
}

/**
 * source_state() - Tell where the screen is now.
 * @src: the source, taken
 *
 * A hidden screen is not looked into: it may be closed by now, and only the
 * one in front is sure to be open, for as long as Intuition's lock is held.
 * The ViewPort's bitmap is the one on show, which a screen flipping between
 * bitmaps changes.
 *
 * Return: its state.
 */
enum source_state source_state(const struct source *src)
{
	enum source_state state = SOURCE_HIDDEN;
	ULONG lock = LockIBase(0);
	if (IntuitionBase->FirstScreen == src->screen)
	{
		struct BitMap *bm = src->screen->ViewPort.RasInfo->BitMap;
		state = (APTR)p96GetBitMapAttr(bm, P96BMA_MEMORY) == src->memory ? SOURCE_SHOWN : SOURCE_MOVED;
	}
	UnlockIBase(lock);
	return state;
}

/**
 * source_release() - Let go of the screen.
 * @src: the source, taken or not
 */
void source_release(struct source *src)
{
	if (src->locked)
		UnlockPubScreen(NULL, src->screen);
}
