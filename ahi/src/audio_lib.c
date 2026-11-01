/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * audio_lib.c - resident, library init/open/close/expunge, the function table.
 *
 * ahi.device opens the driver to read its modes' attributes as well as to
 * play, so opening touches nothing of the VPU: each AHIsub_AllocAudio()
 * opens its own AUDS service (audio_slave.c).
 */
#include <exec/memory.h>
#include <exec/resident.h>

#include <videocore_audio.h>

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
const char vca_idstring[] = LIBRARY_IDSTRING; /* "$VER: " and the version string AHIDB_Version gives */
static const APTR initTable[4];

const struct Resident videocoreAudioResident __attribute__((used, section(".text.modhdr"))) = {
    RTC_MATCHWORD,
    (struct Resident *)&videocoreAudioResident,
    (APTR)&endOfCode,
    RTF_AUTOINIT,
    LIBRARY_VERSION,
    NT_LIBRARY,
    LIBRARY_PRIORITY,
    (APTR)&libraryName,
    (APTR)&vca_idstring,
    (APTR)initTable,
};

_Static_assert(__builtin_offsetof(struct VCAudioBase, SysBase) == 34, "the Disable/Enable stubs read SysBase at 34(a6)");

/*
 * AHIsub_Disable() and AHIsub_Enable(): Forbid() and Permit(), so the slave
 * cannot start a pass while ahi.device changes a channel.  A V4 driver
 * preserves every register here; Forbid() and Permit() do, so the stubs only
 * save the base register they load SysBase into.
 */
void vca_disable(void);
void vca_enable(void);
__asm__("	.text\n"
        "	.even\n"
        "_vca_disable:\n"
        "	move.l	%a6,-(%sp)\n"
        "	move.l	34(%a6),%a6\n"
        "	jsr	-132(%a6)\n" /* Forbid */
        "	move.l	(%sp)+,%a6\n"
        "	rts\n"
        "_vca_enable:\n"
        "	move.l	%a6,-(%sp)\n"
        "	move.l	34(%a6),%a6\n"
        "	jsr	-138(%a6)\n" /* Permit */
        "	move.l	(%sp)+,%a6\n"
        "	rts\n");

/**
 * LibExpunge() - Remove the library once nothing uses it.
 * @base: the library base (A6)
 *
 * Every stream belongs to an AHIsub_AllocAudio() of an opener, so an open
 * count of 0 means none is left.
 *
 * Return: the segment list to unload, 0 while it is open.
 */
static ULONG LibExpunge(struct VCAudioBase *base asm("a6"))
{
	struct ExecBase *SysBase = base->SysBase;
	ULONG segList = base->segList;

	if (base->lib.lib_OpenCnt > 0)
	{
		base->lib.lib_Flags |= LIBF_DELEXP;
		return 0;
	}
	CloseLibrary(base->UtilityBase);

	Forbid();
	Remove((struct Node *)base);
	Permit();

	ULONG size = (ULONG)base->lib.lib_NegSize + base->lib.lib_PosSize;
	FreeMem((APTR)((ULONG)base - base->lib.lib_NegSize), size);
	return segList;
}

/**
 * LibInit() - Set the library base up: utility.library for the hooks and tags.
 * @lib:     the library base (D0), cleared by MakeLibrary()
 * @seglist: the segment list (A0)
 * @SysBase: exec (A6)
 *
 * Return: the base, or NULL without utility.library (the base is freed).
 */
static struct Library *LibInit(struct Library *lib asm("d0"), ULONG seglist asm("a0"), struct ExecBase *SysBase asm("a6"))
{
	struct VCAudioBase *base = (struct VCAudioBase *)lib;

	base->segList = seglist;
	base->SysBase = SysBase;
	base->lib.lib_Revision = (UWORD)LIBRARY_REVISION;
	base->UtilityBase = OpenLibrary((CONST_STRPTR) "utility.library", 36);
	if (base->UtilityBase == NULL)
	{
		ULONG size = (ULONG)lib->lib_NegSize + lib->lib_PosSize;
		FreeMem((APTR)((ULONG)lib - lib->lib_NegSize), size);
		return NULL;
	}
	return lib;
}

/**
 * LibOpen() - Count an opener and cancel a delayed expunge.
 * @version: the version asked for (D0)
 * @base:    the library base (A6)
 *
 * Return: the base.
 */
static struct VCAudioBase *LibOpen(ULONG version asm("d0"), struct VCAudioBase *base asm("a6"))
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
static ULONG LibClose(struct VCAudioBase *base asm("a6"))
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

/* The AHI sub-driver interface (ahi_sub.sfd): AHIsub_AllocAudio at -30 to AHIsub_HardwareControl at -114. */
static const APTR funcTable[] = {
    (APTR)LibOpen,
    (APTR)LibClose,
    (APTR)LibExpunge,
    (APTR)LibNull,
    (APTR)AHIsub_AllocAudio,
    (APTR)AHIsub_FreeAudio,
    (APTR)vca_disable,
    (APTR)vca_enable,
    (APTR)AHIsub_Start,
    (APTR)AHIsub_Update,
    (APTR)AHIsub_Stop,
    (APTR)AHIsub_Unknown, /* SetVol */
    (APTR)AHIsub_Unknown, /* SetFreq */
    (APTR)AHIsub_Unknown, /* SetSound */
    (APTR)AHIsub_Unknown, /* SetEffect */
    (APTR)AHIsub_Unknown, /* LoadSound */
    (APTR)AHIsub_Unknown, /* UnloadSound */
    (APTR)AHIsub_GetAttr,
    (APTR)AHIsub_HardwareControl,
    (APTR)-1,
};

static const APTR initTable[4] = {
    (APTR)sizeof(struct VCAudioBase),
    (APTR)funcTable,
    NULL,
    (APTR)LibInit,
};
