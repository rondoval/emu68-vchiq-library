/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * swconv.c - swconv.codec, a software converter for the test suite only
 * (tests/test_choice.py): the smallest backend of the ABI, which converts
 * I420 to ARGB32 and RGBA32 in image calls on the 68k.  ARGB32 the VideoCore
 * does not write, RGBA32 it does: the suite shows the library choosing this
 * backend for the one and the VideoCore's for the other.  It counts its
 * conversions ("conversions"), which is how the suite tells who converted.
 * Never installed, never in the archive: the suite copies it into
 * LIBS:videocodec/ for its test and takes it out again.
 */
#define __NOLIBBASE__
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/resident.h>
#include <proto/exec.h>

#include <libraries/videocodec_backend.h>

#include <memory.h>
#include <strutil.h>

#define LIBRARY_PRIORITY 0                                          /* rt_Pri: a library in LIBS:, never in a ROM's resident list */
#define SWCONV_MAX_SIDE 4096u                                       /* the largest picture either way */
#define SWCONV_READS VCF_MASK(VCF_I420)                             /* the row's InputFormats */
#define SWCONV_WRITES (VCF_MASK(VCF_ARGB32) | VCF_MASK(VCF_RGBA32)) /* the row's OutputFormats */

#define I420_ROW_ALIGN 64u    /* the natural pitch of an I420 picture, as the library's other backend has it */
#define I420_ROWS_ALIGN 16u   /* I420 keeps whole 16 rows: its chroma starts after them */
#define ALIGN_TO(value, to) (((value) + (to) - 1u) & ~((to) - 1u))

/* BT.601 full range, YUV to RGB, in 16.16 fixed point: how the VideoCore reads YUV too */
#define FIX_SHIFT 16
#define V_TO_R 91881L  /* 1.402 */
#define U_TO_G 22554L  /* 0.344136 */
#define V_TO_G 46802L  /* 0.714136 */
#define U_TO_B 116130L /* 1.772 */
#define CHROMA_ZERO 128L
#define OPAQUE 255u

struct SwconvBase
{
	struct Library lib;
	ULONG segList;
	struct ExecBase *SysBase;
	ULONG conversions; /* image calls converted */
};

LONG __attribute__((used, section(".text.entry"))) doNotExecute(void);
LONG __attribute__((used, section(".text.entry"))) doNotExecute(void)
{
	return -1;
}

extern const UBYTE endOfCode;
static const char libraryName[] = LIBRARY_NAME;
static const char libraryIdString[] = LIBRARY_IDSTRING;
static const APTR initTable[4];

const struct Resident swconvResident __attribute__((used, section(".text.modhdr"))) = {
    RTC_MATCHWORD,
    (struct Resident *)&swconvResident,
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
 * clamp_byte() - A sample into 0 to 255.
 * @value: the sample
 *
 * Return: @value, clamped.
 */
static UBYTE clamp_byte(LONG value)
{
	return value < 0 ? 0u : value > 255 ? 255u : (UBYTE)value;
}

/**
 * convert_i420() - One I420 picture into ARGB32 or RGBA32 of the same size.
 * @source: the I420 picture: its luma pitch, rows whole 16, the chroma planes after
 * @dest:   the picture written, ARGB32 or RGBA32
 */
static void convert_i420(const struct VCImageSide *source, const struct VCImageSide *dest)
{
	ULONG width = source->Width, height = source->Height;
	ULONG pitch = source->Pitch != 0 ? source->Pitch : ALIGN_TO(width, I420_ROW_ALIGN);
	ULONG rows = ALIGN_TO(height, I420_ROWS_ALIGN);
	const UBYTE *luma = source->Data;
	const UBYTE *cb = luma + pitch * rows;
	const UBYTE *cr = cb + (pitch / 2u) * (rows / 2u);
	ULONG out_pitch = dest->Pitch != 0 ? dest->Pitch : width * 4u;
	BOOL alpha_first = dest->Format == VCF_ARGB32;
	for (ULONG y = 0; y < height; y++)
	{
		UBYTE *out = (UBYTE *)dest->Data + y * out_pitch;
		for (ULONG x = 0; x < width; x++)
		{
			LONG lum = luma[y * pitch + x];
			LONG u = (LONG)cb[(y / 2u) * (pitch / 2u) + x / 2u] - CHROMA_ZERO;
			LONG v = (LONG)cr[(y / 2u) * (pitch / 2u) + x / 2u] - CHROMA_ZERO;
			UBYTE red = clamp_byte(lum + ((V_TO_R * v) >> FIX_SHIFT));
			UBYTE green = clamp_byte(lum - ((U_TO_G * u + V_TO_G * v) >> FIX_SHIFT));
			UBYTE blue = clamp_byte(lum + ((U_TO_B * u) >> FIX_SHIFT));
			UBYTE *pixel = out + x * 4u;
			if (alpha_first)
				pixel++;
			pixel[0] = red;
			pixel[1] = green;
			pixel[2] = blue;
			out[x * 4u + (alpha_first ? 0u : 3u)] = OPAQUE;
		}
	}
}

/**
 * VCB_GetInfo() - The ABI version, the one codec row, the counter.
 * @info: the caller's structure (A0)
 * @base: the library base (A6)
 *
 * Return: 0, or VCERR_BADPARAM for a structure too small.
 */
static LONG VCB_GetInfo(struct VCInfo *info asm("a0"), struct SwconvBase *base asm("a6"))
{
	if (info == NULL || !VC_HAS_FIELD(info->Size, struct VCInfo, ABIVersion))
		return VCERR_BADPARAM;
	struct VCInfo all;
	memset(&all, 0, sizeof(all));
	all.Size = info->Size < sizeof(all) ? info->Size : (ULONG)sizeof(all);
	all.ABIVersion = VCB_ABI_VERSION;
	all.Codecs = 1;
	strlcpy((char *)all.Interface, "68k", sizeof(all.Interface));
	all.NumCounters = 1;
	if (VC_HAS_FIELD(info->Size, struct VCInfo, NumCounters) && info->Counters != NULL && info->MaxCounters >= 1u)
	{
		strlcpy((char *)info->Counters[0].Name, "conversions", sizeof(info->Counters[0].Name));
		info->Counters[0].Value = base->conversions;
	}
	all.Counters = info->Counters;
	all.MaxCounters = info->MaxCounters;
	memcpy(info, &all, all.Size);
	return 0;
}

/**
 * VCB_QueryCodec() - The one row: a converter for image calls, I420 to ARGB32 and RGBA32, at software priority.
 * @index: the row (D0)
 * @info:  the caller's structure (A0)
 * @base:  the library base (A6)
 *
 * Return: 0, or VCERR_BADPARAM past the row or for a structure too small.
 */
static LONG VCB_QueryCodec(ULONG index asm("d0"), struct VCCodecInfo *info asm("a0"), struct SwconvBase *base asm("a6"))
{
	(void)base;
	if (index != 0 || info == NULL || !VC_HAS_FIELD(info->Size, struct VCCodecInfo, Kind))
		return VCERR_BADPARAM;

	struct VCCodecInfo row;
	memset(&row, 0, sizeof(row));
	row.Size = info->Size < sizeof(row) ? info->Size : (ULONG)sizeof(row);
	row.Codec = VCC_Raw;
	row.Kind = VCK_Converter;
	row.MaxWidth = row.MaxHeight = SWCONV_MAX_SIDE;
	row.OutputFormats = SWCONV_WRITES;
	row.InputFormats = SWCONV_READS;
	row.Priority = VCB_PRIORITY_SOFTWARE;
	row.Flags = VCCF_Image;
	memcpy(info, &row, row.Size);
	return 0;
}

/**
 * format_in() - Whether a format is one of a mask's.
 * @format: VCF_*, as the caller gave it
 * @mask:   VCF_MASK()s
 *
 * Return: TRUE when it is; FALSE for a number past the mask's 32 bits.
 */
static BOOL format_in(ULONG format, ULONG mask)
{
	return format < 32u && (mask & VCF_MASK(format)) != 0;
}

/**
 * image_supported() - Whether swconv does an image call: I420 into ARGB32 or RGBA32, whole pictures of one size.
 * @params: the call
 *
 * Return: TRUE when it does.
 */
static BOOL image_supported(const struct VCImageParams *params)
{
	const struct VCImageSide *source = &params->Source, *dest = &params->Dest;
	if (params->Kind != VCK_Converter || source->Data == NULL || dest->Data == NULL)
		return FALSE;
	if (!format_in(source->Format, SWCONV_READS) || !format_in(dest->Format, SWCONV_WRITES))
		return FALSE;
	if (source->Width == 0 || source->Height == 0 || source->Width > SWCONV_MAX_SIDE || source->Height > SWCONV_MAX_SIDE)
		return FALSE;
	BOOL dest_sized = dest->Width != 0 || dest->Height != 0; /* none given: the source's */
	if (dest_sized && (dest->Width != source->Width || dest->Height != source->Height))
		return FALSE; /* no scaling */
	return !source->HaveRect && !dest->HaveRect && !params->KeepAspect; /* no cropping or placing */
}

/**
 * VCB_Image() - One conversion, I420 to ARGB32 or RGBA32 of the same size, in the caller's task.
 * @params: the call (A0)
 * @base:   the library base (A6)
 *
 * Return: 0, or VCERR_UNSUPPORTED for anything else (image_supported()).
 */
static LONG VCB_Image(struct VCImageParams *params asm("a0"), struct SwconvBase *base asm("a6"))
{
	if (!image_supported(params))
		return VCERR_UNSUPPORTED;
	convert_i420(&params->Source, &params->Dest);
	base->conversions++;
	return 0;
}

/**
 * VCB_OpenSession() - None: the row serves image calls only.
 * @params: the parameters (A0)
 * @error:  where the error goes (A1)
 * @base:   the library base (A6)
 *
 * Return: NULL, VCERR_UNSUPPORTED at @error.
 */
static APTR VCB_OpenSession(struct VCSessionParams *params asm("a0"), LONG *error asm("a1"), struct SwconvBase *base asm("a6"))
{
	(void)params;
	(void)base;
	if (error != NULL)
		*error = VCERR_UNSUPPORTED;
	return NULL;
}

/**
 * LibExpunge() - Unload when no one has it open.
 * @base: the library base (A6)
 *
 * Return: the segment list to unload, 0 while open.
 */
static ULONG LibExpunge(struct SwconvBase *base asm("a6"))
{
	struct ExecBase *SysBase = base->SysBase;
	if (base->lib.lib_OpenCnt > 0)
	{
		base->lib.lib_Flags |= LIBF_DELEXP;
		return 0;
	}
	ULONG segList = base->segList;
	Forbid();
	Remove((struct Node *)base);
	Permit();
	FreeMem((APTR)((ULONG)base - base->lib.lib_NegSize), (ULONG)base->lib.lib_NegSize + base->lib.lib_PosSize);
	return segList;
}

/**
 * LibInit() - The base: nothing converted yet.
 * @lib:     the library base (D0), cleared by MakeLibrary()
 * @seglist: the segment list (A0)
 * @SysBase: exec (A6)
 *
 * Return: the base.
 */
static struct Library *LibInit(struct Library *lib asm("d0"), ULONG seglist asm("a0"), struct ExecBase *SysBase asm("a6"))
{
	struct SwconvBase *base = (struct SwconvBase *)lib;
	base->segList = seglist;
	base->SysBase = SysBase;
	base->lib.lib_Revision = (UWORD)LIBRARY_REVISION;
	return lib;
}

/**
 * LibOpen() - Count an opener.
 * @version: the version asked for (D0)
 * @base:    the library base (A6)
 *
 * Return: the base.
 */
static struct SwconvBase *LibOpen(ULONG version asm("d0"), struct SwconvBase *base asm("a6"))
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
 * Return: the segment list if expunged, else 0.
 */
static ULONG LibClose(struct SwconvBase *base asm("a6"))
{
	base->lib.lib_OpenCnt--;
	if (base->lib.lib_OpenCnt == 0 && (base->lib.lib_Flags & LIBF_DELEXP))
		return LibExpunge(base);
	return 0;
}

/**
 * LibNull() - The reserved vector, and the session calls that answer nothing, NULL or 0: there are no sessions.
 *
 * Return: 0.
 */
static ULONG LibNull(void)
{
	return 0;
}

/**
 * session_refused() - The session calls that answer with an error: there are no sessions.
 *
 * Return: VCERR_BADSTATE.
 */
static LONG session_refused(void)
{
	return VCERR_BADSTATE;
}

static const APTR funcTable[] = {
    (APTR)LibOpen,
    (APTR)LibClose,
    (APTR)LibExpunge,
    (APTR)LibNull,
    (APTR)VCB_GetInfo,
    (APTR)VCB_QueryCodec,
    (APTR)VCB_OpenSession,
    (APTR)LibNull,         /* VCB_CloseSession */
    (APTR)LibNull,         /* VCB_ImportPacket: NULL */
    (APTR)LibNull,         /* VCB_ImportFrame: NULL */
    (APTR)session_refused, /* VCB_SendPacket */
    (APTR)session_refused, /* VCB_SendFrame */
    (APTR)LibNull,         /* VCB_Poll: nothing */
    (APTR)LibNull,         /* VCB_ReleaseFrame */
    (APTR)LibNull,         /* VCB_ReleasePacket */
    (APTR)session_refused, /* VCB_Flush */
    (APTR)session_refused, /* VCB_SetAttrs */
    (APTR)VCB_Image,
    (APTR)LibNull,         /* VCB_Idle: nothing kept */
    (APTR)LibNull,         /* VCB_OutputFrame: NULL */
    (APTR)-1,
};

static const APTR initTable[4] = {
    (APTR)sizeof(struct SwconvBase),
    (APTR)funcTable,
    NULL,
    (APTR)LibInit,
};
