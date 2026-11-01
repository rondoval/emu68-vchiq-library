/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * still.c - one picture of the screen to a JPEG: the converter's image call,
 * then the encoder's.  Both block, and each waits its turn behind other
 * image calls, so a still during a recording costs it a few ticks.
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/dos_protos.h>
#include <clib/videocodec_protos.h>
#else
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/videocodec.h>
#endif

#include <dos/dos.h>
#include <exec/memory.h>

#include "vcgrab.h"

#define JPEG_SLACK (64u << 10) /* over the encoder's own minimum, a byte per pixel of the buffer */

/**
 * jpeg_save() - Write the JPEG to its file; a file cut short is deleted.
 * @file: where it goes
 * @jpeg: the bytes
 * @size: how many
 *
 * Return: RETURN_OK, RETURN_ERROR with the reason said.
 */
static int jpeg_save(const char *file, APTR jpeg, LONG size)
{
	BPTR fh = Open((CONST_STRPTR)file, MODE_NEWFILE);
	if (fh == 0)
	{
		grab_fault(file);
		return RETURN_ERROR;
	}
	BOOL whole = Write(fh, jpeg, size) == size;
	if (!whole)
		grab_fault(file);
	Close(fh);
	if (!whole)
		DeleteFile((CONST_STRPTR)file);
	return whole ? RETURN_OK : RETURN_ERROR;
}

/**
 * still_grab() - Save a screen as it is now to a JPEG file.
 * @src:    the screen
 * @width:  the picture, 0 for the screen's
 * @height:
 * @file:   where it goes
 *
 * The ISP makes full-range BT.601 of RGB: what a JPEG holds.
 *
 * Return: RETURN_OK, RETURN_ERROR for no memory or no file, RETURN_FAIL for a refused call; the reason was said.
 */
static int still_grab(const struct source *src, ULONG width, ULONG height, const char *file)
{
	ULONG w = (width != 0 ? width : src->width) & ~1u, h = (height != 0 ? height : src->height) & ~1u;
	ULONG pitch = I420_PITCH(w), rows = I420_ROWS(h);
	ULONG i420_bytes = pitch * rows * 3u / 2u, jpeg_bytes = pitch * rows + JPEG_SLACK;

	UBYTE *block = AllocVec(i420_bytes + DMA_ALIGN_MIN, MEMF_PUBLIC); /* on whole cache lines the VPU writes it itself */
	UBYTE *jpeg = AllocVec(jpeg_bytes, MEMF_PUBLIC);
	int rc = RETURN_ERROR;
	if (block == NULL || jpeg == NULL)
	{
		grab_say("no memory for a still");
		goto out;
	}

	UBYTE *i420 = (UBYTE *)ALIGN_UP((ULONG)block, DMA_ALIGN_MIN);
	LONG result = VC_ConvertImageTags(VCA_Data, (ULONG)src->memory,
	                                  VCA_Width, src->width, VCA_Height, src->height,
	                                  VCA_InputFormat, src->format->vcf, VCA_InputPitch, src->pitch,
	                                  VCA_Dest, (ULONG)i420, VCA_OutputFormat, VCF_I420,
	                                  VCA_OutputWidth, w, VCA_OutputHeight, h, VCA_OutputPitch, pitch,
	                                  TAG_DONE); /* 0 or a VCERR_* */
	if (result == 0)
		result = VC_EncodeImageTags(VCA_Data, (ULONG)i420,
		                            VCA_Width, w, VCA_Height, h,
		                            VCA_InputFormat, VCF_I420, VCA_InputPitch, pitch,
		                            VCA_Dest, (ULONG)jpeg, VCA_DestSize, jpeg_bytes,
		                            TAG_DONE); /* the JPEG's bytes, or a VCERR_* */
	if (result <= 0)
	{
		grab_error("the still failed", result);
		rc = RETURN_FAIL;
		goto out;
	}
	rc = jpeg_save(file, jpeg, result);
	if (rc == RETURN_OK)
	{
		grab_report("%s: %lux%lu, %ld bytes", file, w, h, result);
		window_status("%s: %lux%lu, %ld bytes", (const char *)FilePart((CONST_STRPTR)file), w, h, result);
	}
out:
	if (block != NULL)
		FreeVec(block);
	if (jpeg != NULL)
		FreeVec(jpeg);
	return rc;
}

/**
 * still_take() - Grab one still.
 * @width:  the picture, 0 for the screen's
 * @height:
 * @screen: a public screen's name, NULL for the screen in front
 * @file:   where it goes
 *
 * Return: the return code; the reason was said.
 */
int still_take(ULONG width, ULONG height, const char *screen, const char *file)
{
	struct source src;
	if (!source_find(&src, screen))
		return RETURN_ERROR;
	int rc = still_grab(&src, width, height, file);
	source_release(&src);
	return rc;
}
