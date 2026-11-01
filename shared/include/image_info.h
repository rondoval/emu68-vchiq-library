/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * image_info.h - what a still picture's header says: JPEG, PNG, GIF, BMP, TGA,
 * PPM.  Built into videocodec.library (VC_GetImageInfo()) and into backends
 * that check a picture before their hardware sees it.
 */
#ifndef IMAGE_INFO_H
#define IMAGE_INFO_H

#include <exec/types.h>
#include <libraries/videocodec.h>

#define IMAGE_INFO_SHORT 1 /* image_info(): the header goes on past the bytes given; the facts so far are filled in */

/*
 * What a picture's header says: VC_GetImageInfo() hands on the codec, size,
 * transparency and frames; a backend's rules read the rest (a decoder that
 * must find the whole header in its first buffer checks header against it).
 * bits is the size of one sample: one channel of a pixel (alpha included), or
 * a pixel's index for IMAGE_MODEL_PALETTE.  8 for most pictures; 12 or 16 for
 * some JPEG, PNG and PPM; 1 to 8 for a palette; 5 for a 15- or 16-bit RGB
 * pixel (BMP, TGA); 0 where the header does not say (a BMP holding a JPEG or
 * PNG).
 */
struct image_facts
{
	ULONG codec;         /* VCC_* */
	ULONG width, height; /* pixels */
	ULONG model;         /* IMAGE_MODEL_*: what a pixel's samples are */
	ULONG bits;          /* bits of one sample (above) */
	ULONG transparency;  /* VCTR_* */
	ULONG coding;        /* IMAGE_CODING_*: how the samples are stored */
	BOOL interlaced;     /* the rows stored out of order (an interlaced PNG or GIF) */
	ULONG frames;        /* pictures in the file, counted as far as the bytes go (a GIF's) */
	ULONG header;        /* bytes before the picture's data (a JPEG's scan header included), 0 if the bytes end first */
	BOOL whole;          /* the picture's data is all there: its end mark, or the bytes its header implies (a GIF's first image) */
	ULONG skip_from, skip_to; /* a JPEG's longest run of metadata segments in its header (jpeg_metadata_marker()), both 0 for none */
};

/* image_facts.model */
#define IMAGE_MODEL_RGB 1     /* red, green and blue (a JPEG stores them as YCbCr) */
#define IMAGE_MODEL_GREY 2    /* one channel of grey */
#define IMAGE_MODEL_PALETTE 3 /* an index per pixel into a table of colours */
#define IMAGE_MODEL_CMYK 4    /* cyan, magenta, yellow and black (a JPEG's CMYK or YCCK) */

/* image_facts.coding */
#define IMAGE_CODING_NONE 0        /* the samples as they are (BMP, TGA, PPM) */
#define IMAGE_CODING_RLE 1         /* run-length coded (BMP, TGA) */
#define IMAGE_CODING_DEFLATE 2     /* PNG */
#define IMAGE_CODING_LZW 3         /* GIF */
#define IMAGE_CODING_SEQUENTIAL 4  /* JPEG, baseline or extended: the picture once, top to bottom */
#define IMAGE_CODING_PROGRESSIVE 5 /* JPEG in passes of rising detail */
#define IMAGE_CODING_LOSSLESS 6    /* JPEG's lossless process */
#define IMAGE_CODING_OTHER 7       /* a hierarchical or arithmetic-coded JPEG; a BMP with bit fields or holding a JPEG or PNG */

/* The codec a picture's signature names (its first bytes only): VCC_None for none, and for a TGA, which has none */
ULONG image_codec(const UBYTE *data, ULONG length);

/* Read a picture's header into @info: 0, IMAGE_INFO_SHORT, or VCERR_STREAM for none this knows */
LONG image_info(const UBYTE *data, ULONG length, ULONG codec, struct image_facts *info);

#endif /* IMAGE_INFO_H */
