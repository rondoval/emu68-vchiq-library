/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * device_beginio.c - BeginIO.
 *
 * BeginIO never touches the ring: NSCMD_DEVICEQUERY, NSCMD_VQ_GETINFO and
 * CMD_FLUSH (and in a VCHIQ_TEST_HOOKS build NSCMD_VQ_TEST_ABANDON) are
 * answered at once, every other request goes on its unit's queue under the
 * lock and the unit task is kicked.
 */
#include <exec/errors.h>
#include <devices/newstyle.h>

#include <device.h>
#include <vchiq/vchiq_test.h>

static const UWORD supported_commands[] = {
    CMD_READ, CMD_WRITE, CMD_FLUSH, NSCMD_DEVICEQUERY, NSCMD_VQ_BULKWRITE, NSCMD_VQ_BULKREAD, NSCMD_VQ_GETINFO,
#ifdef VCHIQ_TEST_HOOKS
    NSCMD_VQ_TEST_ABANDON,
#endif
    0,
};

static void device_query(struct VCHIQDevice *base, struct IOVchiqReq *io)
{
	struct NSDeviceQueryResult *q = io->iovq_Req.io_Data;
	if (q == NULL || io->iovq_Req.io_Length < sizeof(*q))
	{
		UnitReply(base, io, IOERR_BADLENGTH);
		return;
	}
	q->nsdqr_DevQueryFormat = 0;
	q->nsdqr_SizeAvailable = sizeof(*q);
	q->nsdqr_DeviceType = NSDEVTYPE_UNKNOWN;
	q->nsdqr_DeviceSubType = 0;
	q->nsdqr_SupportedCommands = (APTR)supported_commands;
	io->iovq_Req.io_Actual = sizeof(*q);
	UnitReply(base, io, 0);
}

/* Lock held. */
static void fill_info(struct VCHIQDevice *base, struct VchiqInfo *info)
{
	struct vchiq_ctx *ctx = &base->ctx;
	ULONG services = 0;
	for (u32 i = 0; i < VCHIQ_MAX_LOCAL_SERVICES; i++)
		if (ctx->services[i].state == VCHIQ_SVC_OPEN)
			services++;

	info->vi_WindowBase = base->window_base;
	info->vi_WindowSize = base->window_size;
	info->vi_Sessions = ctx->host->sessions;
	info->vi_VersionCommon = ctx->host->version_common;
	info->vi_Flags = ctx->host->flags;
	info->vi_RxPos = ctx->host->rx_pos;
	info->vi_TxPos = ctx->host->tx_pos;
	info->vi_MsgsTx = ctx->stats.msgs_tx;
	info->vi_MsgsRx = ctx->stats.msgs_rx;
	info->vi_SlotsIn = ctx->stats.slots_recycled_in;
	info->vi_SlotsOut = ctx->stats.slots_recycled_out;
	info->vi_BellRings = ctx->stats.bell_rings;
	info->vi_Interrupts = base->interrupts;
	info->vi_BulksDone = ctx->stats.bulks_done;
	info->vi_Unexpected = ctx->stats.unexpected_msgs;
	info->vi_Dropped = ctx->stats.dropped_msgs;
	info->vi_RxStalls = ctx->stats.rx_stalls;
	info->vi_Services = services;
	info->vi_Bringup = base->bringup;
	info->vi_ReachStart = base->platform.data_cpu_start;
	info->vi_ReachEnd = base->platform.data_cpu_end;
	info->vi_ReachBus = base->platform.data_cpu_start + (base->window_bus - base->window_base);
	info->vi_UncachedStart = base->vram_start;
	info->vi_UncachedEnd = base->vram_end;
}

static void get_info(struct VCHIQDevice *base, struct IOVchiqReq *io)
{
	struct ExecBase *SysBase = base->sysBase;
	struct VchiqInfo info;
	ObtainSemaphore(&base->lock);
	fill_info(base, &info);
	ReleaseSemaphore(&base->lock);

	ULONG n = io->iovq_Req.io_Length < sizeof(info) ? io->iovq_Req.io_Length : sizeof(info);
	if (io->iovq_Req.io_Data != NULL)
		CopyMem(&info, io->iovq_Req.io_Data, n);
	io->iovq_Req.io_Actual = n;
	UnitReply(base, io, 0);
}

void beginIO(struct IOVchiqReq *io asm("a1"), struct VCHIQDevice *base asm("a6"))
{
	struct ExecBase *SysBase = base->sysBase;
	struct IOStdReq *std = &io->iovq_Req;
	struct VCHIQUnit *unit = (struct VCHIQUnit *)std->io_Unit;

	std->io_Message.mn_Node.ln_Type = NT_MESSAGE;
	std->io_Error = 0;
	std->io_Actual = 0;

	switch (std->io_Command)
	{
	case NSCMD_DEVICEQUERY:
		device_query(base, io);
		return;
	case NSCMD_VQ_GETINFO:
		get_info(base, io);
		return;
#ifdef VCHIQ_TEST_HOOKS
	case NSCMD_VQ_TEST_ABANDON:
		base->test_abandon = 1; /* read by vchiq_link_reset() */
		UnitReply(base, io, 0);
		return;
#endif
	case CMD_FLUSH:
		ObtainSemaphore(&base->lock);
		UnitFailRequests(base, unit, IOERR_ABORTED);
		ReleaseSemaphore(&base->lock);
		UnitReply(base, io, 0);
		return;
	case CMD_READ:
	case CMD_WRITE:
		if (std->io_Length != 0 && std->io_Data == NULL)
		{
			UnitReply(base, io, IOERR_BADADDRESS);
			return;
		}
		if (std->io_Command == CMD_WRITE && std->io_Length > VCHIQ_MAX_MESSAGE)
		{
			UnitReply(base, io, IOERR_BADLENGTH);
			return;
		}
		break;
	case NSCMD_VQ_BULKWRITE:
	case NSCMD_VQ_BULKREAD:
		if (std->io_Length == 0 || std->io_Data == NULL)
		{
			UnitReply(base, io, std->io_Length == 0 ? IOERR_BADLENGTH : IOERR_BADADDRESS);
			return;
		}
		break;
	default:
		UnitReply(base, io, IOERR_NOCMD);
		return;
	}

	if (unit->svc == NULL)
	{
		UnitReply(base, io, IOERR_NOCMD); /* an information-only unit */
		return;
	}
	ObtainSemaphore(&base->lock);
	if (!base->up || unit->closed)
	{
		ReleaseSemaphore(&base->lock);
		UnitReply(base, io, base->up ? VQERR_CLOSED : VQERR_NOCONNECTION);
		return;
	}
	std->io_Flags &= (UBYTE)~IOF_QUICK;
	struct MinList *queue = std->io_Command == CMD_READ             ? &unit->readq
	                        : std->io_Command == NSCMD_VQ_BULKREAD ? &unit->bulkreadq
	                                                                : &unit->sendq;
	AddTail((struct List *)queue, &std->io_Message.mn_Node);
	ReleaseSemaphore(&base->lock);
	UnitTaskKick(base);
}
