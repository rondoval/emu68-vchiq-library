/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * videocore_lib.c - resident, library init/open/close/expunge.
 *
 * The library is a videocodec backend: its function table is the backend
 * ABI (videocodec_backend.sfd).  Nothing touches the VPU here: vchiq.device
 * is opened with the first session or image call and closed at VCB_Idle(),
 * the frontend's last close (mmal_client.c, videocore_link.c), so a library
 * nobody has open holds no service and no task.  The DMA pool and the
 * scratch, made at the first service open, stay until the expunge.
 */
#include <exec/memory.h>
#include <exec/resident.h>

#include <minlist.h>

#include <videocore_private.h>

/* The perf slots' names, as enum videocore_perf_slot orders them (profile tier) */
static const char *const perf_names[VCPERF_SLOT_COUNT] = {"rx_parked", "rx_payload", "out_vpu",    "out_client", "in_vpu",
                                                           "poll",      "img_call",   "img_open",   "img_wait",   "img_close",
                                                           "mmal_open", "mmal_close", "component",  "destroy",    "import",
                                                           "release"};

/**
 * doNotExecute() - The file's first code: run as a program, it returns at once.
 *
 * Return: -1.
 */
LONG __attribute__((used, section(".text.entry"))) doNotExecute(void);
LONG __attribute__((used, section(".text.entry"))) doNotExecute(void)
{
	return -1;
}

extern const UBYTE endOfCode;
static const char libraryName[] = LIBRARY_NAME;
static const char libraryIdString[] = LIBRARY_IDSTRING;
static const APTR initTable[4];

const struct Resident videocoreResident __attribute__((used, section(".text.modhdr"))) = {
    RTC_MATCHWORD,
    (struct Resident *)&videocoreResident,
    (APTR)&endOfCode,
    RTF_AUTOINIT,
    LIBRARY_VERSION,
    NT_LIBRARY,
    LIBRARY_PRIORITY,
    (APTR)&libraryName,
    (APTR)&libraryIdString,
    (APTR)initTable,
};

/**
 * LibExpunge() - Remove the library once nothing uses it.
 * @base: the library base (A6)
 *
 * Refused (delayed) while it is open, a session or component is left, a
 * close left the receive task waiting for requests the device still holds
 * (mmal_expunge()), or a later patch of ColdReboot() keeps the reset guard in
 * place.  mmal_idle() (VCB_Idle(), the frontend's last close) closed the link;
 * while it is open the expunge waits.
 *
 * Return: the segment list to unload, 0 while refused.
 */
static ULONG LibExpunge(struct VideoCoreBase *base asm("a6"))
{
	struct ExecBase *SysBase = base->SysBase;
	ULONG segList = base->segList;

	/* a close may have left the receive task waiting for requests the device still holds: not while it runs */
	if (base->lib.lib_OpenCnt > 0 || base->session_count > 0 || !mmal_expunge(base) || (base->reset_guarded && !reset_guard_remove(&base->reset_guard)))
	{
		base->lib.lib_Flags |= LIBF_DELEXP;
		return 0;
	}
	mmal_shutdown(base);
	drv_waitq_cleanup(&base->waitq, SysBase);

	Forbid();
	Remove((struct Node *)base);
	Permit();

	ULONG size = (ULONG)base->lib.lib_NegSize + base->lib.lib_PosSize;
	FreeMem((APTR)((ULONG)base - base->lib.lib_NegSize), size);
	return segList;
}

/**
 * LibInit() - Set the library base up: locks, the timings, the MMAL client's state, no sessions.
 * @lib:     the library base (D0), cleared by MakeLibrary()
 * @seglist: the segment list (A0)
 * @SysBase: exec (A6)
 *
 * Return: the base.
 */
static struct Library *LibInit(struct Library *lib asm("d0"), ULONG seglist asm("a0"), struct ExecBase *SysBase asm("a6"))
{
	struct VideoCoreBase *base = (struct VideoCoreBase *)lib;

	base->segList = seglist;
	base->SysBase = SysBase;
	base->lib.lib_Revision = (UWORD)LIBRARY_REVISION;
	InitSemaphore(&base->mmal_lock);
	InitSemaphore(&base->session_lock);
	InitSemaphore(&base->image_lock);
	drv_waitq_init(&base->waitq);
	base->perf = (struct perf){"videocore", perf_names, base->perf_slots, VCPERF_SLOT_COUNT};
	mmal_init(base);
	sessions_init(base);
	return lib;
}

/**
 * LibOpen() - Count an opener and cancel a delayed expunge.
 * @version: the version asked for (D0); the frontend checks the ABI instead
 * @base:    the library base (A6)
 *
 * Return: the base.
 */
static struct VideoCoreBase *LibOpen(ULONG version asm("d0"), struct VideoCoreBase *base asm("a6"))
{
	(void)version;
	base->lib.lib_OpenCnt++;
	base->lib.lib_Flags &= (UBYTE)~LIBF_DELEXP;
	return base;
}

/**
 * LibClose() - Count an opener off; the last one runs a delayed expunge.
 * @base: the library base (A6)
 *
 * Return: the segment list if the library was expunged, else 0.
 */
static ULONG LibClose(struct VideoCoreBase *base asm("a6"))
{
	base->lib.lib_OpenCnt--;
	if (base->lib.lib_OpenCnt == 0 && (base->lib.lib_Flags & LIBF_DELEXP))
		return LibExpunge(base);
	return 0;
}

/**
 * LibNull() - The reserved vector.
 *
 * Return: 0.
 */
static ULONG LibNull(void)
{
	return 0;
}

static const APTR funcTable[] = {
    (APTR)LibOpen,
    (APTR)LibClose,
    (APTR)LibExpunge,
    (APTR)LibNull,
    (APTR)VCB_GetInfo,
    (APTR)VCB_QueryCodec,
    (APTR)VCB_OpenSession,
    (APTR)VCB_CloseSession,
    (APTR)VCB_ImportPacket,
    (APTR)VCB_ImportFrame,
    (APTR)VCB_SendPacket,
    (APTR)VCB_SendFrame,
    (APTR)VCB_Poll,
    (APTR)VCB_ReleaseFrame,
    (APTR)VCB_ReleasePacket,
    (APTR)VCB_Flush,
    (APTR)VCB_SetAttrs,
    (APTR)VCB_Image,
    (APTR)VCB_Idle,
    (APTR)-1,
};

static const APTR initTable[4] = {
    (APTR)sizeof(struct VideoCoreBase),
    (APTR)funcTable,
    NULL,
    (APTR)LibInit,
};
