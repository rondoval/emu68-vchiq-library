/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * vcjpeg_class.c - the class: a JPEG into picture.datatype's pixels, through
 * the VPU or, failing that, through the system's JPEG class.
 *
 * OM_NEW reads the whole file, sizes the picture from its header (the shared
 * image_info.c) and has the VPU decode it to RGB24 with VC_DecodeImage():
 * straight into picture.datatype's own buffer when the VPU can write it
 * (fits_vpu()), else into a buffer of ours and from there into the picture,
 * one copy.
 *
 * When the VPU fails, the system's class decodes the picture: a second object
 * on the same file, which the descriptor's recognition function leaves to the
 * next descriptor because it finds this task in shared.Fallback.  The second
 * object is made by the file's full name, not on the bytes already read
 * (DTST_MEMORY, only when there is no file): AmiKit's JFIF function examines
 * the file's lock, which a memory source lacks.  Its pixels are read back
 * with PDTM_READPIXELARRAY, one copy, by a direct method call: DoDTMethodA()
 * would put a GadgetInfo pointer over the message's pixel pointer
 * (docs/architecture.md section 8).
 *
 * videocodec.library is opened by the first picture and closed by an idle
 * task once VCJPEG_IDLE_MS pass with none: its last close lets the backend
 * give back the VPU memory its kept pipeline holds between pictures.
 */
#include <vcjpeg_datatype.h> /* first: __NOLIBBASE__ for the proto headers below */

#include <datatypes/datatypes.h>
#include <datatypes/datatypesclass.h>
#include <datatypes/pictureclass.h>
#include <dos/dos.h>
#include <exec/memory.h>
#include <libraries/videocodec.h>
#include <utility/hooks.h>

#include <driver_task.h>
#include <drv_timer.h>

#include <proto/datatypes.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/videocodec.h>

#include <bits.h>
#include <debug.h>

#include <image_info.h>

#define ROW_PIXELS 32u /* the VPU writes rows of whole groups of this many pixels, and an even number of rows */
#define RGB_BYTES 3u
#define NAME_MAX 512u /* a file's full name, for the hand-over to software */

/* A loaded file */
struct source
{
	BPTR fh; /* the object's handle: its name for the hand-over */
	UBYTE *data;
	ULONG length;
};

/* Where the picture's pixels go: picture.datatype's own buffer, or none (by PDTM_WRITEPIXELARRAY) */
struct target
{
	struct pdtBlitPixelArray buffer; /* pbpa_PixelData NULL: no direct buffer */
	ULONG width, height;
};

/**
 * super_method() - A method on the object, by the superclass (picture.datatype).
 * @cl:  the class
 * @o:   the object
 * @msg: the message
 *
 * Return: what the method returns.
 */
static ULONG super_method(Class *cl, Object *o, Msg msg)
{
	struct VCJpegBase *base = (struct VCJpegBase *)cl->cl_UserData;
	struct Library *UtilityBase = base->UtilityBase;
	return CallHookPkt(&cl->cl_Super->cl_Dispatcher, o, msg);
}

/**
 * object_method() - A method on another class's object: its own dispatcher, no GadgetInfo.
 * @base: the library base
 * @o:    the object
 * @msg:  the message
 *
 * Return: what the method returns.
 */
static ULONG object_method(struct VCJpegBase *base, Object *o, Msg msg)
{
	struct Library *UtilityBase = base->UtilityBase;
	return CallHookPkt(&OCLASS(o)->cl_Dispatcher, o, msg);
}

#define IDLE_TASK_NAME "vcjpeg.datatype idle"
#define IDLE_TASK_STACK 8192 /* the backend's idle runs on it: it closes the VPU's service */

/**
 * idle_task() - Close videocodec.library once a while passes with no picture, then end.
 * @base:   the library base
 * @parent: the task that started it, told once it runs
 *
 * Every VCJPEG_IDLE_MS it looks: with no picture since the last look and none
 * on the VPU, it closes the library, whose last close lets the backend give
 * the VPU its memory back (the image calls' kept pipeline and ISP).  CTRL_C
 * (the expunge) closes it at the next look that finds no picture on the VPU.
 * The slot clears with the close, under codec_lock and in Forbid()
 * (drv_task_exit()): a picture that opens the library again starts a task of
 * its own, and an expunge never frees this one's code under it.
 */
static void idle_task(struct VCJpegBase *base, struct Task *parent)
{
	struct ExecBase *SysBase = base->SysBase;
	struct drv_timer timer;
	if (!drv_timer_open(&timer, SysBase))
	{
		drv_task_exit(SysBase, &base->idle_task, parent, FALSE);
		return;
	}
	base->idle_task = FindTask(NULL);
	Signal(parent, SIGBREAKF_CTRL_F);

	for (;;)
	{
		ULONG signals = drv_timer_wait(&timer, SIGBREAKF_CTRL_C, VCJPEG_IDLE_MS);

		ObtainSemaphore(&base->codec_lock);
		BOOL idle = base->decoding == 0 && (!base->used || (signals & SIGBREAKF_CTRL_C) != 0);
		base->used = FALSE;
		if (idle)
		{
			CloseLibrary(base->VideoCodecBase); /* its last close may wait for the VPU: a picture meanwhile waits for the lock */
			base->VideoCodecBase = NULL;
			base->shared.VPUDown = 0; /* a VPU lost before: the next picture opens the library again and tries it */
			Kprintf("[vcjpeg] idle: videocodec.library closed\n");
			drv_timer_close(&timer);
			/* in Forbid() until the task is gone; its report goes to itself, as the task that started it may be gone by now */
			drv_task_exit(SysBase, &base->idle_task, FindTask(NULL), TRUE);
			ReleaseSemaphore(&base->codec_lock);
			return;
		}
		ReleaseSemaphore(&base->codec_lock);
	}
}

/**
 * codec_take() - videocodec.library for a picture: opened if it is not, with its idle task.
 * @base: the library base
 *
 * Opened here, not at init: its first call scans the backends' drawer,
 * which needs a Process (the picture's caller is one).  Without an idle task
 * (no memory) it stays open until the expunge.  drv_task_spawn() waits for
 * the task's report on the calling task's CTRL_F and CTRL_C: the picture's
 * caller's.  Each take is given back with codec_give().
 *
 * Return: the library, or NULL (the picture then sees VCERR_NOBACKEND).
 */
static struct Library *codec_take(struct VCJpegBase *base)
{
	struct ExecBase *SysBase = base->SysBase;
	ObtainSemaphore(&base->codec_lock);
	if (base->VideoCodecBase == NULL)
	{
		base->VideoCodecBase = OpenLibrary((CONST_STRPTR)VIDEOCODEC_NAME, 0);
		if (base->VideoCodecBase != NULL && drv_task_spawn(SysBase, base, (APTR)idle_task, IDLE_TASK_NAME, IDLE_TASK_STACK, 0) != 0)
			Kprintf("[vcjpeg] no idle task: videocodec.library stays open until the expunge\n");
	}
	struct Library *VideoCodecBase = base->VideoCodecBase;
	if (VideoCodecBase != NULL)
		base->decoding++;
	ReleaseSemaphore(&base->codec_lock);
	return VideoCodecBase;
}

/**
 * codec_give() - A picture is off the VPU: the idle task counts from here.
 * @base: the library base
 */
static void codec_give(struct VCJpegBase *base)
{
	struct ExecBase *SysBase = base->SysBase;
	ObtainSemaphore(&base->codec_lock);
	base->decoding--;
	base->used = TRUE;
	ReleaseSemaphore(&base->codec_lock);
}

/**
 * vcjpeg_codec_closed() - At the expunge: whether videocodec.library's idle task is gone.
 * @base: the library base, under Forbid()
 *
 * A running one is told to close the library now: the expunge waits for
 * nothing, and the next one finds the task gone.
 *
 * Return: TRUE if no idle task runs.
 */
BOOL vcjpeg_codec_closed(struct VCJpegBase *base)
{
	struct ExecBase *SysBase = base->SysBase;
	if (base->idle_task == NULL)
		return TRUE;
	Signal(base->idle_task, SIGBREAKF_CTRL_C);
	return FALSE;
}

/**
 * source_read() - The whole file, from the object's handle.
 * @base: the library base
 * @fh:   the object's file handle (a DTST_MEMORY source reads as one too)
 * @src:  filled with the bytes
 *
 * Return: 0, or an IoErr() code.
 */
static LONG source_read(struct VCJpegBase *base, BPTR fh, struct source *src)
{
	struct ExecBase *SysBase = base->SysBase;
	struct Library *DOSBase = base->DOSBase;
	if (Seek(fh, 0, OFFSET_END) < 0)
		return IoErr();
	LONG length = Seek(fh, 0, OFFSET_BEGINNING);
	if (length <= 0)
		return length < 0 ? IoErr() : DTERROR_NOT_ENOUGH_DATA;
	src->data = AllocVec((ULONG)length, MEMF_PUBLIC);
	if (src->data == NULL)
		return ERROR_NO_FREE_STORE;
	if (Read(fh, src->data, length) != length)
	{
		LONG error = IoErr();
		FreeVec(src->data);
		src->data = NULL;
		return error != 0 ? error : DTERROR_NOT_ENOUGH_DATA;
	}
	src->length = (ULONG)length;
	return 0;
}

/**
 * target_set() - The picture's size, and picture.datatype's own buffer for it when it has one (V47).
 * @cl:     the class
 * @o:      the object
 * @width:  pixels
 * @height: rows
 * @t:      filled
 *
 * Return: 0, or DTERROR_INVALID_DATA without a bitmap header.
 */
static LONG target_set(Class *cl, Object *o, ULONG width, ULONG height, struct target *t)
{
	struct VCJpegBase *base = (struct VCJpegBase *)cl->cl_UserData;
	struct Library *DataTypesBase = base->DataTypesBase;
	struct BitMapHeader *bmh = NULL;
	GetDTAttrs(o, PDTA_BitMapHeader, (ULONG)&bmh, TAG_DONE);
	if (bmh == NULL)
		return DTERROR_INVALID_DATA;
	bmh->bmh_Width = (UWORD)width;
	bmh->bmh_Height = (UWORD)height;
	bmh->bmh_PageWidth = (WORD)width;
	bmh->bmh_PageHeight = (WORD)height;
	bmh->bmh_Depth = 24;
	bmh->bmh_Masking = mskNone;
	bmh->bmh_XAspect = 1;
	bmh->bmh_YAspect = 1;

	t->width = width;
	t->height = height;
	t->buffer = (struct pdtBlitPixelArray){0};
	/*
	 * V43 mode: the pixels go in as RGB rows.  Every pixel is written, so the
	 * buffer need not be cleared first (PDTA_SubClassRendersAll).
	 * picture.datatype 47 hands out its own buffer (PDTA_ObtainPixelBuffer);
	 * an older one leaves pbpa_PixelData NULL, and the rows go by method.
	 */
	SetDTAttrs(o, NULL, NULL, PDTA_SourceMode, PMODE_V43, DTA_NominalHoriz, width, DTA_NominalVert, height, PDTA_SubClassRendersAll,
	           1, PDTA_ObtainPixelBuffer, (ULONG)&t->buffer, TAG_DONE);
	if (t->buffer.pbpa_PixelData != NULL && t->buffer.pbpa_PixelFormat != PBPAFMT_RGB)
		t->buffer.pbpa_PixelData = NULL; /* a format we do not write: by method instead */
	return 0;
}

/**
 * target_write() - Rows of RGB24 into the picture: its own buffer, or PDTM_WRITEPIXELARRAY.
 * @cl:    the class
 * @o:     the object
 * @t:     the picture
 * @pix:   the rows, @width pixels used of each
 * @pitch: bytes from one row to the next
 *
 * Return: 0, or DTERROR_INVALID_DATA if the picture class refused them.
 */
static LONG target_write(Class *cl, Object *o, const struct target *t, const UBYTE *pix, ULONG pitch)
{
	struct VCJpegBase *base = (struct VCJpegBase *)cl->cl_UserData;
	struct ExecBase *SysBase = base->SysBase;
	if (t->buffer.pbpa_PixelData != NULL)
	{
		UBYTE *row = t->buffer.pbpa_PixelData;
		for (ULONG y = 0; y < t->height; y++, row += t->buffer.pbpa_PixelArrayMod, pix += pitch)
			CopyMem((APTR)pix, row, t->width * RGB_BYTES);
		return 0;
	}
	struct pdtBlitPixelArray wpa = {PDTM_WRITEPIXELARRAY, (APTR)pix, PBPAFMT_RGB, pitch, 0, 0, t->width, t->height};
	return super_method(cl, o, (Msg)&wpa) != 0 ? 0 : DTERROR_INVALID_DATA;
}

/**
 * fits_vpu() - Whether the VPU can write straight into picture.datatype's buffer.
 * @base: the library base
 * @t:    the picture
 *
 * The backend's rules for an output picture: rows of whole groups of 32
 * pixels (the pitch, not the width) and an even number of rows.  Besides,
 * the buffer must start on a 4-byte boundary (videocore.codec refuses others)
 * and lie outside Chip RAM, which is on the Amiga's bus where the VPU cannot
 * reach.
 *
 * Return: TRUE if it can.
 */
static BOOL fits_vpu(struct VCJpegBase *base, const struct target *t)
{
	struct ExecBase *SysBase = base->SysBase;
	const struct pdtBlitPixelArray *b = &t->buffer;
	if (b->pbpa_PixelData == NULL || ((ULONG)b->pbpa_PixelData & 3u) != 0 || (TypeOfMem(b->pbpa_PixelData) & MEMF_CHIP) != 0)
		return FALSE;
	return b->pbpa_PixelArrayMod % (ROW_PIXELS * RGB_BYTES) == 0 && b->pbpa_PixelArrayMod >= t->width * RGB_BYTES && (t->height & 1u) == 0;
}

/**
 * vpu_decode_with() - The picture through the VPU, into the picture's buffer or through one of ours.
 * @cl:             the class
 * @o:              the object
 * @src:            the file's bytes
 * @t:              the picture, sized
 * @VideoCodecBase: the library, taken for this picture
 *
 * Return: 0, or the VCERR_* (the picture then goes to software).
 */
static LONG vpu_decode_with(Class *cl, Object *o, const struct source *src, const struct target *t, struct Library *VideoCodecBase)
{
	struct VCJpegBase *base = (struct VCJpegBase *)cl->cl_UserData;
	struct ExecBase *SysBase = base->SysBase;

	if (fits_vpu(base, t))
		return VC_DecodeImageTags(VCA_Data, (ULONG)src->data, VCA_Length, src->length, VCA_Codec, VCC_JPEG, VCA_Dest,
		                          (ULONG)t->buffer.pbpa_PixelData, VCA_OutputFormat, VCF_RGB24, VCA_OutputWidth, t->width,
		                          VCA_OutputHeight, t->height, VCA_OutputPitch, t->buffer.pbpa_PixelArrayMod, TAG_DONE);

	/* rows of whole 32 pixels and an even height; the picture keeps its size in the top left corner (VCA_DestRect), unscaled */
	ULONG pitch = ALIGN_UP(t->width, ROW_PIXELS) * RGB_BYTES, rows = ALIGN_UP(t->height, 2u);
	UBYTE *pix = AllocVec(pitch * rows, MEMF_PUBLIC | MEMF_FAST);
	if (pix == NULL)
		return VCERR_NOMEM;
	struct VCRect rect = {0, 0, (LONG)t->width, (LONG)t->height};
	LONG error = VC_DecodeImageTags(VCA_Data, (ULONG)src->data, VCA_Length, src->length, VCA_Codec, VCC_JPEG, VCA_Dest, (ULONG)pix,
	                                VCA_OutputFormat, VCF_RGB24, VCA_OutputWidth, t->width, VCA_OutputHeight, rows, VCA_OutputPitch,
	                                pitch, VCA_DestRect, (ULONG)&rect, TAG_DONE);
	if (error == 0 && target_write(cl, o, t, pix, pitch) != 0)
		error = VCERR_BADSTATE;
	FreeVec(pix);
	return error;
}

/**
 * vpu_decode() - The picture through the VPU, videocodec.library taken for it and given back.
 * @cl:  the class
 * @o:   the object
 * @src: the file's bytes
 * @t:   the picture, sized
 *
 * Return: 0, or the VCERR_* (the picture then goes to software).
 */
static LONG vpu_decode(Class *cl, Object *o, const struct source *src, const struct target *t)
{
	struct VCJpegBase *base = (struct VCJpegBase *)cl->cl_UserData;
	struct Library *VideoCodecBase = codec_take(base);
	if (VideoCodecBase == NULL)
		return VCERR_NOBACKEND;
	LONG error = vpu_decode_with(cl, o, src, t, VideoCodecBase);
	codec_give(base);
	return error;
}

/**
 * software_decode() - The picture through the system's JPEG class: a second object on the same file, read back.
 * @cl:    the class
 * @o:     the object
 * @src:   the file's bytes
 * @t:     the picture
 * @sized: whether @t is set up (our header reader read the size); if not, it
 *         is set up here from the size the other class read
 *
 * While the second object is made, shared.Fallback names this task: the
 * descriptor's recognition function, which runs in the task that makes the
 * object, then leaves the file to the next descriptor.  fallback_lock keeps
 * one hand-over at a time.  The second object is asked for V43 mode and no
 * remapping, so its pixels can be read back as 24-bit RGB.
 *
 * Return: 0, or an IoErr() code.
 */
static LONG software_decode(Class *cl, Object *o, const struct source *src, struct target *t, BOOL sized)
{
	struct VCJpegBase *base = (struct VCJpegBase *)cl->cl_UserData;
	struct ExecBase *SysBase = base->SysBase;
	struct Library *DOSBase = base->DOSBase;
	struct Library *DataTypesBase = base->DataTypesBase;

	UBYTE *name = AllocVec(NAME_MAX, MEMF_PUBLIC);
	if (name == NULL)
		return ERROR_NO_FREE_STORE;
	BOOL named = NameFromFH(src->fh, name, NAME_MAX) != 0;

	ObtainSemaphore(&base->fallback_lock);
	base->shared.Fallback = FindTask(NULL);
	Object *sw = named ? NewDTObject(name, DTA_GroupID, GID_PICTURE, PDTA_DestMode, PMODE_V43, PDTA_Remap, FALSE, TAG_DONE)
	                   : NewDTObject(NULL, DTA_SourceType, DTST_MEMORY, DTA_SourceAddress, (ULONG)src->data, DTA_SourceSize,
	                                 src->length, DTA_GroupID, GID_PICTURE, PDTA_DestMode, PMODE_V43, PDTA_Remap, FALSE, TAG_DONE);
	LONG error = sw == NULL ? IoErr() : 0;
	base->shared.Fallback = NULL;
	ReleaseSemaphore(&base->fallback_lock);
	FreeVec(name);
	if (sw == NULL)
		return error != 0 ? error : DTERROR_INVALID_DATA;

	struct BitMapHeader *bmh = NULL;
	GetDTAttrs(sw, PDTA_BitMapHeader, (ULONG)&bmh, TAG_DONE);
	if (bmh == NULL)
		error = DTERROR_INVALID_DATA;
	else if (!sized)
		error = target_set(cl, o, bmh->bmh_Width, bmh->bmh_Height, t);
	else if (bmh->bmh_Width != t->width || bmh->bmh_Height != t->height)
		error = DTERROR_INVALID_DATA; /* the two classes read another size from the same header */
	if (error == 0 && t->buffer.pbpa_PixelData != NULL)
	{
		struct pdtBlitPixelArray rpa = {PDTM_READPIXELARRAY, t->buffer.pbpa_PixelData, PBPAFMT_RGB, t->buffer.pbpa_PixelArrayMod, 0, 0,
		                                t->width, t->height};
		if (object_method(base, sw, (Msg)&rpa) == 0)
			error = DTERROR_INVALID_DATA;
	}
	else if (error == 0)
	{
		ULONG pitch = t->width * RGB_BYTES;
		UBYTE *pix = AllocVec(pitch * t->height, MEMF_PUBLIC);
		struct pdtBlitPixelArray rpa = {PDTM_READPIXELARRAY, pix, PBPAFMT_RGB, pitch, 0, 0, t->width, t->height};
		if (pix == NULL)
			error = ERROR_NO_FREE_STORE;
		else if (object_method(base, sw, (Msg)&rpa) == 0)
			error = DTERROR_INVALID_DATA;
		else
			error = target_write(cl, o, t, pix, pitch);
		if (pix != NULL)
			FreeVec(pix);
	}
	DisposeDTObject(sw);
	return error;
}

/**
 * vpu_lost() - Whether an image call's error means the VPU is gone, for the pictures after this one.
 * @error: the VCERR_*
 *
 * shared.VPUDown then sends every JPEG to software from the recognition
 * function on, until the idle task closes videocodec.library (5 to 10 s
 * later, no picture reaching the VPU): the next picture opens it again and
 * tries the VPU.  Without the library at all, no idle task runs, and the
 * VPU stays down.
 *
 * Return: TRUE for no backend, a timeout or a failed link.
 */
static BOOL vpu_lost(LONG error)
{
	return error == VCERR_NOBACKEND || error == VCERR_TIMEOUT || error == VCERR_TRANSPORT;
}

/**
 * picture_load() - Fill a new object from its source: the VPU first, the system's class if that fails.
 * @cl: the class
 * @o:  the object, just made by picture.datatype
 *
 * Return: 0, or an IoErr() code.
 */
static LONG picture_load(Class *cl, Object *o)
{
	struct VCJpegBase *base = (struct VCJpegBase *)cl->cl_UserData;
	struct ExecBase *SysBase = base->SysBase;
	struct Library *DOSBase = base->DOSBase;
	struct Library *DataTypesBase = base->DataTypesBase;

	ULONG type = DTST_RAM;
	BPTR fh = 0;
	STRPTR name = NULL;
	GetDTAttrs(o, DTA_SourceType, (ULONG)&type, DTA_Handle, (ULONG)&fh, DTA_Name, (ULONG)&name, TAG_DONE);
	if (type == DTST_RAM)
		return 0; /* an empty object, to be filled by its owner */
	if (type != DTST_FILE || fh == 0)
		return ERROR_OBJECT_WRONG_TYPE;

	struct source src = {fh, NULL, 0};
	LONG error = source_read(base, fh, &src);
	if (error != 0)
		return error;

	/*
	 * Sized from our header reader, both ways fill the one buffer.  A header
	 * it cannot read goes to software unsized: sized there as the system's
	 * class reads it.
	 */
	struct image_facts facts;
	struct target t;
	LONG vc = image_info(src.data, src.length, VCC_JPEG, &facts);
	BOOL sized = vc == 0;
	if (sized)
		error = target_set(cl, o, facts.width, facts.height, &t);
	if (sized && error == 0)
		vc = base->shared.VPUDown != 0 ? VCERR_NOBACKEND : vpu_decode(cl, o, &src, &t);
	if (error == 0 && vc != 0)
	{
		Kprintf("[vcjpeg] %s: the VPU said %ld, to software\n", name != NULL ? name : (STRPTR) "?", vc);
		if (vpu_lost(vc))
			base->shared.VPUDown = 1;
		error = software_decode(cl, o, &src, &t, sized); /* out of VPU memory too: tried again next time, it may have room then */
	}
	FreeVec(src.data);
	if (error == 0 && name != NULL)
		SetDTAttrs(o, NULL, NULL, DTA_ObjName, (ULONG)FilePart(name), TAG_DONE);
	return error;
}

/**
 * dispatch() - The class's dispatcher: OM_NEW loads the picture, everything else is picture.datatype's.
 * @cl:  the class
 * @o:   the object (for OM_NEW, the class being made)
 * @msg: the message
 *
 * Return: what the method returns; for OM_NEW the object, or NULL with IoErr() set.
 */
static ULONG dispatch(Class *cl asm("a0"), Object *o asm("a2"), Msg msg asm("a1"))
{
	if (msg->MethodID != OM_NEW)
		return super_method(cl, o, msg);

	struct VCJpegBase *base = (struct VCJpegBase *)cl->cl_UserData;
	struct Library *DOSBase = base->DOSBase;
	Object *obj = (Object *)super_method(cl, o, msg);
	if (obj == NULL)
		return 0;
	LONG error = picture_load(cl, obj);
	if (error != 0)
	{
		ULONG dispose = OM_DISPOSE;
		super_method(cl, obj, (Msg)&dispose);
		SetIoErr(error);
		return 0;
	}
	return (ULONG)obj;
}

/**
 * vcjpeg_make_class() - Make the class (a subclass of picture.datatype, public under the library's name).
 * @base: the library base, its libraries open
 *
 * Return: the class, or NULL.
 */
Class *vcjpeg_make_class(struct VCJpegBase *base)
{
	struct Library *IntuitionBase = base->IntuitionBase;
	Class *cl = MakeClass((ClassID)base->lib.lib_Node.ln_Name, (ClassID) "picture.datatype", NULL, 0, 0);
	if (cl == NULL)
		return NULL;
	cl->cl_Dispatcher.h_Entry = (HOOKFUNC)(APTR)dispatch; /* called with the class in A0, the object in A2, the message in A1 */
	cl->cl_UserData = (ULONG)base;
	AddClass(cl);
	base->cl = cl;
	return cl;
}

/**
 * vcjpeg_free_class() - Remove and free the class, unless objects or subclasses of it are alive.
 * @base: the library base
 *
 * Return: TRUE once the class is gone.
 */
BOOL vcjpeg_free_class(struct VCJpegBase *base)
{
	struct Library *IntuitionBase = base->IntuitionBase;
	Class *cl = base->cl;
	if (cl == NULL)
		return TRUE;
	if (cl->cl_ObjectCount != 0 || cl->cl_SubclassCount != 0)
		return FALSE;
	RemoveClass(cl);
	FreeClass(cl);
	base->cl = NULL;
	return TRUE;
}
