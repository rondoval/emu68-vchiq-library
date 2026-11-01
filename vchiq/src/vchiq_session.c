/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * vchiq_session.c - the session: the platform table, binding a context to a window, what the
 * window holds, a fresh start (slot zero, the firmware tag), resume, repair,
 * CONNECT, and the session's clean end.
 */
#include <vchiq/vchiq_core_priv.h> /* first: __NOLIBBASE__ for the proto header below */

#ifdef __INTELLISENSE__
#include <clib/mailbox_protos.h>
#else
#include <proto/mailbox.h>
#endif

/* ---- the platform and the window ----------------------------------------- */

/* The SoCs the core runs on, and what differs between them. */
BOOL vchiq_get_platform(const char *compatible, struct vchiq_platform *platform)
{
	static const struct
	{
		const char *compatible;
		u8 addr36;
		u8 doorbell_gic;
	} socs[] = {
	    {"brcm,bcm2711-vchiq", 1, 1}, /* Pi 4, CM4, Pi 400: Cortex-A72 */
	    {"brcm,bcm2836-vchiq", 0, 0}, /* Pi 2 (v1.2), Pi 3, CM3, Zero 2 W: Cortex-A53 */
	};

	for (u32 i = 0; i < sizeof(socs) / sizeof(socs[0]); i++)
	{
		if (strcmp(socs[i].compatible, compatible) == 0)
		{
			*platform = (struct vchiq_platform){.addr36 = socs[i].addr36, .doorbell_gic = socs[i].doorbell_gic};
			return TRUE;
		}
	}
	return FALSE;
}

/* The perf slots' names, as enum vchiq_perf_slot orders them */
static const char *const perf_names[VQP_SLOT_COUNT] = {"rx_bulk", "rx_prep", "rx_inval", "tx_clean"};

/* Bind the context to the window and the doorbell; the window itself is not touched. */
s32 vchiq_attach(struct vchiq_ctx *ctx, struct ExecBase *SysBase, volatile void *window, u32 window_size,
                 u32 window_bus, volatile void *bell, const struct vchiq_platform *platform)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->perf = (struct perf){"vchiq", perf_names, ctx->perf_slots, VQP_SLOT_COUNT};
	if (window == NULL || bell == NULL || platform == NULL || (window_bus & PAGE_MASK) != 0 ||
	    window_size < VCHIQ_WINDOW_MIN_SIZE)
		return IOERR_BADADDRESS;

	ctx->SysBase = SysBase;
	ctx->platform = *platform;
	ctx->window = (volatile u8 *)window;
	ctx->window_size = window_size;
	ctx->window_bus = window_bus;
	ctx->bell = (volatile u8 *)bell;

	/* the window's fixed layout: the host state, then slot zero and the data slots, the fragment pool, the page lists */
	volatile u8 *area = ctx->window + VCHIQ_SLOT_ZERO_OFFSET;
	ctx->host = (volatile struct vchiq_host_state *)window;
	ctx->slot_zero = (volatile struct vchiq_slot_zero *)area;
	ctx->shared_ours = &ctx->slot_zero->slave;
	ctx->shared_vpu = &ctx->slot_zero->master;
	ctx->slots = area; /* slot 0 is slot zero itself */
	ctx->fragments = area + VCHIQ_SLOT_MEM_SIZE;
	ctx->pagelists = area + VCHIQ_SLOT_MEM_SIZE + VCHIQ_FRAG_MEM_SIZE;
	return 0;
}

/* The record's slot zero is in its place, and both sides have initialised it. */
static BOOL is_slot_zero_live(const struct vchiq_ctx *ctx)
{
	return ctx->host->slot_zero_offset == VCHIQ_SLOT_ZERO_OFFSET && le32(ctx->slot_zero->magic) == VCHIQ_MAGIC &&
	       le32(ctx->shared_ours->initialised) != 0 && le32(ctx->shared_vpu->initialised) != 0;
}

/* What the window holds: no record (FRESH), a quiesced session (LIVE_CLEAN), or a dead one (LIVE_DIRTY). */
s32 vchiq_classify(struct vchiq_ctx *ctx)
{
	volatile struct vchiq_host_state *host = ctx->host;
	if (host->magic != VCHIQ_HS_MAGIC || host->layout != VCHIQ_HS_LAYOUT)
		return VCHIQ_STATE_FRESH; /* none, or one this build cannot read */
	if (!is_slot_zero_live(ctx))
		return VCHIQ_STATE_LIVE_DIRTY; /* the record names a slot zero that is not (or no longer) there */
	if ((host->flags & (VCHIQ_HS_BUSY | VCHIQ_HS_DIRTY | VCHIQ_HS_QUIESCED)) != VCHIQ_HS_QUIESCED)
		return VCHIQ_STATE_LIVE_DIRTY; /* the last session did not end cleanly */
	return VCHIQ_STATE_LIVE_CLEAN;
}

/* ---- a fresh start ------------------------------------------------------- */

/* The VPU's data slots, as init_slot_zero() lays them out (its sync slot comes first). */
#define VPU_SLOT_FIRST (VCHIQ_SLOT_ZERO_SLOTS + 1u)
#define VPU_SLOT_LAST (VCHIQ_SLOT_ZERO_SLOTS + VCHIQ_SLOTS_PER_SIDE - 1u)

/* Lay out slot zero: the shared fields, both sides' slot ranges, and our half. */
static void init_slot_zero(struct vchiq_ctx *ctx, u32 bus)
{
	volatile struct vchiq_slot_zero *slot_zero = ctx->slot_zero;

	memset((void *)slot_zero, 0, sizeof(*slot_zero));
	slot_zero->magic = le32(VCHIQ_MAGIC);
	slot_zero->version = le16(VCHIQ_VERSION);
	slot_zero->version_min = le16(VCHIQ_VERSION_MIN);
	slot_zero->slot_zero_size = le32(sizeof(*slot_zero));
	slot_zero->slot_size = le32(VCHIQ_SLOT_SIZE);
	slot_zero->max_slots = le32(VCHIQ_MAX_SLOTS);
	slot_zero->max_slots_per_side = le32(VCHIQ_MAX_SLOTS_PER_SIDE);
	slot_zero->platform_data[0] = le32(bus + VCHIQ_SLOT_MEM_SIZE); /* fragment pool */
	slot_zero->platform_data[1] = le32(VCHIQ_MAX_FRAGMENTS);

	/* each side: a sync slot, then its data slots */
	u32 first = VCHIQ_SLOT_ZERO_SLOTS;
	u32 half = VCHIQ_SLOTS_PER_SIDE;
	slot_zero->master.slot_sync = le32(first);
	slot_zero->master.slot_first = le32(VPU_SLOT_FIRST);
	slot_zero->master.slot_last = le32(VPU_SLOT_LAST);
	slot_zero->slave.slot_sync = le32(first + half);
	slot_zero->slave.slot_first = le32(first + half + 1u);
	slot_zero->slave.slot_last = le32(first + 2u * half - 1u);

	/* our half: every data slot free in the queue, no event armed */
	volatile struct vchiq_shared_state *shared_ours = ctx->shared_ours;
	u32 n = 0;
	for (u32 slot = le32(shared_ours->slot_first); slot <= le32(shared_ours->slot_last); slot++)
		shared_ours->slot_queue[n++] = le32(slot);
	ctx->host->slot_queue_available = n;
	shared_ours->slot_queue_recycle = le32(n);
	shared_ours->sync_release.fired = le32(1); /* the sync slot starts out free */

	volatile struct vchiq_header *sync_hdr = sync_header(ctx, shared_ours);
	sync_hdr->msgid = le32(VCHIQ_MSGID_PADDING);
	sync_hdr->size = le32(0);

	shared_ours->debug[0] = le32(VCHIQ_DEBUG_ENTRIES);

	emu68_barrier();
	shared_ours->initialised = le32(1);
	emu68_barrier();
}

/* Hand slot zero at VPU bus address @bus to the firmware (a property-channel tag); its answer kept in the context. */
static s32 send_init_tag(struct vchiq_ctx *ctx, APTR MailboxBase, u32 bus)
{
	ULONG command[7];
	command[0] = sizeof(command);        /* buffer size */
	command[1] = 0;                      /* a request */
	command[2] = VCHIQ_FIRMWARE_TAG_INIT;
	command[3] = sizeof(ULONG);          /* value buffer size */
	command[4] = sizeof(ULONG);          /* request length */
	command[5] = bus;                    /* in: slot zero; out: 0 if taken */
	command[6] = 0;                      /* end tag */
	MB_RawCommand(command);
	ctx->last_mailbox_value = command[5];
	if (command[0] == 0xffffffffu || command[1] != 0x80000000u)
		return VQERR_NOCONNECTION; /* the mailbox call failed */
	if (command[5] != 0)
		return VQERR_NOCONNECTION; /* the firmware did not take the base (0xFFFFFFFF: a second tag this VPU boot) */
	return 0;
}

/* Wait for the VPU to initialise its half; not vq_poll_until(): the half is not valid until it says so. */
static s32 wait_vpu_half(struct vchiq_ctx *ctx, u32 timeout_us)
{
	u32 deadline = get_time() + timeout_us;
	while (!le32(ctx->shared_vpu->initialised))
	{
		if (time_deadline_passed(get_time(), deadline))
			return VQERR_TIMEOUT;
		wait_step(ctx);
	}
	return 0;
}

/*
 * A FRESH window: a new host state, slot zero at VCHIQ_SLOT_ZERO_OFFSET handed
 * to the firmware (one tag per VPU boot), then wait for the VPU's half.  BUSY
 * and DISCONNECTED after.
 */
static s32 init_fresh(struct vchiq_ctx *ctx, APTR MailboxBase, u32 timeout_us)
{
	volatile struct vchiq_host_state *host = ctx->host;
	u32 bus = ctx->window_bus + VCHIQ_SLOT_ZERO_OFFSET;

	memset((void *)host, 0, sizeof(*host));
	host->magic = VCHIQ_HS_MAGIC;
	host->layout = VCHIQ_HS_LAYOUT;
	host->flags = VCHIQ_HS_BUSY;
	host->slot_zero_offset = VCHIQ_SLOT_ZERO_OFFSET;
	host->slot_zero_bus = bus;
	host->rx_slot = VCHIQ_SLOT_NONE;
	host->tx_slot = VCHIQ_SLOT_NONE;
	host->sessions = 1;
	ctx->conn_state = VCHIQ_CONN_DISCONNECTED;
	vq_reset_services(ctx);
	init_slot_zero(ctx, bus);

	s32 rc = send_init_tag(ctx, MailboxBase, bus);
	if (rc == 0)
		rc = wait_vpu_half(ctx, timeout_us);
	if (rc == 0)
		host->vpu_recycle_base = le32(ctx->shared_vpu->slot_queue_recycle);
	return rc;
}

/* ---- the next session: resume, or repair --------------------------------- */

/* A LIVE_CLEAN window: the next session on the connection, which stays as it was. */
static s32 resume(struct vchiq_ctx *ctx)
{
	volatile struct vchiq_host_state *host = ctx->host;
	if (le32(ctx->shared_ours->tx_pos) != host->tx_pos)
		return VQERR_NOCONNECTION; /* published and recorded disagree: not the clean session the flags claim */

	host->flags = (host->flags | VCHIQ_HS_BUSY) & ~VCHIQ_HS_QUIESCED;
	host->sessions++;
	ctx->conn_state = (host->flags & VCHIQ_HS_CONNECTED) ? VCHIQ_CONN_CONNECTED : VCHIQ_CONN_DISCONNECTED;
	vq_reset_services(ctx);
	return 0;
}

/* A VPU slot with claims not all released, other than @current, the one the walk is in. */
static BOOL is_slot_held(const volatile struct vchiq_host_state *host, u32 slot, u32 current)
{
	return slot != current && host->slot_info[slot].release_count != host->slot_info[slot].use_count;
}

/* How many VPU slots are held, but for @current. */
static u32 count_held_slots(const volatile struct vchiq_host_state *host, u32 current)
{
	u32 held = 0;
	for (u32 slot = VPU_SLOT_FIRST; slot <= VPU_SLOT_LAST; slot++)
		if (is_slot_held(host, slot, current))
			held++;
	return held;
}

/*
 * Return the VPU slots the dead session walked to the end but never gave
 * back: the ones whose claims are not all released (clients release in any
 * order), but for the one the walk is in, which keeps its own claim.  They
 * must number the slots walked minus the slots the VPU got back; FALSE,
 * nothing changed, if not.
 */
static BOOL return_held_slots(struct vchiq_ctx *ctx)
{
	volatile struct vchiq_host_state *host = ctx->host;
	u32 current = host->rx_slot;
	if (current != VCHIQ_SLOT_NONE && (current < VPU_SLOT_FIRST || current > VPU_SLOT_LAST))
		return FALSE;
	u32 walked = VCHIQ_SLOT_QUEUE_INDEX(host->rx_pos);
	u32 returned = le32(ctx->shared_vpu->slot_queue_recycle) - host->vpu_recycle_base;
	if (count_held_slots(host, current) != walked - returned)
		return FALSE;

	for (u32 slot = VPU_SLOT_FIRST; slot <= VPU_SLOT_LAST; slot++)
		if (is_slot_held(host, slot, current))
			vq_recycle_slot(ctx, slot);
	if (walked != returned)
		vq_remote_event_signal(ctx, &ctx->shared_vpu->recycle, FALSE);

	memset((void *)host->slot_info, 0, sizeof(host->slot_info)); /* the dead session's claims forgotten */
	if (current != VCHIQ_SLOT_NONE)
		host->slot_info[current].use_count = 1; /* but the walk's own */
	return TRUE;
}

/* Our stream continues from what the VPU saw: whatever was reserved but never published is dropped. */
static void resync_tx(struct vchiq_ctx *ctx)
{
	u32 tx_pos = le32(ctx->shared_ours->tx_pos);
	ctx->host->tx_pos = tx_pos;
	ctx->host->tx_slot = VCHIQ_SLOT_OFFSET(tx_pos) != 0 ? stream_slot(ctx->shared_ours, tx_pos) : VCHIQ_SLOT_NONE;
	vq_process_free_queue(ctx);
}

/* Close every port the record says the dead session left open or opening: 0, or the last close's error. */
static s32 close_recorded_ports(struct vchiq_ctx *ctx, u32 timeout_us)
{
	s32 rc = 0;
	for (u32 i = 0; i < VCHIQ_MAX_LOCAL_SERVICES; i++)
	{
		if (ctx->host->port_fourcc[i] == 0)
			continue;
		struct vchiq_service *svc = &ctx->services[i];
		svc->fourcc = ctx->host->port_fourcc[i];
		svc->remoteport = ctx->host->port_remote[i];
		svc->state = svc->remoteport == VCHIQ_PORT_FREE ? VCHIQ_SVC_OPENING : VCHIQ_SVC_OPEN; /* no OPENACK yet */
		s32 r = vchiq_close_service(ctx, svc, timeout_us);
		if (r != 0)
			rc = r;
	}
	return rc;
}

/* vq_poll_until() predicate: everything the VPU published has been walked. */
static BOOL is_drained(const struct vchiq_ctx *ctx, const void *arg)
{
	(void)arg;
	return ctx->host->rx_pos == le32(ctx->shared_vpu->tx_pos);
}

/*
 * A session that died mid-flight left three kinds of debris, all visible in
 * the window: VPU slots it walked but never returned, a transmit position
 * ahead of what it published, and ports it never closed (open, or with the
 * OPEN unanswered).  Each is undone from the shared state the VPU still
 * honours, quiet (no callbacks: the dead session's clients are gone), then
 * the stream is drained of what the VPU had queued for them.
 */
static s32 repair(struct vchiq_ctx *ctx, u32 timeout_us)
{
	volatile struct vchiq_host_state *host = ctx->host;
	if (!is_slot_zero_live(ctx) || (host->flags & VCHIQ_HS_CONNECTED) == 0)
		return VQERR_NOCONNECTION; /* no slot zero to repair, or it died before CONNECT: nothing the VPU would answer */
	if (!return_held_slots(ctx))
		return VQERR_NOCONNECTION; /* the record and the shared state disagree */

	host->flags = (host->flags | VCHIQ_HS_BUSY) & ~(VCHIQ_HS_QUIESCED | VCHIQ_HS_DIRTY);
	host->sessions++;
	ctx->conn_state = VCHIQ_CONN_CONNECTED;
	vq_reset_services(ctx);
	resync_tx(ctx);

	ctx->quiet = 1;
	s32 rc = close_recorded_ports(ctx, timeout_us);
	s32 drained = vq_poll_until(ctx, is_drained, NULL, timeout_us);
	ctx->quiet = 0;
	if (rc == 0)
		rc = drained;
	if (rc == 0 && (host->flags & VCHIQ_HS_DIRTY))
		rc = VQERR_NOCONNECTION; /* the walk found the stream corrupt */
	return rc;
}

/* ---- CONNECT, and the session's two calls -------------------------------- */

/* vq_poll_until() predicate: the VPU answered our CONNECT. */
static BOOL is_connected(const struct vchiq_ctx *ctx, const void *arg)
{
	(void)arg;
	return ctx->conn_state == VCHIQ_CONN_CONNECTED;
}

/* The CONNECT exchange: ours once, then the VPU's.  A no-op once connected (a resumed session). */
static s32 exchange_connect(struct vchiq_ctx *ctx, u32 timeout_us)
{
	if (ctx->conn_state == VCHIQ_CONN_CONNECTED)
		return 0;
	if (ctx->conn_state == VCHIQ_CONN_DISCONNECTED)
	{
		s32 rc = vq_queue_message(ctx, VCHIQ_MAKE_MSG(VCHIQ_MSG_CONNECT, 0, 0), NULL, 0, timeout_us, TRUE);
		if (rc != 0)
			return rc;
		ctx->conn_state = VCHIQ_CONN_CONNECTING;
	}
	return vq_poll_until(ctx, is_connected, NULL, timeout_us);
}

/* The session's clean end: nothing open, nothing held.  Clears BUSY, sets QUIESCED. */
static s32 quiesce(struct vchiq_ctx *ctx)
{
	for (u32 i = 0; i < VCHIQ_MAX_LOCAL_SERVICES; i++)
		if (ctx->services[i].state != VCHIQ_SVC_FREE)
			return IOERR_UNITBUSY;

	vchiq_poll(ctx); /* release what the VPU sent last; its slots go back */
	if (ctx->host->flags & VCHIQ_HS_DIRTY)
		return VQERR_NOCONNECTION;

	emu68_barrier();
	ctx->host->flags = (ctx->host->flags & ~VCHIQ_HS_BUSY) | VCHIQ_HS_QUIESCED;
	emu68_barrier();
	return 0;
}

/* The liveness probe: a service nobody serves, and a short wait (a live VPU refuses within a millisecond). */
#define VCHIQ_PROBE_FOURCC VCHIQ_MAKE_FOURCC('v', 'P', 'r', 'B')
#define VCHIQ_PROBE_US 500000u

/**
 * probe_alive() - Make the VPU answer, to tell a live session from a stale record.
 * @ctx: connected, as resume() or repair() and exchange_connect() left it
 * @timeout_us: how long to wait for the answer
 *
 * Opens a service no server provides: a live VPU refuses it at once; a VPU
 * that restarted since the record was written (a watchdog reboot that kept
 * DRAM) does not read this ring and stays silent.  Any other error (no free
 * service, not connected) counts as alive: nothing is re-initialised on a
 * guess.
 *
 * Return: 0 if the VPU answered, VQERR_TIMEOUT if it did not.
 */
static s32 probe_alive(struct vchiq_ctx *ctx, u32 timeout_us)
{
	struct vchiq_service *svc = NULL;
	s32 rc = vchiq_open_service(ctx, VCHIQ_PROBE_FOURCC, 0, 0, &svc, timeout_us);
	if (rc == 0)
		(void)vchiq_close_service(ctx, svc, timeout_us); /* accepted after all: alive, and not wanted */
	return rc == VQERR_TIMEOUT ? VQERR_TIMEOUT : 0;
}

/**
 * forget() - Erase the host state so the window reads as FRESH.
 * @ctx: attached
 *
 * Only correct after the VPU itself has restarted: DRAM may keep the old
 * state across that, and nothing clears the window at Emu68's boot.
 * vchiq_connect() calls it when probe_alive() finds the VPU silent on a
 * resumed record.
 */
static void forget(struct vchiq_ctx *ctx)
{
	memset((void *)ctx->host, 0, sizeof(struct vchiq_host_state));
	emu68_barrier();
	ctx->conn_state = VCHIQ_CONN_DISCONNECTED;
}

/**
 * probe_broken() - Whether the VPU still holds a session the repair could not use.
 * @ctx:        attached; the record LIVE_DIRTY, repair() or resume() failed
 * @timeout_us: how long to wait for CONNECT
 *
 * A record the repair cannot read is one of two things: a live VPU's session
 * whose accounting is lost (a power cycle recovers), or a session the VPU
 * forgot when it restarted while DRAM kept the window (a quick power cycle,
 * a firmware watchdog).  Only the VPU can tell them apart.  With no VPU half
 * of slot zero it never used this window: nothing to destroy.  Otherwise the
 * stream is taken up at its published position, quiet, and the VPU is asked:
 * CONNECT when the dead session never got that far, then the probe service,
 * which a live VPU refuses at once.
 *
 * Return: VQERR_TIMEOUT when the VPU is silent (re-initialise), else
 * VQERR_NOCONNECTION, the session left disconnected.
 */
static s32 probe_broken(struct vchiq_ctx *ctx, u32 timeout_us)
{
	if (!is_slot_zero_live(ctx))
		return VQERR_TIMEOUT;
	ctx->conn_state = (ctx->host->flags & VCHIQ_HS_CONNECTED) ? VCHIQ_CONN_CONNECTED : VCHIQ_CONN_DISCONNECTED;
	vq_reset_services(ctx);
	resync_tx(ctx);
	ctx->quiet = 1;
	s32 rc = exchange_connect(ctx, timeout_us);
	if (rc == 0)
		rc = probe_alive(ctx, VCHIQ_PROBE_US);
	ctx->quiet = 0;
	if (rc != VQERR_TIMEOUT)
	{
		ctx->conn_state = VCHIQ_CONN_DISCONNECTED;
		return VQERR_NOCONNECTION;
	}
	return VQERR_TIMEOUT;
}

/*
 * Bring the session up as the window says (fresh start, resume or repair), then
 * CONNECT.  A record is trusted only once the VPU has answered on it: nothing
 * clears the window when the VPU alone restarts, so a silent VPU means the
 * record is stale, and the VPU, fresh, takes a new tag in the same window.
 * That holds for a record the repair could not use too (probe_broken()).
 */
s32 vchiq_connect(struct vchiq_ctx *ctx, APTR MailboxBase, u32 timeout_us)
{
	if (ctx->conn_state == VCHIQ_CONN_CONNECTED)
		return 0; /* this session is up already */

	ctx->fragments_free[0] = 0xffffffffu; /* a session starts with no bulk in flight */
	ctx->fragments_free[1] = 0xffffffffu;
	s32 rc;
	s32 state = vchiq_classify(ctx);
	switch (state)
	{
	case VCHIQ_STATE_FRESH:
		rc = init_fresh(ctx, MailboxBase, timeout_us); /* the tag and the VPU's half of slot zero: a round trip in itself */
		break;
	case VCHIQ_STATE_LIVE_CLEAN:
		rc = resume(ctx);
		break;
	default:
		rc = repair(ctx, timeout_us); /* connected after */
		break;
	}
	if (rc == 0)
		rc = exchange_connect(ctx, timeout_us);
	if (state != VCHIQ_STATE_FRESH)
	{
		if (rc == 0)
			rc = probe_alive(ctx, VCHIQ_PROBE_US); /* a resumed CONNECTED session has not heard from the VPU yet */
		else if (rc != VQERR_TIMEOUT)
			rc = probe_broken(ctx, timeout_us); /* the record is no use: is the VPU still on it? */
		if (rc == VQERR_TIMEOUT)
		{
			forget(ctx);
			ctx->fragments_free[0] = 0xffffffffu;
			ctx->fragments_free[1] = 0xffffffffu;
			rc = init_fresh(ctx, MailboxBase, timeout_us);
			if (rc == 0)
				rc = exchange_connect(ctx, timeout_us);
		}
	}
	if (rc != 0)
		vchiq_mark_dirty(ctx);
	return rc;
}

/* End the session cleanly: every service closed, the window QUIESCED, so the next session resumes. */
s32 vchiq_shutdown(struct vchiq_ctx *ctx, u32 timeout_us)
{
	ctx->quiet = 1;
	vchiq_disarm_events(ctx);
	for (u32 i = 0; i < VCHIQ_MAX_LOCAL_SERVICES; i++)
		if (ctx->services[i].state != VCHIQ_SVC_FREE)
			(void)vchiq_close_service(ctx, &ctx->services[i], timeout_us); /* quiesce() tells whether all went */
	s32 rc = quiesce(ctx);
	if (rc != 0)
		vchiq_mark_dirty(ctx);
	ctx->quiet = 0;
	ctx->conn_state = VCHIQ_CONN_DISCONNECTED; /* the next vchiq_connect() resumes, or repairs */
	return rc;
}

/* Give up on the session: the next one repairs. */
void vchiq_mark_dirty(struct vchiq_ctx *ctx)
{
	ctx->host->flags |= VCHIQ_HS_DIRTY;
	emu68_barrier();
}
