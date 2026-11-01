/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * image.c - the image calls, one picture each: IMAGE decodes a still (its
 * header first, by VC_GetImageInfo()) with VC_DecodeImage() into a picture
 * (RGB24 as a PPM, else raw), scaled when a size is given; TOJPEG encodes a
 * raw picture with VC_EncodeImage(); CONVERT scales and converts one with
 * VC_ConvertImage().
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/dos_protos.h>
#include <clib/videocodec_protos.h>
#else
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/videocodec.h>
#endif

#include <dos/dos.h>
#include <exec/memory.h>

#include <args.h>
#include <bits.h>
#include <kv.h>
#include <strutil.h>
#include <timing.h>
#include <vc_names.h>

#include "commands.h"
#include "picture.h"

const char image_template[] = "FILE/A,OUT/A,OUTWIDTH/N,OUTHEIGHT/N,OUTFORMAT/K,ENCODING/K,BACKEND/K,MISALIGN/S";
const char tojpeg_template[] = "FILE/A,WIDTH/N/A,HEIGHT/N/A,OUT/A,FORMAT/K,QUALITY/N,BACKEND/K,MISALIGN/S";
const char convert_template[] = "FILE/A,WIDTH/N/A,HEIGHT/N/A,OUT/A,FORMAT/K,OUTWIDTH/N,OUTHEIGHT/N,OUTFORMAT/K,BACKEND/K,MISALIGN/S";

enum
{
	IMG_FILE,
	IMG_OUT,
	IMG_OUTWIDTH,
	IMG_OUTHEIGHT,
	IMG_OUTFORMAT,
	IMG_ENCODING,
	IMG_BACKEND,
	IMG_MISALIGN
};

enum
{
	JPG_FILE,
	JPG_WIDTH,
	JPG_HEIGHT,
	JPG_OUT,
	JPG_FORMAT,
	JPG_QUALITY,
	JPG_BACKEND,
	JPG_MISALIGN
};

enum
{
	CNV_FILE,
	CNV_WIDTH,
	CNV_HEIGHT,
	CNV_OUT,
	CNV_FORMAT,
	CNV_OUTWIDTH,
	CNV_OUTHEIGHT,
	CNV_OUTFORMAT,
	CNV_BACKEND,
	CNV_MISALIGN
};

/**
 * image_codec_named() - The codec an ENCODING name gives.
 * @name: the name, any case
 *
 * Return: VCC_*, VCC_None for none.
 */
static ULONG image_codec_named(const char *name)
{
	static const struct
	{
		const char *name;
		ULONG codec;
	} names[] = {{"JPEG", VCC_JPEG}, {"PNG", VCC_PNG}, {"GIF", VCC_GIF}, {"BMP", VCC_BMP}, {"TGA", VCC_TGA}, {"PPM", VCC_PPM}};
	for (ULONG i = 0; i < ARRAY_SIZE(names); i++)
		if (_Stricmp((CONST_STRPTR)name, (CONST_STRPTR)names[i].name) == 0)
			return names[i].codec;
	return VCC_None;
}

/**
 * image_header() - What VC_GetImageInfo() says of a picture (header.*).
 * @data:  the file
 * @len:   its bytes
 * @codec: ENCODING's, or VCC_None to tell from the data
 * @info:  where the header goes
 *
 * Return: TRUE with @info filled; FALSE with header.rc set.
 */
static BOOL image_header(const UBYTE *data, ULONG len, ULONG codec, struct VCImageInfo *info)
{
	info->Size = sizeof(*info);
	info->Codec = codec;
	LONG r = VC_GetImageInfo((APTR)data, len, info);
	kv_num("header.rc", r);
	if (r != 0)
		return FALSE;
	static const char *const transparencies[] = {"none", "mask", "alpha"};
	kv_str("header.codec", codec_name(info->Codec));
	kv_unum("header.width", info->Width);
	kv_unum("header.height", info->Height);
	kv_str("header.transparency", info->Transparency < ARRAY_SIZE(transparencies) ? transparencies[info->Transparency] : "?");
	kv_unum("header.frames", info->Frames);
	return TRUE;
}

/**
 * do_image() - IMAGE: one still through VC_DecodeImage(), straight or through the converter as the library sees fit.
 * @t:    the tool
 * @args: image_template's
 *
 * Return: RETURN_OK; RETURN_FAIL for a bad name or a refused decode (image.rc); RETURN_ERROR for a missing file or memory.
 */
int do_image(struct tool *t, LONG *args)
{
	const char *file = arg_str(args, IMG_FILE), *outname = arg_str(args, IMG_OUT);
	const char *codec_arg = arg_str(args, IMG_ENCODING), *outfmt_name = arg_str(args, IMG_OUTFORMAT);
	t->backend = arg_str(args, IMG_BACKEND);
	t->misalign = args[IMG_MISALIGN] != 0;
	ULONG codec = codec_arg != NULL ? image_codec_named(codec_arg) : VCC_None;
	ULONG fmt = outfmt_name != NULL ? format_of_name(outfmt_name) : VCF_RGB24;
	if ((codec_arg != NULL && codec == VCC_None) || fmt == 0)
	{
		kv_str("fail", "ENCODING: JPEG, PNG, GIF, BMP, TGA or PPM; OUTFORMAT: I420, RGB16, RGB24, BGR24, BGRA32 or RGBA32");
		return RETURN_FAIL;
	}
	ULONG len;
	UBYTE *data = load_file(file, &len);
	if (data == NULL)
	{
		kv_str("fail", "the picture did not load");
		return RETURN_ERROR;
	}
	kv_str("image.file", file);
	kv_unum("image.bytes", len);
	int rc = RETURN_FAIL;
	struct VCImageInfo info;
	if (!image_header(data, len, codec, &info))
	{
		kv_str("fail", "VC_GetImageInfo refused the header");
		goto out;
	}
	ULONG ow = arg_num(args, IMG_OUTWIDTH, info.Width), oh = arg_num(args, IMG_OUTHEIGHT, info.Height);
	struct VCFrame f;
	ULONG size;
	APTR dest = picture_alloc(&f, fmt, ow, oh, t->misalign, &size);
	if (dest == NULL)
	{
		kv_str("fail", "no memory for the picture");
		rc = RETURN_ERROR;
		goto out;
	}
	kv_str("image.out.format", format_name(fmt));
	kv_unum("image.out.width", ow);
	kv_unum("image.out.height", oh);
	kv_unum("image.out.pitch", f.Pitch[0]);
	LONG err = 0;
	u32 t0 = get_time();
	LONG r = VC_DecodeImageTags(codec != VCC_None ? VCA_Codec : TAG_IGNORE, codec, VCA_Data, (ULONG)data, VCA_Length, len, VCA_Dest,
	                            (ULONG)f.Data, VCA_OutputFormat, fmt, VCA_OutputWidth, ow, VCA_OutputHeight, oh, VCA_OutputPitch, f.Pitch[0],
	                            BACKEND_TAG(t), VCA_ErrorPtr, (ULONG)&err, TAG_DONE);
	u32 took = get_time() - t0;
	kv_num("image.rc", r);
	kv_unum("image.us", took);
	if (r != 0)
	{
		kv_num("error", r);
		kv_str("fail", "VC_DecodeImage refused or failed");
	}
	else if (!frame_to_file(outname, &f))
	{
		kv_str("fail", "the picture could not be written");
		rc = RETURN_ERROR;
	}
	else
	{
		kv_str("image.written", outname);
		rc = RETURN_OK;
	}
	FreeVec(dest);
out:
	FreeVec(data);
	return rc;
}

/**
 * do_tojpeg() - TOJPEG: one raw picture encoded by VC_EncodeImage().
 * @t:    the tool
 * @args: tojpeg_template's
 *
 * Return: RETURN_OK; RETURN_FAIL for a bad format or a refused encode; RETURN_ERROR for a missing file or memory.
 */
int do_tojpeg(struct tool *t, LONG *args)
{
	const char *raw = arg_str(args, JPG_FILE), *outname = arg_str(args, JPG_OUT), *fmt_name = arg_str(args, JPG_FORMAT);
	ULONG w = arg_num(args, JPG_WIDTH, 0), h = arg_num(args, JPG_HEIGHT, 0), quality = arg_num(args, JPG_QUALITY, 80u);
	t->backend = arg_str(args, JPG_BACKEND);
	t->misalign = args[JPG_MISALIGN] != 0;
	ULONG fmt = fmt_name != NULL ? format_of_name(fmt_name) : VCF_I420;
	if (fmt == 0 || w == 0 || h == 0)
	{
		kv_str("fail", "TOJPEG needs WIDTH, HEIGHT and a FORMAT of I420 or RGBA32");
		return RETURN_FAIL;
	}
	BPTR file = Open((CONST_STRPTR)raw, MODE_OLDFILE);
	if (file == 0)
	{
		PrintFault(IoErr(), (CONST_STRPTR)raw);
		kv_str("fail", "the raw picture did not open");
		return RETURN_ERROR;
	}
	kv_str("encode_image.file", raw);
	kv_unum("encode_image.width", w);
	kv_unum("encode_image.height", h);
	kv_str("encode_image.format", format_name(fmt));
	kv_unum("encode_image.quality", quality);
	int rc = RETURN_ERROR;
	struct VCFrame f;
	ULONG size;
	APTR src = picture_alloc(&f, fmt, w, h, t->misalign, &size);
	UBYTE *tmp = AllocVec(w * h * 4u, MEMF_PUBLIC);
	ULONG dsize = w * h + 65536u;
	UBYTE *dest = AllocVec(dsize, MEMF_PUBLIC | MEMF_FAST);
	if (src == NULL || tmp == NULL || dest == NULL)
	{
		kv_str("fail", "no memory");
		goto out;
	}
	if (!raw_into_frame(file, tmp, &f, w, h))
	{
		kv_str("fail", "no picture of that size in the file");
		goto out;
	}
	LONG err = 0;
	u32 t0 = get_time();
	LONG r = VC_EncodeImageTags(VCA_Data, (ULONG)f.Data, VCA_Width, w, VCA_Height, h, VCA_InputFormat, fmt, VCA_InputPitch, f.Pitch[0],
	                            VCA_Dest, (ULONG)dest, VCA_DestSize, dsize, VCA_Quality, quality, BACKEND_TAG(t), VCA_ErrorPtr, (ULONG)&err,
	                            TAG_DONE);
	u32 took = get_time() - t0;
	kv_num("encode_image.rc", r);
	kv_unum("encode_image.us", took);
	if (r <= 0)
	{
		kv_num("error", r);
		kv_str("fail", "VC_EncodeImage refused or failed");
		rc = RETURN_FAIL;
		goto out;
	}
	kv_num("encode_image.bytes", r);
	BPTR out = Open((CONST_STRPTR)outname, MODE_NEWFILE);
	if (out == 0)
	{
		PrintFault(IoErr(), (CONST_STRPTR)outname);
		kv_str("fail", "the JPEG could not be written");
		goto out;
	}
	if (Write(out, dest, r) == r)
	{
		kv_str("encode_image.written", outname);
		rc = RETURN_OK;
	}
	else
		kv_str("fail", "the JPEG could not be written");
	Close(out);
out:
	if (dest != NULL)
		FreeVec(dest);
	if (tmp != NULL)
		FreeVec(tmp);
	if (src != NULL)
		FreeVec(src);
	Close(file);
	return rc;
}

/**
 * do_convert() - CONVERT: one raw picture scaled and converted by VC_ConvertImage().
 * @t:    the tool
 * @args: convert_template's
 *
 * Return: RETURN_OK; RETURN_FAIL for a bad format or a refused conversion; RETURN_ERROR for a missing file or memory.
 */
int do_convert(struct tool *t, LONG *args)
{
	const char *raw = arg_str(args, CNV_FILE), *outname = arg_str(args, CNV_OUT);
	const char *fmt_name = arg_str(args, CNV_FORMAT), *ofmt_name = arg_str(args, CNV_OUTFORMAT);
	ULONG w = arg_num(args, CNV_WIDTH, 0), h = arg_num(args, CNV_HEIGHT, 0);
	t->backend = arg_str(args, CNV_BACKEND);
	t->misalign = args[CNV_MISALIGN] != 0;
	ULONG fmt = fmt_name != NULL ? format_of_name(fmt_name) : VCF_I420;
	ULONG ofmt = ofmt_name != NULL ? format_of_name(ofmt_name) : VCF_RGB24;
	if (fmt == 0 || ofmt == 0 || w == 0 || h == 0)
	{
		kv_str("fail", "CONVERT needs WIDTH, HEIGHT, a FORMAT and an OUTFORMAT");
		return RETURN_FAIL;
	}
	ULONG ow = arg_num(args, CNV_OUTWIDTH, w) & ~1u, oh = arg_num(args, CNV_OUTHEIGHT, h) & ~1u;
	BPTR file = Open((CONST_STRPTR)raw, MODE_OLDFILE);
	if (file == 0)
	{
		PrintFault(IoErr(), (CONST_STRPTR)raw);
		kv_str("fail", "the raw picture did not open");
		return RETURN_ERROR;
	}
	kv_str("convert.file", raw);
	kv_str("convert.in.format", format_name(fmt));
	kv_unum("convert.in.width", w);
	kv_unum("convert.in.height", h);
	int rc = RETURN_ERROR;
	struct VCFrame in, out;
	ULONG isize, osize;
	APTR src = picture_alloc(&in, fmt, w, h, t->misalign, &isize);
	APTR dest = picture_alloc(&out, ofmt, ow, oh, t->misalign, &osize);
	UBYTE *tmp = AllocVec(w * h * 4u, MEMF_PUBLIC);
	if (src == NULL || dest == NULL || tmp == NULL)
	{
		kv_str("fail", "no memory");
		goto out;
	}
	if (!raw_into_frame(file, tmp, &in, w, h))
	{
		kv_str("fail", "no picture of that size in the file");
		goto out;
	}
	kv_str("convert.out.format", format_name(ofmt));
	kv_unum("convert.out.width", ow);
	kv_unum("convert.out.height", oh);
	kv_unum("convert.out.pitch", out.Pitch[0]);
	LONG err = 0;
	u32 t0 = get_time();
	LONG r = VC_ConvertImageTags(VCA_Data, (ULONG)in.Data, VCA_Width, w, VCA_Height, h, VCA_InputFormat, fmt, VCA_InputPitch, in.Pitch[0],
	                             VCA_Dest, (ULONG)out.Data, VCA_OutputFormat, ofmt, VCA_OutputWidth, ow, VCA_OutputHeight, oh, VCA_OutputPitch,
	                             out.Pitch[0], BACKEND_TAG(t), VCA_ErrorPtr, (ULONG)&err, TAG_DONE);
	u32 took = get_time() - t0;
	kv_num("convert.rc", r);
	kv_unum("convert.us", took);
	if (r != 0)
	{
		kv_num("error", r);
		kv_str("fail", "VC_ConvertImage refused or failed");
		rc = RETURN_FAIL;
	}
	else if (!frame_to_file(outname, &out))
		kv_str("fail", "the picture could not be written");
	else
	{
		kv_str("convert.written", outname);
		rc = RETURN_OK;
	}
out:
	if (tmp != NULL)
		FreeVec(tmp);
	if (dest != NULL)
		FreeVec(dest);
	if (src != NULL)
		FreeVec(src);
	Close(file);
	return rc;
}
