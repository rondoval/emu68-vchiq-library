/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * vcjpeg_lib.c - resident, library init/open/close/expunge, ObtainEngine().
 *
 * A datatype class library: datatypes.library opens it by the descriptor's
 * base name (datatypes/vcjpeg.datatype) and takes the class from
 * ObtainEngine(), the first function after the four standard ones.  The class
 * is public too, under the library's name.
 */
#include <vcjpeg_datatype.h> /* first: __NOLIBBASE__ for the proto headers below */

#include <exec/memory.h>
#include <exec/resident.h>
#include <proto/exec.h>

#ifndef LIBRARY_PRIORITY
#define LIBRARY_PRIORITY 0
#endif

LONG __attribute__((used, section(".text.entry"))) doNotExecute(void);
LONG __attribute__((used, section(".text.entry"))) doNotExecute(void)
{
	return -1;
}

extern const UBYTE endOfCode;
static const char libraryName[] = LIBRARY_NAME;
static const char libraryIdString[] = LIBRARY_IDSTRING;
static const APTR initTable[4];

const struct Resident vcjpegResident __attribute__((used, section(".text.modhdr"))) = {
    RTC_MATCHWORD,
    (struct Resident *)&vcjpegResident,
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
 * lib_close_all() - Close what the library opened (any of them may be NULL).
 * @base: the library base
 */
static void lib_close_all(struct VCJpegBase *base)
{
	struct ExecBase *SysBase = base->SysBase;
	if (base->VideoCodecBase != NULL)
		CloseLibrary(base->VideoCodecBase);
	if (base->SuperClassBase != NULL)
		CloseLibrary(base->SuperClassBase);
	if (base->DataTypesBase != NULL)
		CloseLibrary(base->DataTypesBase);
	if (base->UtilityBase != NULL)
		CloseLibrary(base->UtilityBase);
	if (base->IntuitionBase != NULL)
		CloseLibrary(base->IntuitionBase);
	if (base->DOSBase != NULL)
		CloseLibrary(base->DOSBase);
}

/**
 * lib_free() - Free the library base's memory (the function table below it included).
 * @base: the library base, no longer in the system's list
 */
static void lib_free(struct VCJpegBase *base)
{
	struct ExecBase *SysBase = base->SysBase;
	FreeMem((APTR)((ULONG)base - base->lib.lib_NegSize), (ULONG)base->lib.lib_NegSize + base->lib.lib_PosSize);
}

static ULONG LibExpunge(struct VCJpegBase *base asm("a6"))
{
	struct ExecBase *SysBase = base->SysBase;
	ULONG segList = base->segList;

	/* later: still open, the idle task still running (told to close now), or objects of the class alive */
	if (base->lib.lib_OpenCnt > 0 || !vcjpeg_codec_closed(base) || !vcjpeg_free_class(base))
	{
		base->lib.lib_Flags |= LIBF_DELEXP;
		return 0;
	}
	lib_close_all(base);

	Forbid();
	Remove((struct Node *)base);
	Permit();

	lib_free(base);
	return segList;
}

static struct Library *LibInit(struct Library *lib asm("d0"), ULONG seglist asm("a0"), struct ExecBase *SysBase asm("a6"))
{
	struct VCJpegBase *base = (struct VCJpegBase *)lib;

	base->segList = seglist;
	base->SysBase = SysBase;
	base->lib.lib_Revision = (UWORD)LIBRARY_REVISION;
	base->shared.Size = sizeof(base->shared);
	base->shared.MinPixels = VCJPEG_MIN_PIXELS;
	InitSemaphore(&base->codec_lock);
	InitSemaphore(&base->fallback_lock);
	base->DOSBase = OpenLibrary((CONST_STRPTR) "dos.library", 39);
	base->IntuitionBase = OpenLibrary((CONST_STRPTR) "intuition.library", 39);
	base->UtilityBase = OpenLibrary((CONST_STRPTR) "utility.library", 39);
	base->DataTypesBase = OpenLibrary((CONST_STRPTR) "datatypes.library", 44);         /* DTST_MEMORY: the hand-over */
	base->SuperClassBase = OpenLibrary((CONST_STRPTR) "datatypes/picture.datatype", 43); /* V43 mode: pixels by method */
	if (base->DOSBase == NULL || base->IntuitionBase == NULL || base->UtilityBase == NULL || base->DataTypesBase == NULL ||
	    base->SuperClassBase == NULL || vcjpeg_make_class(base) == NULL)
	{
		lib_close_all(base);
		lib_free(base);
		return NULL;
	}
	return lib;
}

static struct VCJpegBase *LibOpen(ULONG version asm("d0"), struct VCJpegBase *base asm("a6"))
{
	(void)version;
	base->lib.lib_OpenCnt++;
	base->lib.lib_Flags &= (UBYTE)~LIBF_DELEXP;
	return base;
}

static ULONG LibClose(struct VCJpegBase *base asm("a6"))
{
	base->lib.lib_OpenCnt--;
	if (base->lib.lib_OpenCnt == 0 && (base->lib.lib_Flags & LIBF_DELEXP))
		return LibExpunge(base);
	return 0;
}

static ULONG LibNull(void)
{
	return 0;
}

/**
 * ObtainEngine() - The class, for datatypes.library's NewDTObject().
 * @base: the library base
 *
 * Return: the class.
 */
static Class *ObtainEngine(struct VCJpegBase *base asm("a6"))
{
	return base->cl;
}

static const APTR funcTable[] = {
    (APTR)LibOpen, (APTR)LibClose, (APTR)LibExpunge, (APTR)LibNull, (APTR)ObtainEngine, (APTR)-1,
};

static const APTR initTable[4] = {
    (APTR)sizeof(struct VCJpegBase),
    (APTR)funcTable,
    NULL,
    (APTR)LibInit,
};
