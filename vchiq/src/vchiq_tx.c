/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * vchiq_tx.c - the transmit side: our stream, our sync slot, and the remote
 * events that announce them.
 */
#include <vchiq/vchiq_core_priv.h>

/*
 * Sender side of a remote event: raise fired, then ring the doorbell if the
 * peer armed it.  On the trigger (@force) it rings regardless, so a transmit
 * can never be missed through an arm/fired race on the VPU side, at the cost
 * of one extra doorbell write.  Returns the peer's
 * armed flag for the trace, not whether we rang.
 */
BOOL vq_remote_event_signal(struct vchiq_ctx *ctx, volatile struct vchiq_remote_event *ev, BOOL force)
{
	emu68_barrier(); /* the shared writes this event announces come first */
	ev->fired = le32(1);
	emu68_barrier();
	BOOL armed = le32(ev->armed) != 0;
	if (armed || force)
	{
		mmio_write32(0, ctx->bell + VCHIQ_BELL2);
		ctx->stats.bell_rings++;
	}
	return armed;
}

/* Publish our stream up to @tx_pos and ring the VPU.  Returns its armed flag, for the trace. */
static BOOL publish_tx(struct vchiq_ctx *ctx, u32 tx_pos)
{
	ctx->host->tx_pos = tx_pos;
	emu68_barrier(); /* the messages before their announcement */
	ctx->shared_ours->tx_pos = le32(tx_pos);
	return vq_remote_event_signal(ctx, &ctx->shared_vpu->trigger, TRUE);
}

/*
 * Room for @stride bytes (VCHIQ_MSG_STRIDE(): header and payload) at
 * host->tx_pos: pads out a slot that cannot hold the message and takes the
 * next slot from our queue.  With the queue exhausted it waits up to
 * @timeout_us (0: not at all) for the VPU to recycle one.  With @poll each
 * step of that wait walks the VPU's stream (vchiq_poll(): callbacks, state
 * changes of any service); without, it only takes back the slots the VPU has
 * recycled, which is what a caller inside vchiq_poll() (a CLOSE answered from
 * the walk) must do.  NULL if no slot comes in time, or the queue is corrupt.
 */
static volatile struct vchiq_header *reserve_space(struct vchiq_ctx *ctx, u32 stride, u32 timeout_us, BOOL poll)
{
	volatile struct vchiq_host_state *host = ctx->host;
	u32 tx_pos = host->tx_pos;
	u32 slot_space = VCHIQ_SLOT_SIZE - VCHIQ_SLOT_OFFSET(tx_pos);

	if (stride > slot_space)
	{
		/* messages never cross a slot's end: the rest of this one is PADDING */
		volatile struct vchiq_header *pad = header_at(ctx, host->tx_slot, tx_pos);
		pad->msgid = le32(VCHIQ_MSGID_PADDING);
		pad->size = le32(slot_space - 8u);
		tx_pos += slot_space;
	}

	if (VCHIQ_SLOT_OFFSET(tx_pos) == 0)
	{
		/* at a slot boundary: the next slot is our queue's entry for this position */
		u32 qidx = VCHIQ_SLOT_QUEUE_INDEX(tx_pos);

		vq_process_free_queue(ctx);
		if (qidx >= host->slot_queue_available)
		{
			/* all our slots are with the VPU: publish the padding, so it can finish one and recycle it */
			publish_tx(ctx, tx_pos);

			u32 deadline = get_time() + timeout_us;
			while (qidx >= host->slot_queue_available)
			{
				if (poll)
					vchiq_poll(ctx);
				else
					vq_process_free_queue(ctx);
				if (qidx < host->slot_queue_available)
					break;
				if (timeout_us == 0 || time_deadline_passed(get_time(), deadline))
					return NULL;
				wait_step(ctx);
			}
		}
		host->tx_slot = queue_entry(ctx->shared_ours, qidx);
		if (host->tx_slot >= VCHIQ_MAX_SLOTS)
		{
			vchiq_mark_dirty(ctx); /* a queue entry no slot has: the shared state is corrupt */
			return NULL;
		}
	}

	host->tx_pos = tx_pos + stride; /* reserved here; the VPU sees it once publish_tx() moves shared_ours->tx_pos */
	return header_at(ctx, host->tx_slot, tx_pos);
}

/* One message of any type into our stream, published and announced. */
s32 vq_queue_message(struct vchiq_ctx *ctx, u32 msgid, const void *data, u32 size, u32 timeout_us, BOOL poll)
{
	if (size > VCHIQ_MAX_MSG_SIZE)
		return IOERR_BADLENGTH;

	volatile struct vchiq_header *hdr = reserve_space(ctx, VCHIQ_MSG_STRIDE(size), timeout_us, poll);
	if (hdr == NULL)
		return (ctx->host->flags & VCHIQ_HS_DIRTY) ? VQERR_NOCONNECTION : IOERR_UNITBUSY;

	/* the message, invisible to the VPU until published */
	if (size != 0)
		memcpy((void *)hdr->data, data, size);
	hdr->size = le32(size);
	hdr->msgid = le32(msgid);

	ctx->stats.msgs_tx++;
	BOOL was_armed __attribute__((unused)) = publish_tx(ctx, ctx->host->tx_pos); /* published, the VPU rung */
	KprintfT("[vchiq] tx type %ld %ld->%ld size %ld (tx_pos %ld) vpu-armed %ld\n", (ULONG)VCHIQ_MSG_TYPE(msgid),
	         (ULONG)VCHIQ_MSG_SRCPORT(msgid), (ULONG)VCHIQ_MSG_DSTPORT(msgid), (ULONG)size, (ULONG)ctx->host->tx_pos,
	         (ULONG)was_armed);
	return 0;
}

/* vq_poll_until() predicate: the VPU released our sync slot. */
static BOOL is_sync_slot_free(const struct vchiq_ctx *ctx, const void *arg)
{
	(void)arg;
	return le32(ctx->shared_ours->sync_release.fired) != 0;
}

/*
 * One message through our sync slot: the slot is ours while sync_release has
 * fired, until the VPU releases it again.  A taken slot is waited for when
 * @poll allows a walk, else IOERR_UNITBUSY; either way sync_tx_waiting asks
 * vchiq_arm_events() for the doorbell that frees it.
 */
s32 vq_queue_message_sync(struct vchiq_ctx *ctx, u32 msgid, const void *data, u32 size, u32 timeout_us, BOOL poll)
{
	if (size > VCHIQ_MAX_MSG_SIZE)
		return IOERR_BADLENGTH;
	if (!is_sync_slot_free(ctx, NULL))
	{
		/* the VPU still has our last one: wait for its release only if we may (vq_poll_until() walks the stream) */
		s32 rc = timeout_us != 0 && poll ? vq_poll_until(ctx, is_sync_slot_free, NULL, timeout_us) : IOERR_UNITBUSY;
		if (rc != 0)
		{
			ctx->sync_tx_waiting = 1;
			return rc == VQERR_TIMEOUT ? IOERR_UNITBUSY : rc;
		}
	}
	ctx->sync_tx_waiting = 0;
	ctx->shared_ours->sync_release.fired = le32(0); /* taken */

	/* one slot, one message: written in place, then announced on sync_trigger */
	volatile struct vchiq_header *hdr = sync_header(ctx, ctx->shared_ours);
	if (size != 0)
		memcpy((void *)hdr->data, data, size);
	hdr->size = le32(size);
	hdr->msgid = le32(msgid);
	ctx->stats.msgs_tx++;
	vq_remote_event_signal(ctx, &ctx->shared_vpu->sync_trigger, TRUE);
	return 0;
}
