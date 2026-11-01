/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * videocore_gencmd.h - the VPU's codec memory, read through the firmware's
 * gencmd service (videocore_gencmd.c).
 *
 * The codecs allocate from the firmware's relocatable heap, carved out of
 * gpu_mem (config.txt).  gencmd reports its size and its free part; a
 * decoder the heap cannot hold gives no error of its own, so this is how a
 * starved decoder is told from a refused stream (decoder_check_input()), and
 * what VCInfo.MemTotal / MemFree report.
 *
 * A query opens a gencmd unit of vchiq.device for itself, in the caller's
 * task, and closes it again: a few milliseconds of VPU round trips, no lock,
 * nothing shared with the MMAL link.  It waits for the answers without a
 * timeout: gencmd answers while the connection is up, and vchiq.device fails
 * the reads when it goes.
 */
#ifndef VIDEOCORE_GENCMD_H
#define VIDEOCORE_GENCMD_H

#include <exec/types.h>

struct VideoCoreBase;

BOOL gencmd_codec_memory(struct VideoCoreBase *base, ULONG *total, ULONG *free_bytes);

#endif /* VIDEOCORE_GENCMD_H */
