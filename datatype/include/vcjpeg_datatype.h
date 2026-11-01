/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * vcjpeg_datatype.h - vcjpeg.datatype: a picture.datatype subclass that
 * decodes JPEGs through videocodec.library, and the state its descriptor's
 * recognition function reads.
 *
 * The VCJPEG descriptor's function (vcjpeg_recog.c) decides which JPEGs come
 * here: baseline or extended, 8 bits, grey or YCbCr, at least MinPixels.  The
 * rest, and every JPEG while the VPU is down, go on to the system's JPEG
 * class.  What reaches the class and still fails on the VPU goes to the
 * system's class from inside OM_NEW (vcjpeg_class.c).
 */
#ifndef VCJPEG_DATATYPE_H
#define VCJPEG_DATATYPE_H

#define __NOLIBBASE__
#define EXEC_BASE_NAME SysBase /* a local in every function: base->SysBase, or the recognition function's context's */

#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/semaphores.h>
#include <exec/tasks.h>
#include <intuition/classes.h>

/* A picture under this goes to software: the VPU's round trip costs more (measured 2026-10-27, docs/architecture.md section 8) */
#define VCJPEG_MIN_PIXELS 1800000u

/*
 * videocodec.library closes after this long without a picture (between it and
 * twice it): its last close lets the backend give the VPU its memory back
 */
#define VCJPEG_IDLE_MS 5000u

/*
 * How far the recognition function looks for the frame header: past any
 * metadata, which the backend leaves out when the header would not end in
 * the image decoder's first input packet (docs/architecture.md section 9).
 * What is too long even so is refused by the backend at once and handed over
 * by the class.
 */
#define VCJPEG_HEADER_LIMIT (4u << 20)

/*
 * What the recognition function reads from a loaded class, under Forbid():
 * found by the library's name, never kept.  Size first, so that a function
 * and a class of different builds read only what both know.
 */
struct VCJpegShared
{
	ULONG Size;            /* sizeof(struct VCJpegShared) */
	ULONG MinPixels;       /* a smaller picture is not ours */
	struct Task *Fallback; /* the task handing a picture to the system's class: its recognitions are not ours */
	ULONG VPUDown;         /* nonzero: the VPU was lost; every JPEG to software until videocodec.library is closed */
};

struct VCJpegBase
{
	struct Library lib;
	struct VCJpegShared shared; /* right after the Library: the recognition function's view */
	struct ExecBase *SysBase;
	struct Library *DOSBase;
	struct Library *IntuitionBase;
	struct Library *UtilityBase;
	struct Library *DataTypesBase;
	struct Library *SuperClassBase; /* picture.datatype */
	struct Library *VideoCodecBase; /* opened by a picture (its first call scans for backends: a Process only), closed by idle_task */
	struct SignalSemaphore codec_lock;    /* VideoCodecBase, decoding, used, idle_task */
	ULONG decoding;                       /* pictures on the VPU now */
	BOOL used;                            /* a picture went to the VPU since idle_task last looked */
	struct Task *idle_task;               /* closes VideoCodecBase once VCJPEG_IDLE_MS pass with no picture; NULL when none */
	struct SignalSemaphore fallback_lock; /* one hand-over to the system's class at a time: shared.Fallback names it */
	Class *cl;
	ULONG segList;
};

/* The class (vcjpeg_class.c): made at the library's init, removed at its expunge */
Class *vcjpeg_make_class(struct VCJpegBase *base);
BOOL vcjpeg_free_class(struct VCJpegBase *base);
/* At the expunge, under Forbid(): whether the idle task is gone; a running one is told to close videocodec.library now */
BOOL vcjpeg_codec_closed(struct VCJpegBase *base);

#endif /* VCJPEG_DATATYPE_H */
