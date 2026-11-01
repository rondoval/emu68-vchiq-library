/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * vchiq_client.h - a VCHIQ service from a CLI tool (VCInfo, VCTest).
 *
 * A service is plain Exec I/O on vchiq.device: VQ_READS reads are kept
 * posted, writes and bulk transfers are DoIO()s.  Every wait takes a
 * timeout from the caller's timer and gives up on Ctrl-C.
 */
#ifndef VCHIQ_CLIENT_H
#define VCHIQ_CLIENT_H

#include <exec/types.h>
#include <exec/ports.h>

#include <drv_timer.h>
#include <devices/vchiq.h>

#define VQ_READS 2u
#define VQ_MSG_BYTES 512u
#define VQ_REPLY_TIMEOUT_MS 3000

#define GCMD_FOURCC VCHIQ_FOURCC('G', 'C', 'M', 'D')

/* One open VCHIQ service: the opened request, VQ_READS posted reads, one request for writes and bulks. */
struct vq_service
{
	struct drv_timer *timer; /* the caller's, for the waits */
	struct MsgPort *port;
	struct IOVchiqReq *io;
	struct IOVchiqReq *rd[VQ_READS];
	struct IOVchiqReq *wr;
	UBYTE buf[VQ_READS][VQ_MSG_BYTES];
	ULONG next;  /* the read whose reply comes next */
	LONG error;  /* why vq_open() failed: OpenDevice()'s io_Error, or -1 out of memory */
};

BOOL vq_open(struct vq_service *s, struct drv_timer *timer, ULONG fourcc, UWORD version, UWORD version_min,
             const char *what);
void vq_close(struct vq_service *s);
LONG vq_do(struct vq_service *s, UWORD command, CONST_APTR data, ULONG size, ULONG *actual);
BOOL vq_next(struct vq_service *s, UBYTE **data, ULONG *len, ULONG timeout_ms);
void vq_consume(struct vq_service *s);
BOOL vq_get_info(struct vq_service *s, struct VchiqInfo *info);

#endif /* VCHIQ_CLIENT_H */
