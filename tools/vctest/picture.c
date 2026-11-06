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
 * fmt_planar() - Whether a format is planar YUV: its planes one after another.
 * @f: VCF_*
 *
 * Return: TRUE for I420, YV12, I422, NV12 and NV21.
 */
BOOL fmt_planar(ULONG f)
{
	return f == VCF_I420 || f == VCF_YV12 || f == VCF_I422 || fmt_pairs(f);
}

/**
 * fmt_pairs() - Whether a planar format keeps its chroma as one plane of pairs.
 * @f: VCF_*
 *
 * Return: TRUE for NV12 and NV21.
 */
BOOL fmt_pairs(ULONG f)
{
	return f == VCF_NV12 || f == VCF_NV21;
}

/**
 * fmt_column() - Whether a format lays the picture out in 128-byte columns.
 * @f: VCF_*
 *
 * Return: TRUE for NV12_COL128 and NV12_10_COL128.
 */
BOOL fmt_column(ULONG f)
{
	return f == VCF_NV12_COL128 || f == VCF_NV12_10_COL128;
}

#define COLUMN_BYTES 128u     /* a column row's bytes */
#define COLUMN10_SAMPLES 96u  /* the 10-bit column format's samples a row: three in each 32 bits */

/**
 * column_pixels() - A column format's pixels across a column.
 * @f: VCF_*, a column format
 *
 * Return: 128 for 8-bit samples, 96 for 10-bit ones.
 */
static ULONG column_pixels(ULONG f)
{
	return f == VCF_NV12_10_COL128 ? COLUMN10_SAMPLES : COLUMN_BYTES;
}

/**
 * chroma_rows() - A planar format's chroma rows for @rows luma rows.
 * @f:    VCF_*, a planar format
 * @rows: the luma rows
 *
 * Return: every row for 4:2:2, half of them for 4:2:0.
 */
static ULONG chroma_rows(ULONG f, ULONG rows)
{
	return f == VCF_I422 ? rows : rows / 2u;
}

/**
 * fmt_bpp() - A packed format's bytes per pixel.
 * @f: VCF_*
 *
 * Return: 2, 3 or 4; 1 for a planar format (its luma plane) and CLUT8.
 */
ULONG fmt_bpp(ULONG f)
{
	switch (f)
	{
	case VCF_RGB16:
	case VCF_RGB16BE:
	case VCF_RGB15:
	case VCF_RGB15BE:
	case VCF_YUYV:
	case VCF_UYVY:
	case VCF_YVYU:
	case VCF_VYUY:
		return 2;
	case VCF_RGB24:
	case VCF_BGR24:
		return 3;
	case VCF_BGRA32:
	case VCF_RGBA32:
	case VCF_RGBX32:
	case VCF_BGRX32:
	case VCF_ARGB32:
	case VCF_ABGR32:
		return 4;
	default:
		return 1;
	}
}

/**
 * raw_bytes() - A picture's bytes as ffmpeg's rawvideo holds it: the visible planes back to back.
 * @f: VCF_*, not a column format
 * @w: width
 * @h: height
 *
 * Return: the bytes.
 */
ULONG raw_bytes(ULONG f, ULONG w, ULONG h)
{
	if (fmt_pairs(f))
		return w * h + w * (h / 2u);
	if (fmt_planar(f))
		return w * h + 2u * (w / 2u) * chroma_rows(f, h);
	return w * h * fmt_bpp(f);
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
 * and 16 rows, the chroma planes after.  NV12 and NV21, which only the
 * converter reads, have their chroma right after their own rows, as a camera
 * sends it.  A column picture's @pitch is one column's bytes; its rows are a
 * multiple of 16, the chroma rows follow them inside each column.
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
	if (fmt_column(fmt))
	{
		ULONG across = column_pixels(fmt);
		ULONG rows = (h + 15u) & ~15u, columns = (w + across - 1u) / across;
		f->Width = columns * across;
		f->Height = rows;
		f->Planes = 2;
		f->Offset[1] = COLUMN_BYTES * rows;
		f->Pitch[1] = pitch;
		return columns * pitch;
	}
	if (fmt_planar(fmt))
	{
		ULONG ha = fmt_pairs(fmt) ? h : (h + 15u) & ~15u;
		f->Width = pitch;
		f->Height = ha;
		if (fmt_pairs(fmt))
		{
			f->Planes = 2;
			f->Offset[1] = pitch * ha;
			f->Pitch[1] = pitch;
			return f->Offset[1] + pitch * (ha / 2u);
		}
		f->Planes = 3;
		ULONG chroma = (pitch / 2u) * chroma_rows(fmt, ha);
		BOOL v_first = fmt == VCF_YV12;
		f->Pitch[1] = f->Pitch[2] = pitch / 2u;
		f->Offset[1] = v_first ? pitch * ha + chroma : pitch * ha;
		f->Offset[2] = v_first ? pitch * ha : pitch * ha + chroma;
		return pitch * ha + 2u * chroma;
	}
	f->Planes = 1;
	f->Width = pitch / fmt_bpp(fmt);
	f->Height = h;
	return pitch * h;
}

/**
 * natural_pitch() - The pitch the backend wants for a picture of @w by @h.
 * @fmt: VCF_*
 * @w:   pixels per row
 * @h:   rows
 *
 * Return: bytes per row: whole groups of 32 pixels, 64 for a planar format;
 * for a column format one column's bytes, 128 per luma and chroma row.
 */
ULONG natural_pitch(ULONG fmt, ULONG w, ULONG h)
{
	if (fmt_column(fmt))
	{
		ULONG rows = (h + 15u) & ~15u;
		return 128u * (rows + rows / 2u);
	}
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
	if (fmt_column(fmt))
	{
		/* the file holds the columns as the buffer does: straight in */
		ULONG size = f->Pitch[0] * (f->Width / column_pixels(fmt));
		LONG n = Read(file, f->Data, (LONG)size);
		if (n > 0 && (ULONG)n != size)
			note("a short picture at the end of the file (%ld of %lu bytes): dropped\n", n, size);
		return n > 0 && (ULONG)n == size;
	}
	ULONG bytes = raw_bytes(fmt, w, h);
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
		if (fmt_pairs(fmt))
			for (ULONG r = 0; r < h / 2u; r++, src += w)
				CopyMem((APTR)src, d + f->Offset[1] + r * f->Pitch[1], w);
		else
			for (ULONG pl = 1; pl <= 2; pl++)
				for (ULONG r = 0; r < chroma_rows(fmt, h); r++, src += w / 2u)
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
		if (fmt_pairs(f->Format))
			for (ULONG r = 0; r < h / 2u && ok; r++)
				ok = Write(out, (APTR)(d + f->Offset[1] + (y / 2u + r) * f->Pitch[1] + x), (LONG)w) == (LONG)w;
		else
		{
			ULONG top = chroma_rows(f->Format, y * 2u) / 2u; /* the first visible row's chroma row */
			for (ULONG pl = 1; pl <= 2 && ok; pl++)
				for (ULONG r = 0; r < chroma_rows(f->Format, h) && ok; r++)
					ok = Write(out, (APTR)(d + f->Offset[pl] + (top + r) * f->Pitch[pl] + x / 2u), (LONG)(w / 2u)) == (LONG)(w / 2u);
		}
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
	ULONG pitch = natural_pitch(fmt, w, h);
	struct VCFrame probe;
	*size = frame_view(&probe, NULL, fmt, w, h, pitch);
	APTR block = AllocVec(ALIGN_UP(*size, PICTURE_ALIGN) + PICTURE_ALIGN + 4u, MEMF_PUBLIC | MEMF_FAST);
	if (block != NULL)
		frame_view(f, (APTR)(ALIGN_UP((ULONG)block, PICTURE_ALIGN) + (misalign ? 4u : 0u)), fmt, w, h, pitch);
	return block;
}
