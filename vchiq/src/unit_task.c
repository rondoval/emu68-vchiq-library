/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * unit_task.c - the device's task: one for all units.
 *
 * The task walks the VPU's stream, serves the openers' requests, and wakes the
 * openers that wait for the VPU to answer an open or a close.  Between rounds
 * it arms the remote events so the VPU rings the doorbell for the next message
 * or recycled slot; BeginIO wakes it with the same signal for a new request.
 * Without a doorbell interrupt (BCM2836/7) a UNIT_POLL_MS tick runs the round.
 *
 * A round, with the lock held and inside the ring:
 *   - walk the VPU's stream (bulk completions are replied from the core's
 *     callback as they are walked);
 *   - hand each unit's held messages to its waiting CMD_READs, copying and
 *     releasing them, and walk again if that let a stalled walk go on;
 *   - issue each unit's writes and bulk transmits in order, and its bulk
 *     receives in order, as far as the VPU has slots and bulk room; the rest
 *     wait for the next round, which a recycled slot or a completed bulk
 *     brings with a doorbell.  The two queues are independent: a transmit
 *     waiting for room (four per direction and service) must not hold up a
 *     receive, which the VPU may need to finish before it takes the next
 *     transmit.
 */
#include <dos/dos.h>

#include <byteorder.h>
#include <driver_task.h>
#include <drv_timer.h>
#include <memory.h>

#include <device.h>

/* ---- the core's callback ------------------------------------------------------ */

void UnitCallback(struct vchiq_ctx *ctx, struct vchiq_service *svc, u32 reason, const struct vchiq_bulk *bulk,
                  s32 bulk_actual)
{
	(void)ctx;
	struct VCHIQUnit *unit = svc->userdata;
	struct VCHIQDevice *base = unit->base;

	switch (reason)
	{
	case VCHIQ_REASON_BULK_TRANSMIT_DONE:
	case VCHIQ_REASON_BULK_RECEIVE_DONE:
	{
		struct IOVchiqReq *io = bulk->userdata;
		io->iovq_Req.io_Actual = (ULONG)bulk_actual;
		UnitReply(base, io, 0);
		break;
	}
	case VCHIQ_REASON_BULK_TRANSMIT_ABORTED:
	case VCHIQ_REASON_BULK_RECEIVE_ABORTED:
		UnitReply(base, bulk->userdata, unit->closed || svc->state != VCHIQ_SVC_OPEN ? VQERR_CLOSED : IOERR_ABORTED);
		break;
	case VCHIQ_REASON_SERVICE_CLOSED:
		Kprintf("[vchiq] the VPU closed service %08lx\n", svc->fourcc);
		unit->closed = 1;
		UnitFailRequests(base, unit, VQERR_CLOSED);
		break;
	default:
		break;
	}
}

/* ---- the round ---------------------------------------------------------------- */

/* Held messages to waiting reads, oldest first; io_Actual is the message's size.  TRUE if any went. */
static BOOL deliver_messages(struct VCHIQDevice *base, struct VCHIQUnit *unit)
{
	struct ExecBase *SysBase = base->sysBase;
	BOOL delivered = FALSE;
	volatile struct vchiq_header *hdr;

	while (unit->readq.mlh_Head->mln_Succ != NULL && (hdr = vchiq_take_message(unit->svc)) != NULL)
	{
		struct IOVchiqReq *io = (struct IOVchiqReq *)RemHead((struct List *)&unit->readq);
		struct IOStdReq *std = &io->iovq_Req;
		std->io_Actual = le32(hdr->size);
		memcpy(std->io_Data, (const void *)hdr->data, std->io_Actual < std->io_Length ? std->io_Actual : std->io_Length);
		vchiq_release_message(&base->ctx, hdr);
		UnitReply(base, io, std->io_Actual > std->io_Length ? VQERR_TRUNCATED : 0);
		delivered = TRUE;
	}
	return delivered;
}

/**
 * submit_requests() - Hand one of a unit's queues to the core, in order.
 * @base:  the device base, lock held, inside the ring
 * @unit:  the unit, its service open
 * @queue: @unit's sendq or bulkreadq
 *
 * Stops at the first request the VPU has no room for: the rest of @queue
 * waits behind it, the other queue does not.
 */
static void submit_requests(struct VCHIQDevice *base, struct VCHIQUnit *unit, struct MinList *queue)
{
	struct ExecBase *SysBase = base->sysBase;
	struct vchiq_ctx *ctx = &base->ctx;

	while (queue->mlh_Head->mln_Succ != NULL)
	{
		struct IOVchiqReq *io = (struct IOVchiqReq *)queue->mlh_Head;
		struct IOStdReq *std = &io->iovq_Req;
		s32 rc;
		if (std->io_Command == CMD_WRITE)
			rc = vchiq_queue_message(ctx, unit->svc, std->io_Data, std->io_Length, 0, FALSE);
		else
			rc = vchiq_queue_bulk(ctx, unit->svc, std->io_Command == NSCMD_VQ_BULKWRITE ? VCHIQ_BULK_TX : VCHIQ_BULK_RX,
			                      std->io_Data, std->io_Length, io);
		if (rc == IOERR_UNITBUSY)
			break; /* no slot, no bulk room or no fragment: the next round */
		Remove((struct Node *)&std->io_Message.mn_Node);
		if (rc != 0)
			UnitReply(base, io, rc);
		else if (std->io_Command == CMD_WRITE)
		{
			std->io_Actual = std->io_Length;
			UnitReply(base, io, 0);
		}
		/* a bulk is the core's now: its completion is replied from the callback */
	}
}

/* Walk the VPU's stream, then deliver and submit for every unit; again while a delivery lets a stalled walk go on. */
static void process_units(struct VCHIQDevice *base)
{
	struct vchiq_ctx *ctx = &base->ctx;
	BOOL delivered;

	do
	{
		vchiq_poll(ctx);
		delivered = FALSE;
		for (struct MinNode *n = base->units.mlh_Head; n->mln_Succ != NULL; n = n->mln_Succ)
		{
			struct VCHIQUnit *unit = UNIT_FROM_NODE(n);
			if (unit->svc == NULL)
				continue; /* an information-only unit */
			delivered |= deliver_messages(base, unit);
			if (!unit->closed)
			{
				submit_requests(base, unit, &unit->bulkreadq);
				submit_requests(base, unit, &unit->sendq);
			}
		}
	} while (delivered && vchiq_has_unwalked(ctx)); /* a read that emptied a full queue frees the walk */
}

/* ---- the task ------------------------------------------------------------------ */

static void UnitTask(struct VCHIQDevice *base, struct Task *parent)
{
	struct ExecBase *SysBase = base->sysBase;
	struct vchiq_ctx *ctx = &base->ctx;

	BYTE irq_signal = AllocSignal(-1);
	if (irq_signal == -1)
	{
		Kprintf("[vchiq] %s: no signal\n", __func__);
		drv_task_exit(SysBase, &base->task, parent, FALSE);
		return;
	}
	ULONG irq_mask = 1UL << irq_signal;

	/* no doorbell interrupt (BCM2836/7): a timer tick runs the round instead */
	struct drv_timer tick;
	BOOL polled = base->irq < 0;
	if (polled && !drv_timer_open(&tick, SysBase))
	{
		Kprintf("[vchiq] %s: no timer to poll with\n", __func__);
		FreeSignal(irq_signal);
		drv_task_exit(SysBase, &base->task, parent, FALSE);
		return;
	}
	ULONG tick_mask = polled ? drv_timer_sigmask(&tick) : 0;
	if (polled)
		drv_timer_arm_ms(&tick, UNIT_POLL_MS);
	/* a core wait inside a round (a CLOSE answered with all our slots out) sleeps on its own timer: the tick stays armed */
	struct drv_timer nap;
	if (drv_timer_open(&nap, SysBase))
		ctx->timer = &nap;

	base->irq_signal = irq_signal; /* read by the interrupt server and UnitTaskKick(), both only after the start */
	base->task = FindTask(NULL);
	Signal(parent, SIGBREAKF_CTRL_F);

	for (;;)
	{
		BOOL fired = FALSE;

		ObtainSemaphore(&base->lock);
		if (vchiq_ring_enter(base))
		{
			vchiq_disarm_events(ctx);
			process_units(base);
			vchiq_arm_events(ctx);
			fired = vchiq_has_fired(ctx); /* raised between the walk and the arming: go round again */
			vchiq_ring_leave(base);
			drv_wake_up_all(&base->waitq, SysBase); /* every waiting client re-checks its condition */
		}
#ifdef PROFILE
		if (get_time() - base->perf_reported_at >= PERF_REPORT_US)
		{
			perf_report(&ctx->perf);
			base->perf_reported_at = get_time();
		}
#endif
		ReleaseSemaphore(&base->lock);

		ULONG sig = fired ? SetSignal(0, 0) : Wait(irq_mask | tick_mask | SIGBREAKF_CTRL_C);
		if (sig & SIGBREAKF_CTRL_C)
			break;
		/* re-arm on completion, not on the signal: the fired path above leaves signals set */
		if (polled && CheckIO((struct IORequest *)tick.req))
		{
			drv_timer_consume(&tick);
			drv_timer_arm_ms(&tick, UNIT_POLL_MS);
		}
	}

	ctx->timer = NULL;
	drv_timer_close(&nap);
	if (polled)
	{
		drv_timer_cancel(&tick);
		drv_timer_close(&tick);
	}

	FreeSignal(irq_signal);
	/* the spawning task may be long gone: report to ourselves, the join polls the slot */
	drv_task_exit(SysBase, &base->task, FindTask(NULL), TRUE);
}

s32 UnitTaskStart(struct VCHIQDevice *base)
{
	struct ExecBase *SysBase = base->sysBase;

	return drv_task_spawn(SysBase, base, (APTR)UnitTask, UNIT_TASK_NAME, STACK_SIZE, UNIT_TASK_PRIORITY) == 0
	           ? 0
	           : VQERR_NOCONNECTION;
}

void UnitTaskStop(struct VCHIQDevice *base)
{
	struct ExecBase *SysBase = base->sysBase;

	drv_task_join(SysBase, &base->task);
}

void UnitTaskKick(struct VCHIQDevice *base)
{
	struct ExecBase *SysBase = base->sysBase;

	Signal(base->task, 1UL << base->irq_signal); /* a unit is open: the task runs (it stops only at expunge) */
}
