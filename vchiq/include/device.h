/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * device.h - vchiq.device internals.
 *
 * One device base holds one transport context (the connection to the VPU),
 * one task that walks the VPU's stream and serves the openers' requests, one
 * interrupt server for the doorbell, and one lock that serialises every touch
 * of the ring and of the openers' queues between the task and the callers.
 * Each OpenDevice() is a unit: one VCHIQ service and its request queues.
 */
#ifndef VCHIQ_DEVICE_H
#define VCHIQ_DEVICE_H

#define __NOLIBBASE__
#define EXEC_BASE_NAME SysBase /* a local in every function, from base->sysBase */

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#include <proto/exec.h>
#endif

#include <exec/libraries.h>
#include <exec/devices.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <exec/semaphores.h>
#include <exec/interrupts.h>
#include <exec/tasks.h>
#include <exec/lists.h>

#include <types.h>
#include <barrier.h>
#include <debug.h>
#include <drv_waitq.h>
#include <reset_guard.h>
#include <devices/vchiq.h>
#include <vchiq/vchiq_core.h>

#include <config.h>

#if defined(__INTELLISENSE__)
#define asm(x)
#define __attribute__(x)
#endif

struct VCHIQDevice;

/*
 * One opener: one service.  Requests are linked through io_Message.mn_Node
 * while they wait here; a bulk the core holds is on no list (the core's bulk
 * record carries it, and its completion comes back through the callback).
 */
struct VCHIQUnit
{
	struct Unit unit;    /* io_Unit points here */
	struct MinNode node; /* on base->units */
	struct VCHIQDevice *base;
	struct vchiq_service *svc; /* NULL: an information-only unit */
	struct MinList readq;      /* CMD_READ, waiting for a message from the VPU */
	struct MinList sendq;      /* CMD_WRITE (DATA) and NSCMD_VQ_BULKWRITE (BULK_TX), in issue order, not yet with the core */
	struct MinList bulkreadq;  /* NSCMD_VQ_BULKREAD (BULK_RX), in issue order, not yet with the core: a transmit
	                              waiting for bulk room never holds up a receive (the VPU may need that first) */
	u8 closed;                 /* the VPU closed the service: every request fails VQERR_CLOSED */
};

#define UNIT_FROM_NODE(n) ((struct VCHIQUnit *)((u8 *)(n) - __builtin_offsetof(struct VCHIQUnit, node)))

struct VCHIQDevice
{
	/* the Exec device */
	struct Device device;
	ULONG segList;
	struct ExecBase *sysBase; /* cached: $4 is an Amiga-bus read on PiStorm */
	APTR MailboxBase;         /* the firmware tag at a fresh start */
	APTR CMABase;             /* the window's allocator (cma.resource, vchiq_link.c) */

	/* hardware, at the first open: the window from cma.resource (vchiq_link.c), the rest from the device tree (devtree_parse.c) */
	volatile u8 *bell;
	s32 irq; /* the doorbell's GIC SPI; -1: none, the task polls */
	u32 window_base; /* the block of cma.resource's zone (VCHIQ_WINDOW_SIZE, page aligned); 0: none yet */
	u32 window_size;
	u32 window_bus; /* window_base as the VPU addresses it (/soc dma-ranges) */
	struct vchiq_platform platform; /* the SoC's page-list format and doorbell, from the node's compatible */
	u32 vram_start; /* the RTG card's VRAM, [start, end): needs no cache maintenance where the CPU allows; 0, 0 if none */
	u32 vram_end;

	/* the connection (vchiq_link.c) */
	struct vchiq_ctx ctx;
	BOOL up;     /* connection, task and interrupt server are in place */
	u32 bringup; /* how the connection came up: VQBU_* (NSCMD_VQ_GETINFO) */

	/* the lock, and what only it guards */
	struct SignalSemaphore lock; /* every touch of the ring, of the service table and of the units */
	struct drv_waitq waitq;      /* openers waiting for the VPU's answer */
	struct MinList units;        /* the openers */

	/* the reset path, which cannot take the lock (vchiq_ring_enter()) */
	volatile u8 halted;            /* the reset path owns the ring now; nobody else enters */
	volatile u32 ring_busy;        /* critical sections in progress; changed under the lock, read by the reset path */
	struct reset_guard resetGuard; /* installed at init, removed at expunge */
	volatile u8 test_abandon;      /* NSCMD_VQ_TEST_ABANDON (VCHIQ_TEST_HOOKS builds): the next reset leaves the session */

	/* the unit task (unit_task.c) */
	struct Task *task;
	BYTE irq_signal;      /* the task's wake-up: the doorbell, and new requests */
	u32 perf_reported_at; /* the task's last report of the bulk path's timings (profile tier) */

	/* the doorbell's interrupt server (irq.c) */
	struct Library *gic400Base;
	struct Interrupt isr;
	BOOL isr_installed;
	u32 interrupts; /* taken, for VCInfo COUNTERS */
};

/*
 * A critical section on the ring, lock held.  The lock keeps contexts apart;
 * the busy count lets the reset path (which cannot take the lock) see whether
 * anyone is inside.  Fails once the reset path has taken the ring over.
 *
 * No Disable(): every load and store of the two is one instruction, only lock
 * holders change the count, and the reset path only reads it, so the order
 * does the rest.  We count ourselves before we look at halted, the reset path
 * raises halted before it reads the count: either it sees us and waits, or we
 * see it and back out without touching the ring.  The barriers keep the ring's
 * accesses inside the section.
 */
static inline BOOL vchiq_ring_enter(struct VCHIQDevice *base)
{
	base->ring_busy++;
	emu68_barrier();
	if (base->halted)
	{
		base->ring_busy--;
		return FALSE;
	}
	return TRUE;
}

static inline void vchiq_ring_leave(struct VCHIQDevice *base)
{
	emu68_barrier();
	base->ring_busy--;
}

/* vchiq_link.c: the connection, at the first open and at expunge */
s32 vchiq_link_up(struct VCHIQDevice *base);     /* lock held; 0 or VQERR_NOCONNECTION */
void vchiq_link_down(struct VCHIQDevice *base);  /* lock not held */
void vchiq_link_reset(APTR user);                /* the reset guard's prepare(), user = the device */

/* devtree_parse.c */
u32 DevTreeParse(struct VCHIQDevice *base); /* 0, or ENODEV */

/* device_beginio.c, device_abortio.c */
void beginIO(struct IOVchiqReq *io asm("a1"), struct VCHIQDevice *base asm("a6"));
LONG abortIO(struct IOVchiqReq *io asm("a1"), struct VCHIQDevice *base asm("a6"));

/* unit.c: the openers' services, and their requests coming back */
struct VCHIQUnit *UnitOpen(struct VCHIQDevice *base, const struct IOVchiqReq *io, s32 *rc); /* lock held */
void UnitClose(struct VCHIQDevice *base, struct VCHIQUnit *unit);                           /* lock held */
void UnitReply(struct VCHIQDevice *base, struct IOVchiqReq *io, s32 error); /* 0, IOERR_* or VQERR_* */
void UnitFailRequests(struct VCHIQDevice *base, struct VCHIQUnit *unit, BYTE error); /* every waiting request back */

/* unit_task.c */
s32 UnitTaskStart(struct VCHIQDevice *base);
void UnitTaskStop(struct VCHIQDevice *base);
void UnitTaskKick(struct VCHIQDevice *base); /* a new request: the task serves it at its next round */
void UnitCallback(struct vchiq_ctx *ctx, struct vchiq_service *svc, u32 reason, const struct vchiq_bulk *bulk,
                  s32 bulk_actual); /* the core's, for every unit's service: the task, lock held */

/* irq.c */
s32 vchiq_int_enable(struct VCHIQDevice *base);
void vchiq_int_shutdown(struct VCHIQDevice *base);

#endif /* VCHIQ_DEVICE_H */
