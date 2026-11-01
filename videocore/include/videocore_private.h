/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * videocore_private.h - videocore.codec internals.
 *
 * One library base holds the MMAL client's state under mmal_lock, the SMEM
 * client's (imports into the VPU), where the VPU reaches host memory, and
 * the link to vchiq.device: the opened "mmal" service and "SMEM" beside it,
 * a pool of requests, and a receive task that owns the requests' reply port.
 * The receive task handles every request that comes back (incoming
 * messages, bulk transfers, failed writes) under mmal_lock; MMAL calls send
 * with SendIO() and wait for their reply under the same lock with drv_waitq.
 * vchiq.device never calls into this library, so there is no lock order
 * across the two.  The service opens with the first component or image
 * call and stays open until VCB_Idle() (mmal_idle()).
 *
 * Above the MMAL client sit the sessions of the videocodec backend ABI,
 * serialised by session_lock.  The MMAL calls take mmal_lock inside and may
 * sleep with it released; session_lock stays held across.  The image calls
 * queue on image_lock, taken before either.
 */
#ifndef VIDEOCORE_PRIVATE_H
#define VIDEOCORE_PRIVATE_H

#define __NOLIBBASE__
#define EXEC_BASE_NAME SysBase /* a local in every function, from base->SysBase */

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#include <proto/exec.h>
#endif

#include <exec/libraries.h>
#include <exec/semaphores.h>
#include <exec/tasks.h>
#include <exec/lists.h>
#include <exec/ports.h>

#include <types.h>
#include <bits.h>
#include <debug.h>
#include <dma_mem.h>
#include <drv_waitq.h>
#include <perf.h>
#include <reset_guard.h>
#include <devices/vchiq.h>
#include <libraries/videocodec_backend.h>
#include <mmal.h>

#include <mmal_client.h>
#include <smem_client.h>
#include <videocore_link.h>
#include <videocore_session.h>
#include <videocore_gencmd.h>

#if defined(__INTELLISENSE__)
#define asm(x)
#define __attribute__(x)
#endif

/* These are overridden by cmake */
#ifndef LIBRARY_NAME
#define LIBRARY_NAME "videocore.codec"
#endif
/* The version comes from CMake (the component's one version, root CMakeLists.txt); an editor's parser gets placeholders. */
#if !defined(LIBRARY_VERSION) || !defined(LIBRARY_REVISION) || !defined(LIBRARY_IDSTRING)
#ifdef __INTELLISENSE__
#ifndef LIBRARY_VERSION
#define LIBRARY_VERSION 0
#endif
#ifndef LIBRARY_REVISION
#define LIBRARY_REVISION 0
#endif
#ifndef LIBRARY_IDSTRING
#define LIBRARY_IDSTRING "videocore.codec"
#endif
#else
#error "LIBRARY_VERSION, LIBRARY_REVISION and LIBRARY_IDSTRING are set by CMake"
#endif
#endif
#ifndef LIBRARY_PRIORITY
#define LIBRARY_PRIORITY 0
#endif

/* The frame path's timings (perf.h, profile tier); the library binds them at init, the sessions report them */
enum videocore_perf_slot
{
	VCPERF_RX_PARKED,  /* a payload's bulk receive, from its buffer's header to the request going out (mmal_pump()) */
	VCPERF_RX_PAYLOAD, /* ... from its buffer's header to the payload landed */
	VCPERF_OUT_VPU,    /* an output object, from sent to back from the VPU */
	VCPERF_OUT_CLIENT, /* an output object, from given to the client to released */
	VCPERF_IN_VPU,     /* an input object, from sent to back from the VPU */
	VCPERF_POLL,       /* one session_poll() */
	VCPERF_IMG_CALL,   /* one image call (VCB_Image()), whole */
	VCPERF_IMG_OPEN,   /* ... one of its sessions opened */
	VCPERF_IMG_WAIT,   /* ... and waited for: the picture made */
	VCPERF_IMG_CLOSE,  /* ... and closed */
	VCPERF_MMAL_OPEN,  /* the "mmal" service opened, for the first component or image call */
	VCPERF_MMAL_CLOSE, /* ... and closed, at VCB_Idle() */
	VCPERF_COMPONENT,  /* a component created and its ports read */
	VCPERF_DESTROY,    /* a component stopped and destroyed */
	VCPERF_IMPORT,     /* a port's buffers imported into the VPU (SMEM), one round trip */
	VCPERF_RELEASE,    /* ... and the wait for the VPU to report them released */
	VCPERF_SLOT_COUNT
};

struct VideoCoreBase
{
	struct Library lib;
	ULONG segList;
	struct ExecBase *SysBase;
	struct SignalSemaphore mmal_lock; /* every MMAL structure, the imports, the reach and the link */
	struct drv_waitq waitq;           /* callers waiting for replies, buffers, free requests (under mmal_lock) */
	struct dma_mem_ctx dma;
	struct mmal_client mmal;
	struct smem_client smem; /* imports into the VPU (SMEM), under mmal_lock */
	struct vc_reach reach;   /* where the VPU reaches host memory (mmal_zero_copy.c) */
	struct link link;
	struct SignalSemaphore session_lock; /* the sessions: every VCB_* call on a session holds it throughout */
	struct SignalSemaphore image_lock;   /* the image calls: one on the VPU at a time, VCB_Image() holds it throughout */
	APTR still_frame;                    /* the frame an image call took for its picture, until the format change (still_fits()) */
	struct MinList sessions;
	u32 session_count; /* written under session_lock; read without a lock by VCB_GetInfo() */
	struct perf_counter perf_slots[VCPERF_SLOT_COUNT];
	struct perf perf;     /* over perf_slots, bound at init (videocore_lib.c) */
	u32 perf_reported_at; /* the last report, from session_poll() (under session_lock) */
	struct reset_guard reset_guard; /* while the service is open: sessions_reset() */
	BOOL reset_guarded;
};

/* the backend's functions (videocore_backend.c; VCB_Image() in videocore_image.c) */
LONG VCB_GetInfo(struct VCInfo *info asm("a0"), struct VideoCoreBase *base asm("a6"));
LONG VCB_QueryCodec(ULONG index asm("d0"), struct VCCodecInfo *info asm("a0"), struct VideoCoreBase *base asm("a6"));
APTR VCB_OpenSession(struct VCSessionParams *params asm("a0"), LONG *error asm("a1"), struct VideoCoreBase *base asm("a6"));
VOID VCB_CloseSession(APTR handle asm("a0"), struct VideoCoreBase *base asm("a6"));
struct VCPacket *VCB_ImportPacket(APTR handle asm("a0"), ULONG side asm("d0"), APTR data asm("a1"), ULONG capacity asm("d1"),
                                  struct VideoCoreBase *base asm("a6"));
struct VCFrame *VCB_ImportFrame(APTR handle asm("a0"), ULONG side asm("d0"), APTR data asm("a1"), struct VideoCoreBase *base asm("a6"));
LONG VCB_SendPacket(APTR handle asm("a0"), struct VCPacket *packet asm("a1"), struct VideoCoreBase *base asm("a6"));
LONG VCB_SendFrame(APTR handle asm("a0"), struct VCFrame *frame asm("a1"), struct VideoCoreBase *base asm("a6"));
LONG VCB_Poll(APTR handle asm("a0"), struct VCItem *item asm("a1"), struct VideoCoreBase *base asm("a6"));
VOID VCB_ReleaseFrame(APTR handle asm("a0"), struct VCFrame *frame asm("a1"), struct VideoCoreBase *base asm("a6"));
VOID VCB_ReleasePacket(APTR handle asm("a0"), struct VCPacket *packet asm("a1"), struct VideoCoreBase *base asm("a6"));
LONG VCB_Flush(APTR handle asm("a0"), struct VideoCoreBase *base asm("a6"));
LONG VCB_SetAttrs(APTR handle asm("a0"), struct VCSessionParams *params asm("a1"), struct VideoCoreBase *base asm("a6"));
LONG VCB_Image(struct VCImageParams *params asm("a0"), struct VideoCoreBase *base asm("a6"));
VOID VCB_Idle(struct VideoCoreBase *base asm("a6"));

#endif /* VIDEOCORE_PRIVATE_H */
