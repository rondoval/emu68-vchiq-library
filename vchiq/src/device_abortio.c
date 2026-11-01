/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * device_abortio.c - AbortIO.
 *
 * A request still on a unit's queue comes back IOERR_ABORTED.  One the core
 * holds (a bulk the VPU already has) completes normally: its DMA cannot be
 * stopped.
 */
#include <device.h>

static BOOL list_holds(const struct MinList *list, const struct IOVchiqReq *io)
{
	for (const struct MinNode *n = list->mlh_Head; n->mln_Succ != NULL; n = n->mln_Succ)
		if ((const struct IOVchiqReq *)n == io)
			return TRUE;
	return FALSE;
}

LONG abortIO(struct IOVchiqReq *io asm("a1"), struct VCHIQDevice *base asm("a6"))
{
	struct ExecBase *SysBase = base->sysBase;
	struct VCHIQUnit *unit = (struct VCHIQUnit *)io->iovq_Req.io_Unit;

	if (unit == NULL)
		return 0;
	ObtainSemaphore(&base->lock);
	if (list_holds(&unit->readq, io) || list_holds(&unit->sendq, io) || list_holds(&unit->bulkreadq, io))
	{
		Remove((struct Node *)&io->iovq_Req.io_Message.mn_Node);
		UnitReply(base, io, IOERR_ABORTED);
	}
	ReleaseSemaphore(&base->lock);
	return 0;
}
