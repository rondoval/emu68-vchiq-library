/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * unit.c - the openers: one unit, one VCHIQ service.
 *
 * The service is opened and closed in the opener's context, which waits for
 * the VPU's answer with the lock released (the unit task takes the lock to
 * walk the stream that carries the answer).  Requests come back from here,
 * from the unit task and from AbortIO.
 */
#include <exec/memory.h>

#include <minlist.h>

#include <device.h>

/*
 * A request comes back: replied, unless BeginIO answers it with IOF_QUICK
 * still set (a queued request has it cleared).  Lock held or not: ReplyMsg()
 * is safe anywhere a task may run.
 */
void UnitReply(struct VCHIQDevice *base, struct IOVchiqReq *io, s32 error)
{
	struct ExecBase *SysBase = base->sysBase;
	io->iovq_Req.io_Error = (BYTE)error;
	if (!(io->iovq_Req.io_Flags & IOF_QUICK))
		ReplyMsg(&io->iovq_Req.io_Message);
}

static void fail_list(struct VCHIQDevice *base, struct MinList *list, BYTE error)
{
	struct ExecBase *SysBase = base->sysBase;
	struct MinNode *n;
	while ((n = (struct MinNode *)RemHead((struct List *)list)) != NULL)
		UnitReply(base, (struct IOVchiqReq *)n, error);
}

void UnitFailRequests(struct VCHIQDevice *base, struct VCHIQUnit *unit, BYTE error)
{
	fail_list(base, &unit->readq, error);
	fail_list(base, &unit->sendq, error);
	fail_list(base, &unit->bulkreadq, error);
}

/* ---- the service ----------------------------------------------------------------- */

/* @pred(base, @arg), the VPU's doing, about service @fourcc.  Lock held on entry and on exit; released while waiting. */
static s32 wait_for(struct VCHIQDevice *base, drv_wait_pred_t pred, APTR arg, u32 fourcc, const char *what)
{
	struct ExecBase *SysBase = base->sysBase;
	(void)fourcc; /* only the debug log reads these */
	(void)what;

	LONG rc = drv_wait_event(&base->waitq, SysBase, &base->lock, pred, base, arg, VCHIQ_WAIT_MS);
	if (rc == DRV_WAIT_TIMEOUT)
	{
		Kprintf("[vchiq] %s %08lx: no answer in %ld ms (rx %ld tx %ld, interrupts %ld, bells %ld)\n", what, fourcc,
		        (ULONG)VCHIQ_WAIT_MS, base->ctx.host->rx_pos, base->ctx.host->tx_pos, base->interrupts,
		        base->ctx.stats.bell_rings);
		return VQERR_TIMEOUT;
	}
	return rc == DRV_WAIT_OK ? 0 : IOERR_OPENFAIL; /* no signal bit or no timer to wait with */
}

static BOOL cond_not_opening(APTR base, APTR arg)
{
	(void)base;
	return ((struct vchiq_service *)arg)->state != VCHIQ_SVC_OPENING;
}

static BOOL cond_not_closing(APTR base, APTR arg)
{
	(void)base;
	return ((struct vchiq_service *)arg)->state != VCHIQ_SVC_CLOSING;
}

/* The VPU handed slots of our stream back since @arg, a snapshot of slot_queue_available. */
static BOOL cond_slots_back(APTR base, APTR arg)
{
	return ((struct VCHIQDevice *)base)->ctx.host->slot_queue_available != *(const u32 *)arg;
}

/*
 * Queue @svc's CLOSE.  A full stream is no reason to give up: the service
 * would stay open with a unit that is gone, so wait for the VPU to hand
 * slots back (each round wakes us) and try again, until it stops doing so.
 */
static s32 queue_close(struct VCHIQDevice *base, struct vchiq_service *svc)
{
	for (;;)
	{
		u32 seen = base->ctx.host->slot_queue_available;
		s32 rc = VQERR_NOCONNECTION; /* the reset path has the ring */
		if (vchiq_ring_enter(base))
		{
			rc = vchiq_queue_service_close(&base->ctx, svc);
			vchiq_ring_leave(base);
		}
		if (rc != IOERR_UNITBUSY)
			return rc;
		rc = wait_for(base, cond_slots_back, &seen, svc->fourcc, "close (no room)");
		if (rc != 0)
			return rc;
	}
}

/* Close and free; a service the VPU closed already only goes back to the table, an unanswered open is abandoned. */
static s32 service_close(struct VCHIQDevice *base, struct vchiq_service *svc)
{
	s32 rc = queue_close(base, svc);
	if (rc == 0)
		rc = wait_for(base, cond_not_closing, svc, svc->fourcc, "close");
	if (rc == 0)
		vchiq_free_service(svc);
	return rc;
}

/* The OPEN goes out at once (it is the first message of the service, the task issues none for it). */
static s32 service_open(struct VCHIQDevice *base, struct VCHIQUnit *unit, const struct IOVchiqReq *io)
{
	struct vchiq_service *svc = NULL;
	s32 rc = VQERR_NOCONNECTION; /* the reset path has the ring */
	if (vchiq_ring_enter(base))
	{
		rc = vchiq_queue_service_open(&base->ctx, io->iovq_FourCC, (s16)io->iovq_Version, (s16)io->iovq_VersionMin,
		                              UnitCallback, unit, &svc);
		vchiq_ring_leave(base);
	}
	if (rc != 0)
		return rc; /* nothing sent */

	rc = wait_for(base, cond_not_opening, svc, svc->fourcc, "open");
	if (rc == 0 && svc->state == VCHIQ_SVC_OPEN)
	{
		unit->svc = svc;
		return 0;
	}
	/* refused: CLOSED, back to the table; unanswered: abandoned, its port taken until the VPU answers */
	BOOL refused = svc->state == VCHIQ_SVC_CLOSED;
	if (refused)
		Kprintf("[vchiq] open %08lx: refused by the VPU\n", svc->fourcc);
	(void)service_close(base, svc);
	return refused ? VQERR_REJECTED : rc;
}

/* ---- open and close --------------------------------------------------------------- */

/* A new unit on base->units, its service open (none for iovq_FourCC 0); NULL and *rc if not. */
struct VCHIQUnit *UnitOpen(struct VCHIQDevice *base, const struct IOVchiqReq *io, s32 *rc)
{
	struct ExecBase *SysBase = base->sysBase;

	struct VCHIQUnit *unit = AllocMem(sizeof(*unit), MEMF_PUBLIC | MEMF_CLEAR);
	if (unit == NULL)
	{
		*rc = IOERR_OPENFAIL;
		return NULL;
	}
	unit->base = base;
	_NewMinList(&unit->readq);
	_NewMinList(&unit->sendq);
	_NewMinList(&unit->bulkreadq);
	*rc = io->iovq_FourCC != 0 ? service_open(base, unit, io) : 0; /* 0: an information-only unit */
	if (*rc != 0)
	{
		FreeMem(unit, sizeof(*unit));
		return NULL;
	}
	AddTail((struct List *)&base->units, (struct Node *)&unit->node);
	return unit;
}

/* Every request of the opener is back by now (Exec's rule); the service closes, waiting for the VPU. */
void UnitClose(struct VCHIQDevice *base, struct VCHIQUnit *unit)
{
	struct ExecBase *SysBase = base->sysBase;

	Remove((struct Node *)&unit->node);
	UnitFailRequests(base, unit, IOERR_ABORTED); /* a client that broke the rule still gets its requests */
	struct vchiq_service *svc = unit->svc;
	if (svc != NULL && svc->state == VCHIQ_SVC_CLOSED)
		vchiq_free_service(svc); /* the VPU closed it: no ring access needed */
	else if (svc != NULL && base->up)
		(void)service_close(base, svc); /* a timeout is logged; the entry stays CLOSING until the answer */
	FreeMem(unit, sizeof(*unit));
	perf_report(&base->ctx.perf); /* a client's run ends with its own lines (profile tier) */
}
