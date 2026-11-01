/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * pixel_format.h - the Picasso96 formats with a videocodec.library name, for
 * the tools that work on a screen's bitmap (VPUPlay, VCGrab, VCTest SCREEN).
 */
#ifndef PIXEL_FORMAT_H
#define PIXEL_FORMAT_H

#include <exec/types.h>
#include <libraries/Picasso96.h>

struct pixel_format
{
	RGBFTYPE rgb;
	ULONG vcf;
	ULONG bpp;
	const char *name;
};

/* Ends with an entry whose name is NULL. */
extern const struct pixel_format pixel_formats[];

const struct pixel_format *pixel_format_of(RGBFTYPE rgb);
ULONG pixel_format_bit(const struct pixel_format *pf);
ULONG pixel_formats_of_depth(ULONG depth);

#endif /* PIXEL_FORMAT_H */
