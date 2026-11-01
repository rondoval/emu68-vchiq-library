/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * vchiq_core.h - host side of the VCHIQ transport.
 *
 * The client role of the VCHIQ protocol (after the Linux core, "GPL-2.0 OR
 * BSD-3-Clause", Copyright (c) 2010-2012 Broadcom, and its function names).
 * One context is the one connection to the VPU; vchiq.device multiplexes its
 * clients onto it, one service per unit.  The connection lives in a window of
 * non-cacheable memory the VPU reaches (a block of cma.resource's zone),
 * little-endian where shared.  Our bookkeeping is in the window too,
 * so the connection outlives an Amiga reset: the next session resumes, or
 * repairs what a dead one left (docs/architecture.md, 3.2).
 *
 * Window layout:
 *   +0                         host state (one page)
 *   +VCHIQ_SLOT_ZERO_OFFSET    slot zero + 64 data slots (VCHIQ_SLOT_MEM_SIZE)
 *   + VCHIQ_SLOT_MEM_SIZE      fragment pool for the VPU (VCHIQ_FRAG_MEM_SIZE)
 *   + VCHIQ_FRAG_MEM_SIZE      page lists, one slot per (service, direction, bulk)
 *
 * Using it:
 *   1. vchiq_get_platform(), the data range from /soc dma-ranges,
 *      vchiq_attach(); optionally ctx->uncached_* and ctx->timer.
 *   2. vchiq_connect(): fresh init, resume or repair, then CONNECT.
 *   3. vchiq_queue_service_open(), poll until the service leaves OPENING;
 *      messages and bulks; vchiq_queue_service_close(), poll until it leaves
 *      CLOSING, vchiq_free_service().  vchiq_open_service() and
 *      vchiq_close_service() do the same, blocking.
 *   4. Serve the doorbell: vchiq_disarm_events(), vchiq_poll() and the
 *      caller's work, vchiq_arm_events(); sleep only if vchiq_has_fired() is
 *      FALSE.  Without an interrupt, the same on a timer.
 *   5. vchiq_shutdown(), so the next session resumes; vchiq_mark_dirty() if
 *      the stream cannot be touched any more, so it repairs.
 *
 * Concurrency: none inside; the caller serialises.  The session calls and the
 * blocking helpers poll with a timeout in microseconds, sleeping between polls
 * on ctx->timer (a drv_timer of the calling task), or spinning without one
 * (the reset path cannot Wait()).  "Polls" below means the call runs
 * vchiq_poll() itself: callbacks, state changes of any service, slots coming
 * back.
 *
 * Callbacks run inside vchiq_poll(), for bulk completions and a service the
 * VPU closes; messages wait in their service's queue instead (up to
 * VCHIQ_RXQ_SIZE, then the walk stalls).  A callback may queue messages
 * (without @poll) and bulks, nothing that polls.
 *
 * A service the VPU answers on its sync slot is synchronous: its DATA goes
 * through the sync slots, one message per side at a time, queued and released
 * like any other.
 *
 * Return codes, as the device puts them in io_Error: 0, or
 *   VQERR_TIMEOUT       the VPU did not answer in time
 *   VQERR_REJECTED      the VPU refused the service (no such server, no common version)
 *   VQERR_CLOSED        the service is not open
 *   VQERR_NOCONNECTION  the connection is down, dirty, or could not be brought up
 *   IOERR_UNITBUSY      no room now: no free service, slot, bulk record or fragment
 *   IOERR_BADLENGTH     a message too long, a buffer the page list cannot describe
 *   IOERR_BADADDRESS    no buffer, a buffer the VPU cannot reach, or a window that cannot be used
 * Once the session is dirty (a corrupt stream or record), a call that queues
 * or waits returns VQERR_NOCONNECTION.
 */
#ifndef VCHIQ_CORE_H
#define VCHIQ_CORE_H

#include <exec/types.h>
#include <exec/errors.h>
#include <types.h>
#include <perf.h>

#include <devices/vchiq.h>
#include <vchiq/vchiq_protocol.h>

/* ---- the host record and the window ------------------------------------- */

#define VCHIQ_HS_MAGIC 0x56363851u /* 'V68Q' */
#define VCHIQ_HS_LAYOUT 3u /* bump on any change to struct vchiq_host_state; another layout's record is not used (power-cycle the Pi) */

/* host state flags */
#define VCHIQ_HS_BUSY 0x1u      /* a session is in progress: the state may be mid-update */
#define VCHIQ_HS_QUIESCED 0x2u  /* the last session ended cleanly: no services, no bulks, all messages released */
#define VCHIQ_HS_DIRTY 0x4u     /* something went wrong; repair or a power cycle is needed */
#define VCHIQ_HS_CONNECTED 0x8u /* CONNECT has been exchanged */

#define VCHIQ_SLOT_ZERO_OFFSET 4096u          /* slot zero's place: the host state's page comes first */
#define VCHIQ_MAX_LOCAL_SERVICES 16u          /* services open at once */
#define VCHIQ_RXQ_SIZE 128u /* messages held per service */

/* One page-list slot per (service, direction, bulk): 8 bytes + 4 per run of pages; a still picture's RGBA output is tens of MB. */
#define VCHIQ_PAGELIST_SLOT_SIZE 512u
#define VCHIQ_PAGELIST_MAX_ENTRIES ((VCHIQ_PAGELIST_SLOT_SIZE - 8u) / 4u) /* 126 runs: 126 MB on the BCM2711, 2 GB on the BCM2836/7 */
#define VCHIQ_PAGELIST_SLOTS (VCHIQ_MAX_LOCAL_SERVICES * 2u * VCHIQ_NUM_SERVICE_BULKS)
#define VCHIQ_PAGELIST_MEM_SIZE (VCHIQ_PAGELIST_SLOTS * VCHIQ_PAGELIST_SLOT_SIZE)
#define VCHIQ_AREA_SIZE (VCHIQ_SLOT_MEM_SIZE + VCHIQ_FRAG_MEM_SIZE + VCHIQ_PAGELIST_MEM_SIZE) /* from slot zero on */
#define VCHIQ_WINDOW_MIN_SIZE (VCHIQ_SLOT_ZERO_OFFSET + VCHIQ_AREA_SIZE)
#define VCHIQ_WINDOW_SIZE VCHIQ_WINDOW_MIN_SIZE /* what the device and the probe allocate: 84 pages, no spare area */
#define VCHIQ_SLOT_NONE 0xffffffffu           /* host state rx_slot / tx_slot: between slots */

/* ---- states and reasons ------------------------------------------------- */

/* What vchiq_classify() finds in the window. */
enum vchiq_window_state
{
	VCHIQ_STATE_FRESH = 0,      /* no session: initialise and send the firmware tag */
	VCHIQ_STATE_LIVE_CLEAN = 1, /* a quiesced connection: resume it */
	VCHIQ_STATE_LIVE_DIRTY = 2, /* a connection left mid-flight: repair it, or power cycle */
};

/* ctx->conn_state: the CONNECT exchange. */
enum vchiq_conn_state
{
	VCHIQ_CONN_DISCONNECTED = 0, /* no CONNECT sent */
	VCHIQ_CONN_CONNECTING = 1,   /* ours sent, the VPU's not yet seen */
	VCHIQ_CONN_CONNECTED = 2,    /* services may open */
};

/*
 * A service's life: FREE -> OPENING (OPEN sent) -> OPEN (OPENACK) ->
 * CLOSING (our CLOSE sent) -> CLOSED (the VPU's CLOSE) -> FREE
 * (vchiq_free_service()).  A refused OPEN goes OPENING -> CLOSED; a close by
 * the VPU goes OPEN -> CLOSED, with VCHIQ_REASON_SERVICE_CLOSED; an open
 * abandoned before its answer goes OPENING -> CLOSING, and the port stays
 * taken until the VPU's CLOSE.
 */
enum vchiq_service_state
{
	VCHIQ_SVC_FREE = 0,
	VCHIQ_SVC_OPENING = 1,
	VCHIQ_SVC_OPEN = 2,
	VCHIQ_SVC_CLOSING = 3,
	VCHIQ_SVC_CLOSED = 4,
};

enum vchiq_bulk_dir
{
	VCHIQ_BULK_TX = 0, /* host to VPU */
	VCHIQ_BULK_RX = 1, /* VPU to host */
};

/* Reasons passed to a service callback (messages are not: they wait in the service's queue). */
enum vchiq_reason
{
	VCHIQ_REASON_BULK_TRANSMIT_DONE = 2,    /* @bulk_actual bytes sent */
	VCHIQ_REASON_BULK_RECEIVE_DONE = 3,     /* @bulk_actual bytes received, the buffer's cache maintained */
	VCHIQ_REASON_BULK_TRANSMIT_ABORTED = 4, /* the VPU aborted it, or the service closed with it in flight */
	VCHIQ_REASON_BULK_RECEIVE_ABORTED = 5,  /* the same; the buffer's contents are undefined */
	VCHIQ_REASON_SERVICE_CLOSED = 6,        /* the VPU closed the service: it is CLOSED, our answer queued */
};

/* ---- the platform ------------------------------------------------------- */

/* What differs between the SoCs: from the vchiq node's compatible (vchiq_get_platform()) and dma-ranges. */
struct vchiq_platform
{
	u8 addr36;          /* BCM2711: page lists hold 36-bit physical page ids; else 32-bit bus addresses */
	u8 doorbell_gic;    /* the doorbell is a GIC SPI (BCM2711); else no interrupt the stack can take yet */
	u32 data_cpu_start; /* 32-bit page lists: the CPU range the VPU reaches, [start, end), through the */
	u32 data_cpu_end;   /* window's dma-ranges record (a bus address is the CPU one + window_bus - window) */
};

/*
 * The SoC part of @platform for a vchiq compatible ("brcm,bcm2711-vchiq",
 * "brcm,bcm2836-vchiq"), the data range cleared for the caller to fill from
 * /soc dma-ranges (only 32-bit page lists use it).  FALSE if not supported,
 * @platform untouched.
 */
BOOL vchiq_get_platform(const char *compatible, struct vchiq_platform *platform);

/* ---- the context -------------------------------------------------------- */

/*
 * Host-private state, persisted in the window (native byte order: only the
 * 68k reads it).  Everything needed to continue, or repair, a connection.
 */
struct vchiq_host_state
{
	u32 magic;                /* VCHIQ_HS_MAGIC */
	u32 layout;               /* VCHIQ_HS_LAYOUT */
	u32 flags;                /* VCHIQ_HS_* */
	u32 slot_zero_offset;     /* from the window base: VCHIQ_SLOT_ZERO_OFFSET, checked */
	u32 slot_zero_bus;        /* the VC bus address handed to the firmware */
	u32 rx_pos;               /* next byte to read in the VPU's stream */
	u32 tx_pos;               /* next byte to write in our stream; shared_ours->tx_pos is what the VPU has seen */
	u32 slot_queue_available; /* how many of our slots the queue holds (recycled ones included) */
	u32 rx_slot;              /* the VPU slot being read, or VCHIQ_SLOT_NONE */
	u32 tx_slot;              /* our slot being written, or VCHIQ_SLOT_NONE */
	u32 version_common;       /* the VCHIQ version the VPU settled on */
	u32 sessions;             /* how many sessions used this connection */
	u32 vpu_recycle_base;     /* shared_vpu->slot_queue_recycle as the VPU initialised it: minus, the slots we returned */
	u32 reserved[3];
	struct vchiq_slot_claims
	{
		u16 use_count;     /* claims: 1 while the walk is in the slot, +1 per message held */
		u16 release_count; /* claims given back; equal to use_count = the slot can be recycled */
	} slot_info[VCHIQ_MAX_SLOTS]; /* the VPU's slots, native-endian (struct vchiq_slot_info is the shared form) */
	u32 port_fourcc[VCHIQ_MAX_LOCAL_SERVICES]; /* open ports, for the repair of a dirty session */
	u16 port_remote[VCHIQ_MAX_LOCAL_SERVICES];
};

struct vchiq_ctx;
struct drv_timer;
struct vchiq_service;

/* A bulk in flight, as vchiq_queue_bulk() took it; the callback gets it back. */
struct vchiq_bulk
{
	APTR data;     /* the client's buffer */
	u32 size;      /* bytes asked for */
	APTR userdata; /* the client's, from vchiq_queue_bulk() */
	u8 fragment;   /* a receive not aligned to cache lines: its fragment slot + 1; 0 for none */
	u32 queued_at; /* when it went to the VPU (profile tier) */
};

/*
 * A service's callback (see the file header for its context).  @bulk for
 * the bulk reasons, NULL otherwise; @bulk_actual the bytes moved, or
 * VCHIQ_BULK_ACTUAL_ABORTED with an _ABORTED reason.  @bulk is only valid
 * during the call.
 */
typedef void (*vchiq_callback_t)(struct vchiq_ctx *ctx, struct vchiq_service *svc, u32 reason, const struct vchiq_bulk *bulk,
                                 s32 bulk_actual);

/* One entry of the service table.  The client reads state, sync, peer_version and userdata; the core writes them all. */
struct vchiq_service
{
	u32 fourcc;
	u16 localport;  /* our port: the service's index in ctx->services[] */
	u16 remoteport; /* the VPU's, from its OPENACK; VCHIQ_PORT_FREE before */
	u8 state;       /* enum vchiq_service_state */
	s16 peer_version; /* the VPU's service version, from its OPENACK */
	u8 sync; /* the VPU answered on its sync slot: our DATA goes through ours */
	vchiq_callback_t callback; /* bulk completions and the VPU's close; may be NULL */
	APTR userdata;             /* the client's, from vchiq_queue_service_open() */
	volatile struct vchiq_header *rxq[VCHIQ_RXQ_SIZE]; /* received messages, claimed; a full queue stops the walk */
	u32 rxq_head; /* next to hand out */
	u32 rxq_tail; /* next free */
	struct vchiq_bulk bulks[2][VCHIQ_NUM_SERVICE_BULKS]; /* [VCHIQ_BULK_TX], [VCHIQ_BULK_RX] */
	u32 bulk_head[2];
	u32 bulk_tail[2];
};

/* Counters since vchiq_attach(), for diagnostics only. */
struct vchiq_stats
{
	u32 msgs_tx;
	u32 msgs_rx;
	u32 slots_recycled_in;  /* our slots the VPU returned */
	u32 slots_recycled_out; /* VPU slots we returned */
	u32 bell_rings;
	u32 unexpected_msgs;
	u32 dropped_msgs;
	u32 rx_stalls; /* walks stopped at a message whose service queue was full */
	u32 bulks_done;
};

/* The bulk path's timings (perf.h, profile tier); the device reports them */
enum vchiq_perf_slot
{
	VQP_RX_BULK,  /* a receive, from queued at the VPU to its BULK_RX_DONE */
	VQP_RX_PREP,  /* the cache clean and invalidate before a receive that needs it */
	VQP_RX_INVAL, /* the invalidate after it */
	VQP_TX_CLEAN, /* the cache clean before a transmit that needs it */
	VQP_SLOT_COUNT
};

/*
 * One connection.  Caller-allocated, anywhere (it is not shared with the
 * VPU); vchiq_attach() fills it.  The caller may set uncached_start/end and
 * timer after that, and reads conn_state, services[], stats and host, and
 * reports perf; the rest is the core's to write.
 */
struct vchiq_ctx
{
	struct ExecBase *SysBase; /* for the cache maintenance of bulk buffers */
	struct drv_timer *timer;  /* an open timer of the calling task: the polled waits sleep on it; NULL: they spin */
	volatile u8 *window;
	u32 window_size;
	u32 window_bus; /* the window as the VPU addresses it (slot zero, page lists) */
	volatile u8 *bell;

	volatile struct vchiq_host_state *host;          /* private: only we read it (in the window to outlive a reset) */
	volatile struct vchiq_slot_zero *slot_zero;      /* shared with the VPU, little-endian */
	volatile struct vchiq_shared_state *shared_ours; /* slot zero's slave half: our stream, written by us */
	volatile struct vchiq_shared_state *shared_vpu;  /* slot zero's master half: the VPU's stream, written by it */
	volatile u8 *slots;
	volatile u8 *fragments; /* the VPU's pool for the unaligned ends of receives */
	volatile u8 *pagelists;
	u32 fragments_free[2]; /* one bit per fragment slot, set when free */
	u32 uncached_start, uncached_end; /* bulk buffers in here need no cache maintenance (Emu68's VRAM); 0, 0 for none */

	u8 conn_state;      /* enum vchiq_conn_state */
	u8 quiet;           /* no callbacks, the VPU's DATA dropped unread (shutdown, repair) */
	u8 sync_held;       /* the message in the VPU's sync slot sits in a service's queue */
	u8 sync_tx_waiting; /* a sync send found our sync slot taken: watch sync_release */
	struct vchiq_platform platform;
	u32 last_mailbox_value; /* what the firmware wrote back into the tag */
	struct vchiq_service services[VCHIQ_MAX_LOCAL_SERVICES];
	struct vchiq_stats stats;
	struct perf_counter perf_slots[VQP_SLOT_COUNT];
	struct perf perf; /* over perf_slots, bound by vchiq_attach() */
};

/* ---- the session (vchiq_session.c) -------------------------------------- */

/*
 * Bind @ctx to a window and the doorbell block: clears @ctx and points it at
 * the window's fixed layout (above); touches nothing in the window.
 * @window, @window_size: the window's CPU address and size, at least
 *     VCHIQ_WINDOW_MIN_SIZE, non-cacheable for both sides.
 * @window_bus: the window's VPU bus address, page aligned (the vchiq node's
 *     dma-ranges translation of @window).
 * @bell: the doorbell block (the vchiq node's reg); VCHIQ_BELL2 rings the VPU.
 * @platform: copied.
 * 0, or IOERR_BADADDRESS for a NULL pointer, an unaligned @window_bus or a
 * window too small.
 */
s32 vchiq_attach(struct vchiq_ctx *ctx, struct ExecBase *SysBase, volatile void *window, u32 window_size,
                 u32 window_bus, volatile void *bell, const struct vchiq_platform *platform);

/*
 * What the window holds (enum vchiq_window_state).  Reads the window only.
 * A record of another layout reads as FRESH: power-cycle the Pi before using
 * it.
 */
s32 vchiq_classify(struct vchiq_ctx *ctx);

/*
 * Bring the session up, as the window says (vchiq_classify()):
 *   FRESH       a new host state and slot zero, handed to the firmware (tag
 *               0x48010 through mailbox.resource; one per VPU boot, a second
 *               is refused), and a wait for the VPU to initialise its half;
 *   LIVE_CLEAN  the quiesced session's connection continued as it was;
 *   LIVE_DIRTY  the repair of a session that died mid-flight: the transmit
 *               side re-synchronised with what the VPU saw, the VPU slots it
 *               held returned, the ports it left open or opening closed and
 *               the stream drained, quiet (no callbacks, its messages
 *               dropped); the VPU keeps the connection, so no power cycle;
 * then the CONNECT exchange, unless the record says it happened.  Polls and
 * waits, up to @timeout_us per step (each port's close is one).  Every
 * service FREE after.  0 at once if this session is connected already.
 * On failure the session is marked dirty: VQERR_NOCONNECTION (the mailbox
 * call failed or the firmware refused the base; a record or a shared state
 * that cannot be used; a dead session that had not connected), VQERR_TIMEOUT,
 * IOERR_UNITBUSY (no slot for our CONNECT), or a repair close's error.
 * After a failed fresh init the firmware may own the area or not: power-cycle
 * the Pi (ctx->last_mailbox_value holds its answer to the tag).  A failed
 * repair is tried again by the next session.
 */
s32 vchiq_connect(struct vchiq_ctx *ctx, APTR MailboxBase, u32 timeout_us);

/*
 * End the session cleanly, so the next one resumes: quiet from here (no
 * callbacks, the VPU's DATA dropped), the doorbell events disarmed, every
 * service closed (an OPENING one abandoned) with vchiq_close_service() and
 * @timeout_us each, then a last poll so the VPU's slots go back, and the
 * host state QUIESCED, no longer BUSY.  The context is DISCONNECTED after.
 * 0, or, with the session marked dirty for a repair next time,
 * IOERR_UNITBUSY (a service did not close) or VQERR_NOCONNECTION (dirty).
 */
s32 vchiq_shutdown(struct vchiq_ctx *ctx, u32 timeout_us);

/* Give up on the session: the next one classifies LIVE_DIRTY and has to repair. */
void vchiq_mark_dirty(struct vchiq_ctx *ctx);

/* ---- the VPU's stream and the doorbell events (vchiq_rx.c) -------------- */

/*
 * Do everything the VPU has asked for: acknowledge the events, take back our
 * recycled slots, walk the VPU's stream and its sync slot.  The walk moves
 * services between states, queues DATA in their queues, finishes bulks and
 * runs the callbacks; it stops early at a message for a service whose queue
 * is full (vchiq_has_unwalked() then stays TRUE).  A CLOSE from the VPU is
 * answered from here, which may wait up to 100 ms for a slot.  Never
 * fails; a corrupt stream marks the session dirty.
 */
void vchiq_poll(struct vchiq_ctx *ctx);

/*
 * The doorbell events (the VPU rings only for an armed event).  Arm before
 * the caller sleeps, then check vchiq_has_fired(): TRUE means something came
 * between the last poll and the arming, so poll again instead of sleeping.
 * Disarm while running and when nobody is left to serve the doorbell.
 * Armed: a message on either channel, a recycled slot, and, only while a
 * sync send waits, our sync slot coming free.
 */
void vchiq_arm_events(struct vchiq_ctx *ctx);
void vchiq_disarm_events(struct vchiq_ctx *ctx);
BOOL vchiq_has_fired(const struct vchiq_ctx *ctx);

/*
 * The VPU has sent more than has been walked: a walk stalled on a full
 * queue (poll again once the client has taken messages), or a doorbell not
 * yet taken.  FALSE without a connected session.
 */
BOOL vchiq_has_unwalked(const struct vchiq_ctx *ctx);

/* ---- services (vchiq_service.c) ----------------------------------------- */

/*
 * Ask the VPU for service @fourcc, versions @version_min..@version: take a
 * FREE entry, make it OPENING and queue OPEN; does not poll or wait.  A later
 * poll moves it to OPEN (svc->sync and svc->peer_version set) or, refused,
 * to CLOSED.  @callback (may be NULL) and @userdata are kept in the service.
 * *@out is set on success only.  0, VQERR_NOCONNECTION (not connected),
 * IOERR_UNITBUSY (no free entry, or no room in our stream).  To abandon an
 * OPENING service, close it (vchiq_queue_service_close()).
 */
s32 vchiq_queue_service_open(struct vchiq_ctx *ctx, u32 fourcc, s16 version, s16 version_min,
                             vchiq_callback_t callback, APTR userdata, struct vchiq_service **out);

/*
 * Ask the VPU to close an OPEN service: queue CLOSE, release its held
 * messages, make it CLOSING; does not poll or wait.  Its bulks stay in flight
 * until the VPU's CLOSE, whose poll reports any left as aborted and makes the
 * service CLOSED.  An OPENING service is abandoned the same way: the
 * VPU closes what it opened, or its refusal comes, and either is the CLOSE
 * that ends CLOSING; until then the port is not reused, so a late OPENACK
 * cannot open the next service on it.  0 also when already CLOSING or
 * CLOSED; VQERR_CLOSED when FREE; IOERR_UNITBUSY (no room: nothing
 * changes); VQERR_NOCONNECTION.
 */
s32 vchiq_queue_service_close(struct vchiq_ctx *ctx, struct vchiq_service *svc);

/* A CLOSED service back to the table (FREE); in any other state, nothing. */
void vchiq_free_service(struct vchiq_service *svc);

/*
 * Blocking open, for polled users (the probe): request, then poll up to
 * @timeout_us for the answer.  No callback.  0 with *@out
 * set, or the request's error, VQERR_TIMEOUT, VQERR_REJECTED (refused).  On
 * failure the service is closed with vchiq_close_service() and @timeout_us:
 * FREE again, or CLOSING if the VPU stays silent.
 */
s32 vchiq_open_service(struct vchiq_ctx *ctx, u32 fourcc, s16 version, s16 version_min,
                       struct vchiq_service **out, u32 timeout_us);

/*
 * Blocking close: request, poll up to @timeout_us for the VPU's CLOSE, free.
 * 0, or the request's error or VQERR_TIMEOUT; a timeout leaves the service
 * CLOSING, so vchiq_shutdown() cannot quiesce.
 */
s32 vchiq_close_service(struct vchiq_ctx *ctx, struct vchiq_service *svc, u32 timeout_us);

/* ---- messages (vchiq_service.c) ----------------------------------------- */

/*
 * Queue a DATA message of @size bytes (up to VCHIQ_MAX_MSG_SIZE) on an OPEN
 * service, through our sync slot if the service is synchronous.  @data is
 * copied; the call returns with the message published and the VPU rung.
 * @timeout_us bounds the wait for room: 0 fails at once with
 * IOERR_UNITBUSY.  With @poll the wait walks the VPU's stream (callbacks run
 * in the caller's context); without it only our recycled slots are taken
 * back, which is what a client task wants.  A sync send that finds the slot
 * taken polls only with @poll; without, IOERR_UNITBUSY, and the next
 * vchiq_arm_events() asks for the doorbell that frees it.  0, VQERR_CLOSED,
 * IOERR_BADLENGTH, IOERR_UNITBUSY, VQERR_NOCONNECTION.
 */
s32 vchiq_queue_message(struct vchiq_ctx *ctx, struct vchiq_service *svc, const void *data, u32 size,
                        u32 timeout_us, BOOL poll);

/*
 * The service's next received message, without polling; NULL when its queue
 * is empty.  The message stays in the VPU's slot, claimed: le32(hdr->size)
 * payload bytes at hdr->data, valid until vchiq_release_message().  Messages
 * come in the VPU's order; a synchronous service's next one comes only
 * after this one is released.
 */
volatile struct vchiq_header *vchiq_take_message(struct vchiq_service *svc);

/*
 * vchiq_take_message(), polling up to @timeout_us for a message.  NULL on a
 * timeout, on a dirty session, and once the service has left OPEN with its
 * queue empty.
 */
volatile struct vchiq_header *vchiq_dequeue_message(struct vchiq_ctx *ctx, struct vchiq_service *svc, u32 timeout_us);

/*
 * Release a taken message, of either channel, in any order.  A VPU data slot
 * goes back to the VPU once every message in it is released, the VPU's sync
 * slot at once.  Releasing a data message twice does nothing.
 */
void vchiq_release_message(struct vchiq_ctx *ctx, volatile struct vchiq_header *hdr);

/* ---- bulk transfers (vchiq_bulk.c) -------------------------------------- */

/*
 * Queue a bulk transfer on an OPEN service: @dir VCHIQ_BULK_TX sends @size
 * bytes at @data to the VPU, VCHIQ_BULK_RX receives up to @size into it.  The
 * buffer is physically contiguous (virtual == physical), of any alignment,
 * and is the VPU's until the callback reports the bulk with @userdata in its
 * vchiq_bulk; the core does its cache maintenance.  Does not poll or wait.
 * The request goes into our stream like DATA, so the VPU sees it in the order
 * it was queued with the service's messages; transfers of one direction
 * complete in order.
 *
 * A receive whose ends are not on cache lines gets those ends through the
 * VPU's fragment pool (the DMA writes whole lines only; the host copies the
 * partial ones in afterwards), so neighbouring data
 * in the same lines is never touched.
 *
 * 0, VQERR_CLOSED, IOERR_BADADDRESS (NULL @data; on 32-bit platforms a buffer
 * outside the VPU's data range), IOERR_BADLENGTH (@size 0, a bad @dir, a
 * buffer of more than VCHIQ_PAGELIST_MAX_ENTRIES runs of pages),
 * IOERR_UNITBUSY (VCHIQ_NUM_SERVICE_BULKS transfers of @dir in flight on the
 * service, no free fragment slot, no room in our stream), VQERR_NOCONNECTION.
 */
s32 vchiq_queue_bulk(struct vchiq_ctx *ctx, struct vchiq_service *svc, u32 dir, APTR data, u32 size, APTR userdata);

#endif /* VCHIQ_CORE_H */
