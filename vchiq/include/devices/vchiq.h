/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * devices/vchiq.h - vchiq.device: the VideoCore VCHIQ transport for AmigaOS.
 *
 * VCHIQ connects to named servers in the VPU firmware ("GCMD" for vcgencmd,
 * "AUDS" for audio, "mmal" for the codecs and the ISP, ...).  One OpenDevice()
 * opens one such service; any number of services and openers work in
 * parallel.  Fill in iovq_FourCC, iovq_Version and iovq_VersionMin before
 * OpenDevice() (unit 0); it fails with VQERR_REJECTED when the VPU has no such
 * server or no common version, and sets iovq_PeerVersion when it succeeds.
 * iovq_FourCC 0 opens no service: such a request serves NSCMD_VQ_GETINFO
 * and NSCMD_DEVICEQUERY only.
 * OpenDevice() and CloseDevice() wait for the VPU's answer (up to 2 s).  To use
 * a service from several tasks, give each a copy of the opened request.
 *
 * Messages are small (at most VCHIQ_MAX_MESSAGE bytes) and copied through
 * shared memory; bulk transfers move whole buffers with the VPU's DMA engine.
 * Everything the VPU reads and writes is little-endian: use VCHIQ_LE32() /
 * VCHIQ_LE16() on payloads.
 *
 * The loop a client writes:
 *     OpenDevice("vchiq.device", 0, ioreq, 0)
 *     SendIO() a few CMD_READs   - each comes back with one message
 *     CMD_WRITE                  - one message to the VPU
 *     NSCMD_VQ_BULKWRITE/READ    - a buffer to or from the VPU, by DMA
 *     CloseDevice()              - every request must be back first
 *
 * Writes and bulk transmits of one service go out in the order they were
 * issued, so a message and the bulk transmit that belongs to it stay
 * together; bulk receives go out in their own order, never held up behind a
 * transmit (the VPU may have to finish a receive before it takes the next
 * transmit).  The device holds requests while the VPU has no room.  A request that
 * came back owns nothing in the device (a read has copied its message).
 * AbortIO() works on a queued request; a bulk transfer the VPU already has
 * completes normally.  Bulk buffers lie in the Pi's RAM (Fast RAM or VRAM,
 * never Chip RAM), at any alignment; the device does the cache maintenance.
 *
 * A service whose client does not read holds at most 128 messages; then the
 * device stops reading what the VPU sends, for every service, until that
 * client reads.  Keep reads posted.
 *
 * vchiq.device is a new-style device: NSCMD_DEVICEQUERY lists what it
 * understands.  Of the standard commands it knows CMD_READ, CMD_WRITE and
 * CMD_FLUSH (aborts every queued request of the opener's service).
 */
#ifndef DEVICES_VCHIQ_H
#define DEVICES_VCHIQ_H

#include <exec/types.h>
#include <exec/io.h>

#define VCHIQ_NAME "vchiq.device"

#define VCHIQ_FOURCC(a, b, c, d) (((ULONG)(a) << 24) | ((ULONG)(b) << 16) | ((ULONG)(c) << 8) | (ULONG)(d))

#define VCHIQ_LE32(v) ((ULONG)__builtin_bswap32((ULONG)(v)))
#define VCHIQ_LE16(v) ((UWORD)__builtin_bswap16((UWORD)(v)))

#define VCHIQ_MAX_MESSAGE 4088 /* payload bytes of one message */

/*
 * Commands: a block in the third-party command area (the fleet's blocks:
 * nvme 0x8020, usbhcd 0x8800, netdev 0x8900, usbvideo 0x8a00, vchiq 0x8b00).
 */
#define NSCMD_VCHIQ_BASE   0x8B00
#define NSCMD_VQ_BULKWRITE (NSCMD_VCHIQ_BASE + 0x00) /* io_Data, io_Length: to the VPU; io_Actual: bytes moved */
#define NSCMD_VQ_BULKREAD  (NSCMD_VCHIQ_BASE + 0x01) /* io_Data, io_Length: from the VPU; io_Actual: bytes moved */
#define NSCMD_VQ_GETINFO   (NSCMD_VCHIQ_BASE + 0x02) /* io_Data: struct VchiqInfo, io_Length: its size; io_Actual: filled */

/*
 * The request.  A plain struct IOStdReq is too short: OpenDevice() needs the
 * service fields.  io_Data, io_Length and io_Actual carry the data as usual.
 */
struct IOVchiqReq
{
	struct IOStdReq iovq_Req;
	ULONG iovq_FourCC;      /* OpenDevice: the service, e.g. VCHIQ_FOURCC('G','C','M','D') */
	UWORD iovq_Version;     /* OpenDevice: the service's protocol version this client speaks ... */
	UWORD iovq_VersionMin;  /* ... and the oldest it can still speak; the VPU refuses the open if its own range does not overlap */
	UWORD iovq_PeerVersion; /* set by OpenDevice: the version the VPU's side of the service speaks */
	UWORD iovq_Pad;
};

/*
 * io_Error: 0, the standard IOERR_* or one of the VQERR_* below.
 *   IOERR_OPENFAIL    OpenDevice: unit not 0, request too short, no memory
 *   IOERR_UNITBUSY    OpenDevice: every service in use, or no slot free for the OPEN
 *   IOERR_ABORTED     AbortIO(), CMD_FLUSH, or CloseDevice() with the request still queued
 *   IOERR_NOCMD       a command vchiq.device does not know, or one an information-only unit cannot do
 *   IOERR_BADLENGTH   a message longer than VCHIQ_MAX_MESSAGE, a bulk of 0 bytes or one the page list cannot describe
 *   IOERR_BADADDRESS  no buffer
 */
#define VQERR_NOCONNECTION 1 /* the connection to the VPU is not up (bring-up failed, or it was lost) */
#define VQERR_REJECTED     2 /* OpenDevice: the VPU refused the service (no such server, or no common version) */
#define VQERR_TIMEOUT      3 /* OpenDevice/CloseDevice: no answer from the VPU */
#define VQERR_CLOSED       4 /* the VPU closed the service */
#define VQERR_TRUNCATED    5 /* CMD_READ: the message was longer than io_Length; io_Actual is its full size */

/* NSCMD_VQ_GETINFO.  May gain fields at its end: pass the size you compiled with, trust io_Actual. */
struct VchiqInfo
{
	ULONG vi_WindowBase;    /* the shared window (a block of cma.resource): address ... */
	ULONG vi_WindowSize;    /* ... and size in bytes */
	ULONG vi_Sessions;      /* sessions on this connection since the Pi started */
	ULONG vi_VersionCommon; /* VCHIQ protocol version agreed with the VPU */
	ULONG vi_Flags;         /* session record flags: 1 busy, 2 quiesced, 4 dirty, 8 connected */
	ULONG vi_RxPos;         /* byte position reached in the VPU's message stream */
	ULONG vi_TxPos;         /* byte position reached in ours */
	ULONG vi_MsgsTx;        /* messages sent, the transport's own included */
	ULONG vi_MsgsRx;        /* messages received, the transport's own included */
	ULONG vi_SlotsIn;       /* our slots the VPU returned */
	ULONG vi_SlotsOut;      /* VPU slots we returned */
	ULONG vi_BellRings;     /* doorbells rung to the VPU */
	ULONG vi_Interrupts;    /* doorbell interrupts from the VPU */
	ULONG vi_BulksDone;     /* bulk transfers the VPU reported complete or aborted */
	ULONG vi_Unexpected;    /* messages for no service, or out of protocol */
	ULONG vi_Dropped;       /* messages dropped (only while repairing a session) */
	ULONG vi_RxStalls;      /* times the device stopped reading for a client that did not read */
	ULONG vi_Services;      /* services open now */
	ULONG vi_Bringup;       /* how the connection came up at this boot's first open: VQBU_* */
	ULONG vi_ReachStart;    /* the memory the VPU's components address directly, [start, end): what the SMEM */
	ULONG vi_ReachEnd;      /* ... service imports (and 32-bit page lists name) */
	ULONG vi_ReachBus;      /* vi_ReachStart as the VPU addresses it: a CPU address there + (bus - start) */
	ULONG vi_UncachedStart; /* the RTG card's VRAM, [start, end): mapped so that a buffer there needs no cache */
	ULONG vi_UncachedEnd;   /* ... maintenance; 0, 0 if none */
};

/* vi_Bringup */
#define VQBU_FRESH 0    /* a fresh start: the VPU held no session of ours */
#define VQBU_RESUMED 1  /* the previous session, closed cleanly, taken up again */
#define VQBU_REPAIRED 2 /* the previous session, left in mid-flight (a crash), repaired */
#define VQBU_REINIT 3   /* the record was stale (the VPU had restarted): started afresh */

#endif /* DEVICES_VCHIQ_H */
