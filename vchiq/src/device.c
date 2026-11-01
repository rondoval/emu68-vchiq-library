/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * device.c - resident, device init/open/close/expunge.
 *
 * Init only checks the cache-ops flavour and installs the reset guard, as the
 * stack's other devices do.  The hardware is looked up (devtree_parse.c) and
 * the connection to the VPU brought up at the first OpenDevice(), kept across
 * closes and taken down at expunge (vchiq_link.c).  Each open is a unit with one VCHIQ service
 * (unit.c), opened and closed in the caller, waiting for the VPU's answer.
 */
#include <exec/memory.h>
#include <exec/resident.h>

#include <emu68_features.h>
#include <minlist.h>

#include <device.h>

LONG __attribute__((used, section(".text.entry"))) doNotExecute(void);
LONG __attribute__((used, section(".text.entry"))) doNotExecute(void)
{
	return -1;
}

extern const UBYTE endOfCode;
static const char deviceName[] = DEVICE_NAME;
static const char deviceIdString[] = DEVICE_IDSTRING;
static const APTR initTable[4];

static const struct Resident vchiqDeviceResident __attribute__((used, section(".text.modhdr"))) = {
    RTC_MATCHWORD,
    (struct Resident *)&vchiqDeviceResident,
    (APTR)&endOfCode,
    RTF_AUTOINIT | RTF_COLDSTART,
    DEVICE_VERSION,
    NT_DEVICE,
    DEVICE_PRIORITY,
    (APTR)&deviceName,
    (APTR)&deviceIdString,
    (APTR)initTable,
};

/* ---- device vectors ------------------------------------------------------------ */

static ULONG expungeLib(struct VCHIQDevice *base asm("a6"))
{
	struct ExecBase *SysBase = base->sysBase;
	ULONG segList = base->segList;

	if (base->device.dd_Library.lib_OpenCnt > 0)
	{
		base->device.dd_Library.lib_Flags |= LIBF_DELEXP;
		return 0;
	}

	/* The ColdReboot vector may have been re-patched on top of our stub: then the code must stay resident. */
	if (!reset_guard_remove(&base->resetGuard))
		return 0;
	vchiq_link_down(base);
	drv_waitq_cleanup(&base->waitq, SysBase);

	Forbid();
	Remove((struct Node *)base);
	Permit();

	ULONG size = (ULONG)base->device.dd_Library.lib_NegSize + base->device.dd_Library.lib_PosSize;
	FreeMem((APTR)((ULONG)base - base->device.dd_Library.lib_NegSize), size);
	return segList;
}

static APTR initFunction(struct VCHIQDevice *base asm("d0"), ULONG segList asm("a0"), struct ExecBase *SysBase asm("a6"))
{
	if (!emu68_has_dcache_range_ops(SysBase))
	{
		Kprintf("[vchiq] %s: rangeops build, but Emu68 lacks dcache-range-ops rev 1 - refusing to load. Install the standard driver package or update Emu68.\n", __func__);
		ULONG size = (ULONG)base->device.dd_Library.lib_NegSize + base->device.dd_Library.lib_PosSize;
		FreeMem((APTR)((ULONG)base - base->device.dd_Library.lib_NegSize), size);
		return NULL;
	}

	base->segList = segList;
	base->sysBase = SysBase;
	base->device.dd_Library.lib_Revision = (UWORD)DEVICE_REVISION;
	InitSemaphore(&base->lock);
	drv_waitq_init(&base->waitq);
	_NewMinList(&base->units);
	base->irq_signal = -1;

	if (!reset_guard_install(&base->resetGuard, SysBase, vchiq_link_reset, base, (CONST_STRPTR)deviceName))
		Kprintf("[vchiq] %s: reset guard install failed\n", __func__);

	return base;
}

/*
 * Unit 0, a struct IOVchiqReq naming the service.  Brings the connection up
 * on first use, then opens the service and waits for the VPU's answer (the
 * lock is released meanwhile, Exec's Forbid() broken by the wait, as by any
 * device that talks to its hardware here).
 */
static void openLib(struct IOVchiqReq *io asm("a1"), ULONG unitNumber asm("d0"), ULONG flags asm("d1"), struct VCHIQDevice *base asm("a6"))
{
	struct ExecBase *SysBase = base->sysBase;
	(void)flags;

	if (unitNumber != 0 || io->iovq_Req.io_Message.mn_Length < sizeof(struct IOVchiqReq))
	{
		io->iovq_Req.io_Error = IOERR_OPENFAIL;
		io->iovq_Req.io_Device = NULL; /* a CloseDevice() after the failed open must not reach closeLib */
		io->iovq_Req.io_Unit = NULL;
		return;
	}

	base->device.dd_Library.lib_OpenCnt++; /* no expunge while we wait */
	ObtainSemaphore(&base->lock);
	s32 rc = base->up ? 0 : vchiq_link_up(base);
	struct VCHIQUnit *unit = rc == 0 ? UnitOpen(base, io, &rc) : NULL;
	if (unit != NULL)
	{
		io->iovq_Req.io_Unit = &unit->unit;
		io->iovq_Req.io_Error = 0;
		io->iovq_PeerVersion = unit->svc != NULL ? (UWORD)unit->svc->peer_version : 0;
		io->iovq_Req.io_Message.mn_Node.ln_Type = NT_REPLYMSG; /* CheckIO() of the unused request says done */
		base->device.dd_Library.lib_Flags &= (UBYTE)~LIBF_DELEXP;
	}
	else
	{
		io->iovq_Req.io_Error = (BYTE)rc;
		io->iovq_Req.io_Device = NULL; /* a CloseDevice() after the failed open must not reach closeLib */
		io->iovq_Req.io_Unit = NULL;
		base->device.dd_Library.lib_OpenCnt--;
	}
	ReleaseSemaphore(&base->lock);
}

static ULONG closeLib(struct IOVchiqReq *io asm("a1"), struct VCHIQDevice *base asm("a6"))
{
	struct ExecBase *SysBase = base->sysBase;
	struct VCHIQUnit *unit = (struct VCHIQUnit *)io->iovq_Req.io_Unit;

	ObtainSemaphore(&base->lock);
	if (unit != NULL)
		UnitClose(base, unit);
	io->iovq_Req.io_Unit = NULL;
	io->iovq_Req.io_Device = NULL;
	ReleaseSemaphore(&base->lock);

	base->device.dd_Library.lib_OpenCnt--;
	if (base->device.dd_Library.lib_OpenCnt == 0 && (base->device.dd_Library.lib_Flags & LIBF_DELEXP))
		return expungeLib(base);
	return 0;
}

static APTR extFunc(struct VCHIQDevice *base asm("a6"))
{
	(void)base;
	return NULL;
}

static const APTR funcTable[] = {
    (APTR)openLib,
    (APTR)closeLib,
    (APTR)expungeLib,
    (APTR)extFunc,
    (APTR)beginIO,
    (APTR)abortIO,
    (APTR)-1,
};

static const APTR initTable[4] = {
    (APTR)sizeof(struct VCHIQDevice),
    (APTR)funcTable,
    NULL,
    (APTR)initFunction,
};
