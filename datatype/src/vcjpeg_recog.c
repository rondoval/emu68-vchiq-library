/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * vcjpeg_recog.c - the VCJPEG descriptor's recognition function (its DTCD
 * chunk): whether a file is one of vcjpeg.datatype's.
 *
 * datatypes.library calls it for every binary file it examines, ahead of the
 * descriptors with a mask: the descriptor has none, priority 0, and is added
 * after the system's (docs/architecture.md section 8).  Its context comes in
 * A0, the answer goes back in D0.  It gets the file's first 64 bytes, so
 * anything that does not start like a JPEG is refused without I/O; a JPEG's
 * markers are walked through the library's file handle, a segment at a time,
 * and the handle is put back where it was.
 *
 * Not ours, so the next descriptor (the system's JPEG class) gets the file:
 * a frame header that is not baseline or extended, 8 bits, 1 or 3 components
 * (grey or YCbCr), or that comes past VCJPEG_HEADER_LIMIT; a picture under
 * the class's MinPixels; a source with no file handle; the class's own hand-over to software; anything while
 * the class has the VPU down (until it closes videocodec.library).  A
 * picture the VPU has no memory for is the class's: the backend refuses it in
 * milliseconds, and the class hands it over.  The class's state is read from
 * its library base under Forbid(), when it is loaded; the defaults otherwise.
 *
 * Linked on its own, entry first (ldscripts/module.lds' .text.entry), with no
 * data: the descriptor carries it as a hunk file, loaded once with the
 * descriptor.
 */
#include <vcjpeg_datatype.h> /* first: __NOLIBBASE__ for the proto headers below */

#include <datatypes/datatypes.h>
#include <dos/dos.h>
#include <exec/execbase.h>

#include <proto/dos.h>
#include <proto/exec.h>

#include <jpeg.h>

#define SIGNATURE_BYTES 3u /* SOI and the next marker's prefix: FF D8 FF */
#define SAMPLE_BITS 8u     /* the only precision the VPU decodes */
#define GREY_COMPONENTS 1u
#define YCBCR_COMPONENTS 3u

static BOOL find_frame(struct Library *DOSBase, BPTR file, UBYTE *sof);
static BOOL frame_is_ours(const UBYTE *sof, ULONG min_pixels);
static BOOL class_lets(struct ExecBase *SysBase, ULONG *min_pixels);

/**
 * vcjpeg_recognise() - Whether a file goes to vcjpeg.datatype.
 * @dthc: datatypes.library's context: the file's first bytes, its handle and lock
 *
 * Return: TRUE for a JPEG the VPU should decode, FALSE for anything else.
 */
LONG __attribute__((used, section(".text.entry"))) vcjpeg_recognise(struct DTHookContext *dthc asm("a0"));
LONG __attribute__((used, section(".text.entry"))) vcjpeg_recognise(struct DTHookContext *dthc asm("a0"))
{
	const UBYTE *head = (const UBYTE *)dthc->dthc_Buffer;
	if (dthc->dthc_BufferLength < SIGNATURE_BYTES || head[0] != JPEG_MARKER_PREFIX || head[1] != JPEG_SOI ||
	    head[2] != JPEG_MARKER_PREFIX)
		return FALSE;
	BPTR file = dthc->dthc_FileHandle;
	ULONG min_pixels;
	if (file == 0 || !class_lets((struct ExecBase *)dthc->dthc_SysBase, &min_pixels))
		return FALSE;

	struct Library *DOSBase = dthc->dthc_DOSBase;
	LONG position = Seek(file, 0, OFFSET_CURRENT);
	UBYTE sof[JPEG_SOF_BYTES];
	BOOL ours = find_frame(DOSBase, file, sof) && frame_is_ours(sof, min_pixels);
	Seek(file, position, OFFSET_BEGINNING);
	return ours;
}

/**
 * find_frame() - Walk a JPEG's markers to its frame header.
 * @DOSBase: dos.library
 * @file:    the file
 * @sof:     room for JPEG_SOF_BYTES
 *
 * Each step reads JPEG_SOF_BYTES at a marker: enough for a segment's length,
 * and all that is wanted of a frame header.  With fewer left in the file no
 * frame header can follow.
 *
 * Return: TRUE with the frame header in @sof, FALSE for none before the scan,
 * the end or VCJPEG_HEADER_LIMIT.
 */
static BOOL find_frame(struct Library *DOSBase, BPTR file, UBYTE *sof)
{
	ULONG offset = JPEG_MARKER_BYTES; /* behind SOI */
	while (offset + JPEG_SOF_BYTES <= VCJPEG_HEADER_LIMIT)
	{
		if (Seek(file, (LONG)offset, OFFSET_BEGINNING) < 0 || Read(file, sof, JPEG_SOF_BYTES) != (LONG)JPEG_SOF_BYTES)
			return FALSE;
		UBYTE marker = sof[1];
		if (sof[0] != JPEG_MARKER_PREFIX || marker == JPEG_EOI || marker == JPEG_SOS)
			return FALSE;
		if (marker == JPEG_MARKER_PREFIX)
			offset++; /* a fill byte */
		else if (jpeg_standalone(marker))
			offset += JPEG_MARKER_BYTES;
		else
		{
			ULONG length = get_be16(sof + JPEG_MARKER_BYTES); /* counts itself, not the marker */
			if (length < JPEG_LENGTH_BYTES)
				return FALSE;
			if (jpeg_frame_marker(marker))
				return TRUE;
			offset += JPEG_MARKER_BYTES + length;
		}
	}
	return FALSE;
}

/**
 * frame_is_ours() - Whether the VPU decodes the picture a frame header describes, and it is large enough to be worth it.
 * @sof:        the frame header, from its marker on
 * @min_pixels: the class's threshold
 *
 * Return: TRUE for a baseline or extended Huffman-coded picture of 8-bit
 * samples, grey or YCbCr, of at least @min_pixels.
 */
static BOOL frame_is_ours(const UBYTE *sof, ULONG min_pixels)
{
	BOOL sequential = sof[1] == JPEG_SOF0 || sof[1] == JPEG_SOF1;
	BOOL colour = sof[JPEG_SOF_COMPONENTS] == GREY_COMPONENTS || sof[JPEG_SOF_COMPONENTS] == YCBCR_COMPONENTS;
	ULONG pixels = get_be16(sof + JPEG_SOF_WIDTH) * get_be16(sof + JPEG_SOF_HEIGHT);
	return sequential && colour && sof[JPEG_SOF_PRECISION] == SAMPLE_BITS && pixels >= min_pixels;
}

/**
 * class_lets() - What the class says, if it is loaded: its threshold, and whether this task's JPEGs may be its.
 * @SysBase:    exec
 * @min_pixels: set to the class's threshold, or the default
 *
 * Return: FALSE while the class has the VPU down or hands a picture of this
 * task's to software, TRUE otherwise.
 */
static BOOL class_lets(struct ExecBase *SysBase, ULONG *min_pixels)
{
	BOOL lets = TRUE;
	*min_pixels = VCJPEG_MIN_PIXELS;
	Forbid();
	const struct VCJpegBase *base = (const struct VCJpegBase *)FindName(&SysBase->LibList, (CONST_STRPTR)LIBRARY_NAME);
	if (base != NULL && base->shared.Size >= sizeof(base->shared))
	{
		*min_pixels = base->shared.MinPixels;
		lets = base->shared.VPUDown == 0 && base->shared.Fallback != FindTask(NULL);
	}
	Permit();
	return lets;
}
