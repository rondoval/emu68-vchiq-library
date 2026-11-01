/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * picture.c - pictures in memory: the backend's layout (libraries/videocodec.h),
 * allocation on a cache line, ffmpeg's rawvideo in and out.
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/dos_protos.h>
#else
#include <proto/exec.h>
#include <proto/dos.h>
#endif

#include <exec/memory.h>

#include <bits.h>
#include <kv.h>

#include "picture.h"

/**
 * fmt_planar() - Whether a format is planar YUV 4:2:0.
 * @f: VCF_*
 *
 * Return: TRUE for I420, YV12 and NV12.
 */
BOOL fmt_planar(ULONG f)
{
	return f == VCF_I420 || f == VCF_YV12 || f == VCF_NV12;
}

/**
 * fmt_bpp() - A packed format's bytes per pixel.
 * @f: VCF_*
 *
 * Return: 2, 3 or 4; 1 for a planar format (its luma plane).
 */
ULONG fmt_bpp(ULONG f)
{
	switch (f)
	{
	case VCF_RGB16:
	case VCF_YUYV:
	case VCF_UYVY:
		return 2;
	case VCF_RGB24:
	case VCF_BGR24:
		return 3;
	case VCF_BGRA32:
	case VCF_RGBA32:
		return 4;
	default:
		return 1;
	}
}

static ULONG align32(ULONG v)
{
	return (v + 31u) & ~31u;
}

/**
 * frame_view() - A frame over a picture laid out as the backend lays it out.
 * @f:     the frame to fill
 * @data:  the picture's memory (NULL to size it)
 * @fmt:   VCF_*
 * @w:     visible width
 * @h:     visible height
 * @pitch: bytes per luma row
 *
 * Rows of @pitch bytes; a planar picture's luma rows aligned to 32 pixels
 * and 16 rows, the chroma planes after.
 *
 * Return: the bytes the picture takes.
 */
ULONG frame_view(struct VCFrame *f, APTR data, ULONG fmt, ULONG w, ULONG h, ULONG pitch)
{
	for (ULONG i = 0; i < sizeof(*f); i++)
		((UBYTE *)f)[i] = 0;
	f->Size = sizeof(*f);
	f->Data = data;
	f->Format = fmt;
	f->Crop.Width = (LONG)w;
	f->Crop.Height = (LONG)h;
	f->Pitch[0] = pitch;
	if (fmt_planar(fmt))
	{
		ULONG ha = (h + 15u) & ~15u;
		f->Width = pitch;
		f->Height = ha;
		if (fmt == VCF_NV12)
		{
			f->Planes = 2;
			f->Offset[1] = pitch * ha;
			f->Pitch[1] = pitch;
			return f->Offset[1] + pitch * (ha / 2u);
		}
		f->Planes = 3;
		ULONG chroma = (pitch / 2u) * (ha / 2u);
		f->Pitch[1] = f->Pitch[2] = pitch / 2u;
		f->Offset[1] = fmt == VCF_I420 ? pitch * ha : pitch * ha + chroma;
		f->Offset[2] = fmt == VCF_I420 ? pitch * ha + chroma : pitch * ha;
		return pitch * ha + 2u * chroma;
	}
	f->Planes = 1;
	f->Width = pitch / fmt_bpp(fmt);
	f->Height = h;
	return pitch * h;
}

/**
 * natural_pitch() - The pitch the backend wants for a row of @w pixels.
 * @fmt: VCF_*
 * @w:   pixels per row
 *
 * Return: bytes per row: whole groups of 32 pixels, 64 for a planar format.
 */
ULONG natural_pitch(ULONG fmt, ULONG w)
{
	return fmt_planar(fmt) ? (w + 63u) & ~63u : align32(w) * fmt_bpp(fmt);
}

/**
 * raw_into_frame() - One raw picture from a file into a frame.
 * @file: ffmpeg's rawvideo: the visible planes back to back
 * @tmp:  room for one picture as the file holds it
 * @f:    the frame, over its memory
 * @w:    visible width
 * @h:    visible height
 *
 * Return: TRUE with the picture copied in; FALSE at the end of the file.
 */
BOOL raw_into_frame(BPTR file, UBYTE *tmp, struct VCFrame *f, ULONG w, ULONG h)
{
	ULONG fmt = f->Format;
	ULONG bytes = fmt_planar(fmt) ? w * h * 3u / 2u : w * h * fmt_bpp(fmt);
	LONG n = Read(file, tmp, (LONG)bytes);
	if (n <= 0)
		return FALSE;
	if ((ULONG)n != bytes)
	{
		note("a short picture at the end of the file (%ld of %lu bytes): dropped\n", n, bytes);
		return FALSE;
	}
	UBYTE *d = f->Data;
	const UBYTE *src = tmp;
	if (fmt_planar(fmt))
	{
		for (ULONG r = 0; r < h; r++, src += w)
			CopyMem((APTR)src, d + f->Offset[0] + r * f->Pitch[0], w);
		if (fmt == VCF_NV12)
			for (ULONG r = 0; r < h / 2u; r++, src += w)
				CopyMem((APTR)src, d + f->Offset[1] + r * f->Pitch[1], w);
		else
			for (ULONG pl = 1; pl <= 2; pl++)
				for (ULONG r = 0; r < h / 2u; r++, src += w / 2u)
					CopyMem((APTR)src, d + f->Offset[pl] + r * f->Pitch[pl], w / 2u);
		return TRUE;
	}
	ULONG row = w * fmt_bpp(fmt);
	for (ULONG r = 0; r < h; r++, src += row)
		CopyMem((APTR)src, d + r * f->Pitch[0], row);
	return TRUE;
}

/**
 * ppm_header() - A P6 header for a picture.
 * @hdr: room for it (32 bytes)
 * @w:   width
 * @h:   height
 *
 * Return: the header's length.
 */
static ULONG ppm_header(char *hdr, ULONG w, ULONG h)
{
	ULONG n = 0;
	for (const char *p6 = "P6\n"; *p6 != 0; p6++)
		hdr[n++] = *p6;
	ULONG vals[2] = {w, h};
	for (ULONG v = 0; v < 2; v++)
	{
		char num[12];
		ULONG k = 0, val = vals[v];
		do
		{
			num[k++] = (char)('0' + val % 10u);
			val /= 10u;
		} while (val != 0);
		while (k != 0)
			hdr[n++] = num[--k];
		hdr[n++] = v == 0 ? ' ' : '\n';
	}
	for (const char *max = "255\n"; *max != 0; max++)
		hdr[n++] = *max;
	return n;
}

/**
 * frame_to_file() - A frame's visible picture to a file.
 * @name: the file
 * @f:    the frame
 *
 * RGB24 goes as a PPM (P6), anything else as its planes raw, in ffmpeg's
 * rawvideo order.
 *
 * Return: TRUE when written.
 */
BOOL frame_to_file(const char *name, const struct VCFrame *f)
{
	BPTR out = Open((CONST_STRPTR)name, MODE_NEWFILE);
	if (out == 0)
	{
		PrintFault(IoErr(), (CONST_STRPTR)name);
		return FALSE;
	}
	ULONG w = (ULONG)f->Crop.Width, h = (ULONG)f->Crop.Height;
	ULONG x = (ULONG)f->Crop.X, y = (ULONG)f->Crop.Y;
	BOOL ok = TRUE;
	if (f->Format == VCF_RGB24)
	{
		char hdr[32];
		ULONG n = ppm_header(hdr, w, h);
		ok = Write(out, hdr, (LONG)n) == (LONG)n;
	}
	const UBYTE *d = f->Data;
	if (fmt_planar(f->Format))
	{
		for (ULONG r = 0; r < h && ok; r++)
			ok = Write(out, (APTR)(d + f->Offset[0] + (y + r) * f->Pitch[0] + x), (LONG)w) == (LONG)w;
		if (f->Format == VCF_NV12)
			for (ULONG r = 0; r < h / 2u && ok; r++)
				ok = Write(out, (APTR)(d + f->Offset[1] + (y / 2u + r) * f->Pitch[1] + x), (LONG)w) == (LONG)w;
		else
			for (ULONG pl = 1; pl <= 2 && ok; pl++)
				for (ULONG r = 0; r < h / 2u && ok; r++)
					ok = Write(out, (APTR)(d + f->Offset[pl] + (y / 2u + r) * f->Pitch[pl] + x / 2u), (LONG)(w / 2u)) == (LONG)(w / 2u);
	}
	else
	{
		ULONG bpp = fmt_bpp(f->Format), row = w * bpp;
		for (ULONG r = 0; r < h && ok; r++)
			ok = Write(out, (APTR)(d + (y + r) * f->Pitch[0] + x * bpp), (LONG)row) == (LONG)row;
	}
	Close(out);
	if (!ok)
		note("writing %s failed\n", name);
	return ok;
}

/**
 * load_file() - A whole file into memory.
 * @name: the file
 * @len:  where its length goes
 *
 * Return: the bytes (FreeVec()), NULL when it could not be read (printed).
 */
UBYTE *load_file(const char *name, ULONG *len)
{
	BPTR f = Open((CONST_STRPTR)name, MODE_OLDFILE);
	if (f == 0)
	{
		PrintFault(IoErr(), (CONST_STRPTR)name);
		return NULL;
	}
	Seek(f, 0, OFFSET_END);
	LONG size = Seek(f, 0, OFFSET_BEGINNING);
	UBYTE *data = size > 0 ? AllocVec((ULONG)size, MEMF_PUBLIC) : NULL;
	if (data == NULL || Read(f, data, size) != size)
	{
		note("cannot read %s (%ld bytes)\n", name, size);
		if (data != NULL)
			FreeVec(data);
		data = NULL;
	}
	Close(f);
	*len = (ULONG)size;
	return data;
}

/**
 * picture_alloc() - A picture in DMA-reachable memory, with a frame over it.
 * @f:        the frame to fill
 * @fmt:      VCF_*
 * @w:        visible width
 * @h:        visible height
 * @misalign: TRUE puts the picture 4 bytes past a cache line (the backend then sends it by bulk)
 * @size:     where the picture's bytes go
 *
 * Return: the block to FreeVec(), NULL without memory.
 */
APTR picture_alloc(struct VCFrame *f, ULONG fmt, ULONG w, ULONG h, BOOL misalign, ULONG *size)
{
	ULONG pitch = natural_pitch(fmt, w);
	struct VCFrame probe;
	*size = frame_view(&probe, NULL, fmt, w, h, pitch);
	APTR block = AllocVec(ALIGN_UP(*size, PICTURE_ALIGN) + PICTURE_ALIGN + 4u, MEMF_PUBLIC | MEMF_FAST);
	if (block != NULL)
		frame_view(f, (APTR)(ALIGN_UP((ULONG)block, PICTURE_ALIGN) + (misalign ? 4u : 0u)), fmt, w, h, pitch);
	return block;
}
