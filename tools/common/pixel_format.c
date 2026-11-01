/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * pixel_format.c - the Picasso96 formats with a videocodec.library name.
 */
#include <libraries/videocodec.h>

#include "pixel_format.h"

const struct pixel_format pixel_formats[] = {
    {RGBFB_R5G6B5PC, VCF_RGB16, 2, "R5G6B5PC"},
    {RGBFB_R8G8B8, VCF_RGB24, 3, "R8G8B8"},
    {RGBFB_B8G8R8, VCF_BGR24, 3, "B8G8R8"},
    {RGBFB_B8G8R8A8, VCF_BGRA32, 4, "B8G8R8A8"},
    {RGBFB_R8G8B8A8, VCF_RGBA32, 4, "R8G8B8A8"},
    {RGBFB_NONE, 0, 0, NULL},
};

/**
 * pixel_format_of() - The entry of a Picasso96 format.
 * @rgb: the format
 *
 * Return: the entry, NULL for a format videocodec.library has no name for.
 */
const struct pixel_format *pixel_format_of(RGBFTYPE rgb)
{
	for (const struct pixel_format *pf = pixel_formats; pf->name != NULL; pf++)
		if (pf->rgb == rgb)
			return pf;
	return NULL;
}

/**
 * pixel_format_bit() - A format's bit in a mask of formats, as Picasso96 has them.
 * @pf: the entry
 *
 * Return: the bit.
 */
ULONG pixel_format_bit(const struct pixel_format *pf)
{
	return 1UL << (ULONG)pf->rgb;
}

/**
 * pixel_formats_of_depth() - The formats of the size per pixel a depth has.
 * @depth: the depth, in bits
 *
 * Return: their mask.
 */
ULONG pixel_formats_of_depth(ULONG depth)
{
	ULONG bpp = depth < 24u ? 2u : depth == 24u ? 3u : 4u;
	ULONG mask = 0;
	for (const struct pixel_format *pf = pixel_formats; pf->name != NULL; pf++)
		if (pf->bpp == bpp)
			mask |= pixel_format_bit(pf);
	return mask;
}
