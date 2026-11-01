/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * vchiq_core_priv.h - what the core's modules share and nobody else sees:
 * the common includes, the window and cache-line constants, the accessors of
 * the shared state, and the vq_* functions one module calls in another.
 * Included by vchiq_session.c, vchiq_rx.c, vchiq_tx.c, vchiq_service.c and
 * vchiq_bulk.c only.
 */
#ifndef VCHIQ_CORE_PRIV_H
#define VCHIQ_CORE_PRIV_H

#define __NOLIBBASE__
#define EXEC_BASE_NAME SysBase

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#include <proto/exec.h> /* CachePreDMA()/CachePostDMA() in the LVO cache-op flavour */
#endif

#include <barrier.h>
#include <byteorder.h>
#include <debug.h>
#include <iomem.h>
#include <memory.h>
#include <strutil.h>
#include <timing.h>
#include <drv_timer.h>

#include <vchiq/vchiq_core.h>

#define POLL_STEP_US 50u                        /* a polled wait's step without a timer: a spin */
#define POLL_STEP_MS 1u                         /* a polled wait's step on ctx->timer: a sleep */
#define PAGE_SIZE 4096u                         /* the page of the page lists */
#define PAGE_MASK (PAGE_SIZE - 1u)
#define LINE_MASK (VCHIQ_CACHE_LINE_SIZE - 1u) /* a receive's ends inside a line go through a fragment */

/* ---- accessors of the shared state -------------------------------------- */

/* CPU address to VPU bus address: the window's dma-ranges record maps every buffer the VPU reaches (32-bit page lists). */
static inline u32 bus_offset(const struct vchiq_ctx *ctx)
{
	return ctx->window_bus - (u32)ctx->window;
}

/* @addr's offset in its cache line. */
static inline u32 line_offset(u32 addr)
{
	return addr & LINE_MASK;
}

/* The start of slot @slot; slot 0 is slot zero itself. */
static inline volatile u8 *slot_base(const struct vchiq_ctx *ctx, u32 slot)
{
	return ctx->slots + slot * VCHIQ_SLOT_SIZE;
}

/* The slot @p points into. */
static inline u32 slot_index_of(const struct vchiq_ctx *ctx, const volatile void *p)
{
	return (u32)((const volatile u8 *)p - ctx->slots) / VCHIQ_SLOT_SIZE;
}

/* Entry @i of a side's circular slot queue. */
static inline u32 queue_entry(const volatile struct vchiq_shared_state *s, u32 i)
{
	return le32(s->slot_queue[i & VCHIQ_SLOT_QUEUE_MASK]);
}

/* The slot that holds byte @pos of a side's stream: stream slot q is queue entry q. */
static inline u32 stream_slot(const volatile struct vchiq_shared_state *s, u32 pos)
{
	return queue_entry(s, VCHIQ_SLOT_QUEUE_INDEX(pos));
}

/* The message at stream position @pos, in slot @slot. */
static inline volatile struct vchiq_header *header_at(const struct vchiq_ctx *ctx, u32 slot, u32 pos)
{
	return (volatile struct vchiq_header *)(slot_base(ctx, slot) + VCHIQ_SLOT_OFFSET(pos));
}

/* The message slot of a side's synchronous channel. */
static inline volatile struct vchiq_header *sync_header(const struct vchiq_ctx *ctx, const volatile struct vchiq_shared_state *s)
{
	return (volatile struct vchiq_header *)slot_base(ctx, le32(s->slot_sync));
}

/* One step of a polled wait: sleep on the caller's timer, or spin without one (the reset path cannot Wait()). */
static inline void wait_step(const struct vchiq_ctx *ctx)
{
	if (ctx->timer != NULL)
		drv_timer_sleep_ms(ctx->timer, POLL_STEP_MS);
	else
		delay_us(POLL_STEP_US);
}

/* Our OPEN is unanswered: OPENING, or abandoned (CLOSING) before the VPU's port came. */
static inline BOOL is_open_pending(const struct vchiq_service *svc)
{
	return svc->state == VCHIQ_SVC_OPENING || (svc->state == VCHIQ_SVC_CLOSING && svc->remoteport == VCHIQ_PORT_FREE);
}

/* ---- shared between the modules ----------------------------------------- */

/* vchiq_rx.c */

/* Append VPU slot @slot to the VPU's recycle queue; the caller signals shared_vpu->recycle. */
void vq_recycle_slot(struct vchiq_ctx *ctx, u32 slot);

/*
 * Release one claim on VPU slot @slot: @hdr's (its CLAIMED bit; a second
 * release does nothing), or with NULL the walk's own.  The last one recycles
 * the slot and signals the VPU.
 */
void vq_release_slot(struct vchiq_ctx *ctx, u32 slot, volatile struct vchiq_header *hdr);

/* The VPU's sync slot is free again: PADDING written, sync_release signalled. */
void vq_release_message_sync(struct vchiq_ctx *ctx);

/* Take back our slots the VPU has recycled (shared_ours->slot_queue_recycle). */
void vq_process_free_queue(struct vchiq_ctx *ctx);

/*
 * Poll until @done(ctx, @arg), a wait_step() between polls: 0,
 * VQERR_NOCONNECTION once the session is dirty, VQERR_TIMEOUT after
 * @timeout_us.  The wait of every blocking call.
 */
s32 vq_poll_until(struct vchiq_ctx *ctx, BOOL (*done)(const struct vchiq_ctx *, const void *), const void *arg,
                  u32 timeout_us);

/* vchiq_tx.c */

/* Raise @ev (one of the VPU's events) and ring the doorbell if it is armed or @force.  Returns the armed flag. */
BOOL vq_remote_event_signal(struct vchiq_ctx *ctx, volatile struct vchiq_remote_event *ev, BOOL force);

/*
 * One message of any type (@msgid from VCHIQ_MAKE_MSG()) into our stream:
 * room reserved (waiting as vchiq_queue_message() describes for @timeout_us
 * and @poll), @size bytes of @data copied, published, the VPU rung.  0,
 * IOERR_BADLENGTH, IOERR_UNITBUSY, VQERR_NOCONNECTION.
 */
s32 vq_queue_message(struct vchiq_ctx *ctx, u32 msgid, const void *data, u32 size, u32 timeout_us, BOOL poll);

/*
 * The same through our sync slot.  A slot still taken is waited for only
 * with @poll and @timeout_us; else IOERR_UNITBUSY, with sync_tx_waiting set
 * so the next vchiq_arm_events() asks for the doorbell that frees it.
 */
s32 vq_queue_message_sync(struct vchiq_ctx *ctx, u32 msgid, const void *data, u32 size, u32 timeout_us, BOOL poll);

/* vchiq_service.c */

/* The service on our port @port; NULL if out of range or FREE. */
struct vchiq_service *vq_find_service_by_port(struct vchiq_ctx *ctx, u32 port);

/* The service on our port @dst whose VPU end is @src: a message it may receive.  NULL if none. */
struct vchiq_service *vq_find_peer_service(struct vchiq_ctx *ctx, u32 dst, u32 src);

/* Every service FREE and empty: a new session. */
void vq_reset_services(struct vchiq_ctx *ctx);

/* The host-state record of @svc's port, set (@open) or cleared: what a repair closes. */
void vq_record_port(struct vchiq_ctx *ctx, const struct vchiq_service *svc, BOOL open);

/* @svc's callback with @reason, unless it has none or the session is quiet. */
void vq_make_service_callback(struct vchiq_ctx *ctx, struct vchiq_service *svc, u32 reason, const struct vchiq_bulk *bulk,
                              s32 bulk_actual);

/*
 * An OPENACK from the VPU's port @src (@hdr, @size bytes; on its sync slot
 * if @sync): the remote port recorded, the service OPEN with the peer's
 * version, or, abandoned mid-open, left CLOSING for the CLOSE to come.
 */
void vq_set_service_open(struct vchiq_ctx *ctx, struct vchiq_service *svc, u32 src, volatile struct vchiq_header *hdr,
                         u32 size, BOOL sync);

/* The VPU's CLOSE: held messages released, bulks reported aborted, the record cleared, the service CLOSED. */
void vq_close_service_complete(struct vchiq_ctx *ctx, struct vchiq_service *svc);

/* vchiq_bulk.c */

/* The oldest bulk of @dir in flight on @svc, off its queue; NULL if none.  Bulks complete in order. */
struct vchiq_bulk *vq_pop_bulk(struct vchiq_service *svc, u32 dir);

/*
 * Bulk @bulk is over: @actual bytes, or VCHIQ_BULK_ACTUAL_ABORTED.  A receive
 * gets its cache lines dropped and its partial lines copied in from the
 * fragment slot; the fragment goes back; the callback reports it.
 */
void vq_finish_bulk(struct vchiq_ctx *ctx, struct vchiq_service *svc, u32 dir, struct vchiq_bulk *bulk, s32 actual);

#endif /* VCHIQ_CORE_PRIV_H */
