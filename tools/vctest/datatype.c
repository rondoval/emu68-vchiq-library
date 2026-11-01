/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * datatype.c - DATATYPE: a picture through datatypes.library: the descriptor
 * and class it gets, the time NewDTObject() takes (V43 mode, no screen),
 * its pixels read back as RGB24 by a direct method call (a sum, and a PPM
 * with OUT).  PAGE: many of them at once, a process each, as a browser
 * loads a page.  vcjpeg.datatype's tests.
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/dos_protos.h>
#include <clib/datatypes_protos.h>
#include <clib/videocodec_protos.h>
#else
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/datatypes.h>
#include <proto/videocodec.h>
#endif

#include <dos/dos.h>
#include <dos/dostags.h>
#include <exec/memory.h>
#include <datatypes/datatypes.h>
#include <datatypes/datatypesclass.h>
#include <datatypes/pictureclass.h>
#include <intuition/classes.h>
#include <string.h>

#include <args.h>
#include <bits.h>
#include <kv.h>
#include <timing.h>

#include "commands.h"
#include "picture.h"

#define PAGE_PICTURES_MAX 64
#define PAGE_STACK 32768  /* the tool's own: the library's calls and waits run on it */
#define PAGE_SAMPLE_MS 50 /* how often the codec memory is read while the pictures load */

const char datatype_template[] = "FILE/A,OUT/K";
const char page_template[] = "FILES/A/M,COPIES/K/N";

enum
{
	ARG_FILE,
	ARG_OUT
};

enum
{
	ARG_FILES,
	ARG_COPIES
};

/* One picture of a page: the file its task loads, and what came of it */
struct page_picture
{
	const char *file;
	LONG rc;              /* as datatype.rc */
	u32 started, took;    /* NewDTObject(): when, on the microsecond counter, and for how long */
	ULONG width, height;  /* of the object */
	ULONG sum;            /* of its pixels, when have_sum */
	BOOL have_sum;        /* the object gave its pixels */
	char descriptor[32];  /* its descriptor's name */
};

/* The run, one at a time: its tasks find it here */
static struct
{
	struct page_picture pictures[PAGE_PICTURES_MAX];
	ULONG count;
	ULONG taken;          /* pictures a task has taken, under Forbid() */
	volatile ULONG ended; /* tasks done, under Forbid() */
	struct Task *parent;  /* told each time a task ends */
	ULONG sigmask;
} page;

struct Library *DataTypesBase;

/**
 * object_method() - A method on an object the way a class calls one.
 * @o:   the object
 * @msg: the message
 *
 * Its class's dispatcher (A0 the hook, A2 the object, A1 the message).  Not
 * DoDTMethodA(): it puts a GadgetInfo pointer in the message's second
 * long-word, which for PDTM_READPIXELARRAY is the pixel pointer.
 *
 * Return: the method's result.
 */
static ULONG object_method(Object *o, Msg msg)
{
	struct Hook *h = &OCLASS(o)->cl_Dispatcher;
	return ((ULONG (*)(struct Hook *hook __asm("a0"), Object *obj __asm("a2"), Msg m __asm("a1")))(APTR)h->h_Entry)(h, o, msg);
}

/**
 * picture_new() - A picture as a program without a screen loads one: V43 mode, no remap.
 * @file: the file
 *
 * Return: the object, or NULL with IoErr() set.
 */
static Object *picture_new(const char *file)
{
	return NewDTObject((APTR)file, DTA_GroupID, GID_PICTURE, PDTA_DestMode, PMODE_V43, PDTA_Remap, FALSE, TAG_DONE);
}

/**
 * pixels_read() - A picture's pixels as RGB24 rows.
 * @o:   the object
 * @pix: room for them
 * @w:   its width
 * @h:   its height
 *
 * Return: TRUE if the object gave them.
 */
static BOOL pixels_read(Object *o, UBYTE *pix, ULONG w, ULONG h)
{
	struct pdtBlitPixelArray rpa = {PDTM_READPIXELARRAY, pix, PBPAFMT_RGB, w * 3, 0, 0, w, h};
	return object_method(o, (Msg)&rpa) != 0;
}

/**
 * pixels_sum() - The sum of a picture's RGB24 rows, the one that tells two decodes apart.
 * @pix:   the rows
 * @bytes: their bytes
 *
 * Return: the sum.
 */
static ULONG pixels_sum(const UBYTE *pix, ULONG bytes)
{
	ULONG sum = 0;
	for (ULONG i = 0; i < bytes; i++)
		sum = sum * 31u + pix[i];
	return sum;
}

/**
 * datatype_descriptor() - The descriptor datatypes.library picks for a file.
 * @file: the file
 *
 * Return: TRUE with datatype.descriptor and datatype.class set; FALSE for none.
 */
static BOOL datatype_descriptor(const char *file)
{
	BPTR lock = Lock((CONST_STRPTR)file, ACCESS_READ);
	if (lock == 0)
	{
		kv_str("fail", "cannot lock the file");
		return FALSE;
	}
	struct DataType *dtn = ObtainDataType(DTST_FILE, (APTR)lock, TAG_DONE);
	if (dtn != NULL)
	{
		kv_str("datatype.descriptor", (const char *)dtn->dtn_Header->dth_Name);
		kv_str("datatype.class", (const char *)dtn->dtn_Header->dth_BaseName);
		ReleaseDataType(dtn);
	}
	else
	{
		kv_str("datatype.descriptor", "none");
		kv_num("datatype.descriptor_error", IoErr());
		kv_str("fail", "no descriptor takes the file");
	}
	UnLock(lock);
	return dtn != NULL;
}

/**
 * do_datatype() - DATATYPE: the descriptor, NewDTObject()'s time, the pixels read back.
 * @t:    the tool
 * @args: datatype_template's
 *
 * Return: RETURN_OK; RETURN_ERROR without datatypes.library 44 or memory; RETURN_FAIL when the picture did not load.
 */
int do_datatype(struct tool *t, LONG *args)
{
	(void)t;
	const char *file = arg_str(args, ARG_FILE), *outname = arg_str(args, ARG_OUT);
	DataTypesBase = OpenLibrary((CONST_STRPTR) "datatypes.library", 44);
	if (DataTypesBase == NULL)
	{
		kv_str("fail", "datatypes.library 44 did not open");
		return RETURN_ERROR;
	}
	int rc = RETURN_FAIL;
	kv_str("datatype.file", file);
	if (!datatype_descriptor(file))
		goto out;
	u32 t0 = get_time();
	Object *o = picture_new(file);
	u32 took = get_time() - t0;
	struct BitMapHeader *bmh = NULL;
	if (o != NULL)
		GetDTAttrs(o, PDTA_BitMapHeader, (ULONG)&bmh, TAG_DONE);
	kv_num("datatype.rc", o == NULL ? IoErr() : bmh == NULL ? -1L : 0L);
	kv_unum("datatype.us", took);
	if (o == NULL || bmh == NULL)
	{
		kv_str("fail", "NewDTObject failed");
		goto out_object;
	}
	ULONG w = bmh->bmh_Width, h = bmh->bmh_Height;
	kv_unum("datatype.width", w);
	kv_unum("datatype.height", h);
	kv_unum("datatype.depth", bmh->bmh_Depth);
	UBYTE *pix = AllocVec(w * h * 3, MEMF_PUBLIC);
	if (pix == NULL)
	{
		kv_str("fail", "no memory for the pixels");
		rc = RETURN_ERROR;
		goto out_object;
	}
	if (!pixels_read(o, pix, w, h))
		kv_str("fail", "PDTM_READPIXELARRAY refused");
	else
	{
		kv_hex("datatype.sum", pixels_sum(pix, w * h * 3));
		struct VCFrame f;
		frame_view(&f, pix, VCF_RGB24, w, h, w * 3);
		if (outname == NULL)
			rc = RETURN_OK;
		else if (frame_to_file(outname, &f))
		{
			kv_str("datatype.written", outname);
			rc = RETURN_OK;
		}
		else
		{
			kv_str("fail", "the picture could not be written");
			rc = RETURN_ERROR;
		}
	}
	FreeVec(pix);
out_object:
	if (o != NULL)
		DisposeDTObject(o);
out:
	CloseLibrary(DataTypesBase);
	DataTypesBase = NULL;
	return rc;
}

/**
 * page_task() - One picture of a page, in a process of its own.
 *
 * It takes the next picture and waits on CTRL_F, so that the parent starts
 * them all together, then loads the picture and reads it back.  It ends in
 * Forbid(): the code is the parent's, which must see it gone.
 */
static void page_task(void)
{
	Forbid();
	struct page_picture *p = &page.pictures[page.taken++];
	Permit();
	Wait(SIGBREAKF_CTRL_F);

	p->started = get_time();
	Object *o = picture_new(p->file);
	p->took = get_time() - p->started;
	p->rc = o == NULL ? IoErr() : -1;
	if (o != NULL)
	{
		struct BitMapHeader *bmh = NULL;
		struct DataType *dtn = NULL;
		GetDTAttrs(o, PDTA_BitMapHeader, (ULONG)&bmh, DTA_DataType, (ULONG)&dtn, TAG_DONE);
		if (dtn != NULL)
			strncpy(p->descriptor, (const char *)dtn->dtn_Header->dth_Name, sizeof(p->descriptor) - 1u);
		if (bmh != NULL)
		{
			p->rc = 0;
			p->width = bmh->bmh_Width;
			p->height = bmh->bmh_Height;
			UBYTE *pix = AllocVec(p->width * p->height * 3, MEMF_PUBLIC);
			if (pix != NULL && pixels_read(o, pix, p->width, p->height))
			{
				p->sum = pixels_sum(pix, p->width * p->height * 3);
				p->have_sum = TRUE;
			}
			if (pix != NULL)
				FreeVec(pix);
		}
		DisposeDTObject(o);
	}

	Forbid();
	page.ended++;
	Signal(page.parent, page.sigmask);
}

/**
 * codec_free() - The codec memory free now.
 * @free_bytes: where it goes
 *
 * Return: TRUE if a backend knew: gencmd can fail under load, and the
 * backend's memory check is passed by a failure.
 */
static BOOL codec_free(ULONG *free_bytes)
{
	struct VCInfo vi;
	memset(&vi, 0, sizeof(vi));
	vi.Size = sizeof(vi);
	for (ULONG n = 0; VC_GetBackendInfo(n, &vi) == 0; n++)
		if (vi.MemTotal != 0)
		{
			*free_bytes = vi.MemFree;
			return TRUE;
		}
	return FALSE;
}

/**
 * page_report() - A page's facts.
 * @lowest:  the least codec memory seen free while it loaded
 * @samples: the times the memory was read
 * @missed:  the times it could not be
 *
 * Each picture's start counts from the first NewDTObject() call.
 *
 * Return: TRUE if every picture gave its pixels.
 */
static BOOL page_report(ULONG lowest, ULONG samples, ULONG missed)
{
	u32 first = page.pictures[0].started, us = 0;
	for (ULONG i = 1; i < page.count; i++)
		if ((s32)(page.pictures[i].started - first) < 0)
			first = page.pictures[i].started;
	BOOL whole = TRUE;
	for (ULONG i = 0; i < page.count; i++)
	{
		const struct page_picture *p = &page.pictures[i];
		u32 start_us = p->started - first;
		us = umax(us, start_us + p->took);
		kv_str(kv_at("picture", i, "file"), p->file);
		kv_str(kv_at("picture", i, "descriptor"), p->descriptor[0] != 0 ? p->descriptor : "none");
		kv_num(kv_at("picture", i, "rc"), p->rc);
		kv_unum(kv_at("picture", i, "start_us"), start_us);
		kv_unum(kv_at("picture", i, "us"), p->took);
		kv_unum(kv_at("picture", i, "width"), p->width);
		kv_unum(kv_at("picture", i, "height"), p->height);
		if (p->have_sum)
			kv_hex(kv_at("picture", i, "sum"), p->sum);
		whole = whole && p->have_sum;
	}
	kv_unum("page.pictures", page.count);
	kv_unum("page.us", us);
	kv_unum("page.mem_lowest", lowest);
	kv_unum("page.mem_samples", samples);
	kv_unum("page.mem_missed", missed);
	return whole;
}

/**
 * do_page() - PAGE: every file, COPIES times over, loaded at once, a process each.
 * @t:    the tool
 * @args: page_template's
 *
 * The processes are made first and started together; the codec memory is read
 * meanwhile, which tells how much of the VPU the pictures held.  A picture
 * that goes wrong does not stop the others: the suite reads each one's facts.
 *
 * Return: RETURN_OK; RETURN_ERROR without datatypes.library 44 or a process
 * for every picture; RETURN_FAIL when a picture did not load.
 */
int do_page(struct tool *t, LONG *args)
{
	const char **files = (const char **)args[ARG_FILES];
	ULONG copies = arg_num(args, ARG_COPIES, 1);
	DataTypesBase = OpenLibrary((CONST_STRPTR) "datatypes.library", 44);
	if (DataTypesBase == NULL)
	{
		kv_str("fail", "datatypes.library 44 did not open");
		return RETURN_ERROR;
	}

	memset(&page, 0, sizeof(page));
	page.parent = t->waiter.task;
	page.sigmask = waiter_sigmask(&t->waiter);
	for (ULONG c = 0; c < copies; c++)
		for (ULONG f = 0; files[f] != NULL && page.count < PAGE_PICTURES_MAX; f++)
			page.pictures[page.count++].file = files[f];

	static struct Process *tasks[PAGE_PICTURES_MAX];
	ULONG made = 0;
	while (made < page.count)
	{
		tasks[made] = CreateNewProcTags(NP_Entry, (ULONG)page_task, NP_Name, (ULONG) "VCTest page", NP_StackSize, PAGE_STACK, TAG_DONE);
		if (tasks[made] == NULL)
			break;
		made++;
	}
	BOOL all_made = made == page.count;
	page.count = made;

	for (ULONG i = 0; i < made; i++)
		Signal(&tasks[i]->pr_Task, SIGBREAKF_CTRL_F);

	ULONG lowest = 0, samples = 0, missed = 0, free_bytes;
	while (page.ended < made)
	{
		(void)waiter_wait(&t->waiter, 0, PAGE_SAMPLE_MS);
		if (!codec_free(&free_bytes))
			missed++;
		else
		{
			lowest = samples == 0 ? free_bytes : umin(lowest, free_bytes);
			samples++;
		}
	}

	BOOL whole = page_report(lowest, samples, missed);
	int rc = RETURN_OK;
	if (!all_made)
	{
		kv_str("fail", "no process for every picture");
		rc = RETURN_ERROR;
	}
	else if (!whole)
	{
		kv_str("fail", "a picture did not load");
		rc = RETURN_FAIL;
	}
	CloseLibrary(DataTypesBase);
	DataTypesBase = NULL;
	return rc;
}
