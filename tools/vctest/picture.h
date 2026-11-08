/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * picture.h - pictures in memory for the image calls and the encoder: the
 * backend's layout, DMA-reachable allocation, raw files in and out.
 */
#ifndef VCTEST_PICTURE_H
#define VCTEST_PICTURE_H

#include <exec/types.h>
#include <dos/dos.h>
#include <libraries/videocodec.h>

#define PICTURE_ALIGN 64 /* a cache line: a picture on whole lines can be imported into the VPU (zero copy) */

BOOL fmt_planar(ULONG f);
BOOL fmt_pairs(ULONG f);
ULONG raw_bytes(ULONG f, ULONG w, ULONG h);
BOOL fmt_column(ULONG f);
ULONG fmt_bpp(ULONG f);
ULONG frame_view(struct VCFrame *f, APTR data, ULONG fmt, ULONG w, ULONG h, ULONG pitch);
ULONG natural_pitch(ULONG fmt, ULONG w, ULONG h);
BOOL raw_into_frame(BPTR file, UBYTE *tmp, struct VCFrame *f, ULONG w, ULONG h);
BOOL frame_to_file(const char *name, const struct VCFrame *f);
UBYTE *load_file(const char *name, ULONG *len);
APTR picture_alloc(struct VCFrame *f, ULONG fmt, ULONG w, ULONG h, BOOL misalign, ULONG *size);

#endif /* VCTEST_PICTURE_H */
