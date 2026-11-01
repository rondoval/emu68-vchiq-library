/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * jpeg.h - a JPEG's markers and frame header (ITU-T T.81, annex B), for the
 * code that walks a JPEG's header: image_info.c and vcjpeg.datatype's
 * recognition function.  Macros and inline functions only: the recognition
 * function is linked with no data.
 *
 * A marker is two bytes, FF and its code; any number of FF fill bytes may
 * come before it.  Most markers are followed by a 16-bit big-endian length
 * that counts itself but not the marker; the standalone ones are not.
 */
#ifndef JPEG_H
#define JPEG_H

#include <exec/types.h>

#define JPEG_MARKER_PREFIX 0xffu /* every marker's first byte, and a fill byte */
#define JPEG_MARKER_BYTES 2u
#define JPEG_LENGTH_BYTES 2u /* a segment's length field, after its marker */

/* Marker codes */
#define JPEG_SOF0 0xc0u  /* frame header: baseline sequential, Huffman */
#define JPEG_SOF1 0xc1u  /* frame header: extended sequential, Huffman */
#define JPEG_SOF2 0xc2u  /* frame header: progressive, Huffman */
#define JPEG_SOF3 0xc3u  /* frame header: lossless, Huffman */
#define JPEG_DHT 0xc4u   /* Huffman tables: inside the SOFn range, no frame header */
#define JPEG_JPG 0xc8u   /* reserved for extensions: likewise */
#define JPEG_DAC 0xccu   /* arithmetic conditioning: likewise */
#define JPEG_SOF15 0xcfu /* the last frame header code (lossless, hierarchical, arithmetic) */
#define JPEG_RST0 0xd0u  /* restart markers RST0 to RST7: standalone */
#define JPEG_SOI 0xd8u   /* start of image: standalone, the file's first marker */
#define JPEG_EOI 0xd9u   /* end of image: standalone */
#define JPEG_SOS 0xdau   /* start of scan: the coded picture follows its header */
#define JPEG_TEM 0x01u   /* temporary, for arithmetic coding: standalone */
#define JPEG_APP0 0xe0u  /* application data: APP0 JFIF */
#define JPEG_APP14 0xeeu /* Adobe's: how the samples are coded */
#define JPEG_APP15 0xefu /* the last application segment */
#define JPEG_COM 0xfeu   /* comment */

/* A frame header (SOFn), offsets from its marker on */
#define JPEG_SOF_PRECISION 4u  /* bits per sample */
#define JPEG_SOF_HEIGHT 5u     /* rows, 16 bits */
#define JPEG_SOF_WIDTH 7u      /* pixels, 16 bits */
#define JPEG_SOF_COMPONENTS 9u /* 1 grey, 3 YCbCr (or RGB), 4 CMYK */
#define JPEG_SOF_BYTES 10u     /* as far as the component count */

/**
 * get_be16() - A 16-bit big-endian field, at any address: a segment's length, a frame header's size.
 * @d: its first byte
 *
 * Return: the value.
 */
static inline ULONG get_be16(const UBYTE *d)
{
	return ((ULONG)d[0] << 8) | d[1];
}

/**
 * jpeg_standalone() - Whether a marker has no length after it: TEM, RST0 to RST7, SOI.
 * @marker: the marker's code
 *
 * Return: TRUE for a standalone marker.
 */
static inline BOOL jpeg_standalone(UBYTE marker)
{
	return marker == JPEG_TEM || (marker >= JPEG_RST0 && marker <= JPEG_SOI);
}

/**
 * jpeg_metadata_marker() - Whether a segment is about the picture and not needed to decode it.
 * @marker: the marker's code
 *
 * Application data and comments: Exif, XMP, ICC profiles.  Not APP0, which
 * says JFIF, nor APP14, which says how the samples are coded.
 *
 * Return: TRUE for a segment a decoder can go without.
 */
static inline BOOL jpeg_metadata_marker(UBYTE marker)
{
	return (marker > JPEG_APP0 && marker <= JPEG_APP15 && marker != JPEG_APP14) || marker == JPEG_COM;
}

/**
 * jpeg_frame_marker() - Whether a marker starts a frame header: SOF0 to SOF15 but DHT, JPG and DAC.
 * @marker: the marker's code
 *
 * Return: TRUE for a frame header.
 */
static inline BOOL jpeg_frame_marker(UBYTE marker)
{
	return marker >= JPEG_SOF0 && marker <= JPEG_SOF15 && marker != JPEG_DHT && marker != JPEG_JPG && marker != JPEG_DAC;
}

#endif /* JPEG_H */
