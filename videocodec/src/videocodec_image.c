/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * videocodec_image.c - the image calls: VC_DecodeImage(), VC_EncodeImage()
 * and VC_ConvertImage(), one picture each, blocking, in the caller's task;
 * and VC_GetImageInfo(), which reads a still picture's header.
 *
 * An image call is its tags parsed into a struct VCImageParams, no
 * session's parameters, and one VCB_Image() at the backend whose codec row
 * serves image calls (VCCF_Image): how the picture is made, and how long
 * that may take, is the backend's business.  A decode's codec is told from the picture's first
 * bytes when the caller names none.
 */
#include <memory.h>

#include <videocodec_private.h>
#include <image_info.h>

/**
 * image_rect() - A rectangle tag into one side of an image call.
 * @side:  the side
 * @value: the tag's value, a struct VCRect pointer; NULL clears it
 */
static void image_rect(struct VCImageSide *side, ULONG value)
{
	if (value != 0)
		side->Rect = *(const struct VCRect *)value;
	side->HaveRect = value != 0;
}

/**
 * image_tag() - Store one of an image call's tags in its parameters.
 * @params: the call's parameters
 * @tag:    the tag
 * @value:  its value
 *
 * VCA_Data and the input picture's tags describe the source; VCA_Dest and the
 * output picture's tags the destination.  VCA_Backend and VCA_ErrorPtr are
 * the frontend's (image_call()); a session's tags mean nothing here.  Both
 * are ignored, as tag lists allow.
 */
static void image_tag(struct VCImageParams *params, Tag tag, ULONG value)
{
	struct VCImageSide *source = &params->Source, *dest = &params->Dest;
	switch (tag)
	{
	case VCA_Codec:
		params->Codec = value;
		break;
	case VCA_Data:
		source->Data = (APTR)value;
		break;
	case VCA_Length:
		source->Length = value;
		break;
	case VCA_InputFormat:
		source->Format = value;
		break;
	case VCA_Width:
		source->Width = value;
		break;
	case VCA_Height:
		source->Height = value;
		break;
	case VCA_InputPitch:
		source->Pitch = value;
		break;
	case VCA_SourceRect:
		image_rect(source, value);
		break;
	case VCA_Dest:
		dest->Data = (APTR)value;
		break;
	case VCA_DestSize:
		dest->Length = value;
		break;
	case VCA_OutputFormat:
		dest->Format = value;
		break;
	case VCA_OutputWidth:
		dest->Width = value;
		break;
	case VCA_OutputHeight:
		dest->Height = value;
		break;
	case VCA_OutputPitch:
		dest->Pitch = value;
		break;
	case VCA_DestRect:
		image_rect(dest, value);
		break;
	case VCA_KeepAspect:
		params->KeepAspect = value != 0;
		break;
	case VCA_Quality:
		params->Quality = value;
		break;
	case VCA_Timeout:
		params->TimeoutMs = value;
		break;
	default:
		break;
	}
}

/**
 * image_parse() - An image call's tags into its parameters.
 * @base:   the library base (utility.library walks the list)
 * @tags:   the caller's tag list
 * @kind:   VCK_* of the call
 * @codec:  the codec when VCA_Codec is not given
 * @params: the parameters, filled whole
 */
static void image_parse(struct VideoCodecBase *base, struct TagItem *tags, ULONG kind, ULONG codec, struct VCImageParams *params)
{
	memset(params, 0, sizeof(*params));
	params->Size = sizeof(*params);
	params->Kind = kind;
	params->Codec = codec;
	struct Library *UtilityBase = base->UtilityBase;
	struct TagItem *tag;
	while ((tag = NextTagItem(&tags)) != NULL) /* follows TAG_MORE, skips TAG_IGNORE and TAG_SKIP */
		image_tag(params, tag->ti_Tag, tag->ti_Data);
}

/**
 * image_check() - Whether an image call has what its kind needs; a decode's codec told from its signature.
 * @params: the call's parameters, parsed
 *
 * Always a source and a destination; a length for compressed input (a
 * decoder), room for compressed output (an encoder).  A decode without
 * VCA_Codec needs its codec here, to choose the backend; the header behind
 * the signature is the backend's to read and refuse.
 *
 * Return: 0, VCERR_BADPARAM for something missing, or VCERR_STREAM for a
 * decode whose bytes start with no signature this knows.
 */
static LONG image_check(struct VCImageParams *params)
{
	const struct VCImageSide *source = &params->Source, *dest = &params->Dest;
	BOOL bytes_in = params->Kind == VCK_Decoder, bytes_out = params->Kind == VCK_Encoder;
	if (source->Data == NULL || dest->Data == NULL || (bytes_in && source->Length == 0) || (bytes_out && dest->Length == 0))
		return VCERR_BADPARAM;
	if (bytes_in && params->Codec == VCC_None)
	{
		params->Codec = image_codec(source->Data, source->Length); /* the backend is chosen by codec; it reads the header */
		if (params->Codec == VCC_None)
			return VCERR_STREAM; /* no signature this knows (a TGA has none: it needs VCA_Codec) */
	}
	return 0;
}

/**
 * image_run() - An image call at the backend whose row serves it.
 * @base:   the library base
 * @params: the call's parameters, checked; Written comes back filled
 * @name:   the backend the program asked for (VCA_Backend), or NULL
 *
 * Return: 0, VCERR_NOBACKEND if no backend serves the codec for the kind in
 * image calls, or the backend's error.
 */
static LONG image_run(struct VideoCodecBase *base, struct VCImageParams *params, const char *name)
{
	struct ExecBase *SysBase = base->SysBase;
	ObtainSemaphore(&base->lock);
	vc_backends_load(base);
	/* a compressed side has no format: a decoder's source, an encoder's destination */
	struct vc_choice want = {
	    .codec = params->Codec,
	    .kind = params->Kind,
	    .use = VCCF_Image,
	    .reads = vc_format_mask(params->Source.Format),
	    .writes = vc_format_mask(params->Dest.Format),
	};
	struct vc_backend *backend = vc_backend_choose(base, &want, name, NULL);
	ReleaseSemaphore(&base->lock);
	if (backend == NULL)
		return VCERR_NOBACKEND;
	struct Library *VCBackendBase = backend->library; /* the VCB_* call goes through it */
	return VCB_Image(params);
}

/**
 * image_call() - One image call: its tags parsed and checked, run at its backend, the error also at VCA_ErrorPtr.
 * @base:   the library base
 * @tags:   the caller's tag list
 * @kind:   VCK_* of the call
 * @codec:  the codec when VCA_Codec is not given; VCC_None for a decode (told from the bytes)
 * @params: where the call's parameters go; an encoder's Written comes back in them
 *
 * Return: 0 or a VCERR_*.
 */
static LONG image_call(struct VideoCodecBase *base, struct TagItem *tags, ULONG kind, ULONG codec, struct VCImageParams *params)
{
	struct Library *UtilityBase = base->UtilityBase;
	image_parse(base, tags, kind, codec, params);
	LONG err = image_check(params);
	if (err == 0)
		err = image_run(base, params, (const char *)GetTagData(VCA_Backend, 0, tags));
	LONG *errp = (LONG *)GetTagData(VCA_ErrorPtr, 0, tags);
	if (errp != NULL)
		*errp = err;
	return err;
}

/* ---- the library's functions ------------------------------------------------------------- */

/**
 * VC_DecodeImageTagList() - Decode one still picture into the caller's picture (LVO).
 * @tags: VCA_Data and VCA_Length (the picture's bytes), VCA_Dest (the
 *        picture); its geometry from VCA_OutputWidth/Height/Pitch/Format,
 *        VCA_DestRect, VCA_KeepAspect (the picture's own size when none is
 *        given); VCA_Codec [told from the bytes; VCERR_STREAM if they name
 *        no format], VCA_Timeout, VCA_Backend, VCA_ErrorPtr
 * @base: the library base (A6)
 *
 * Return: 0, or a VCERR_* (also at VCA_ErrorPtr).
 */
LONG VC_DecodeImageTagList(struct TagItem *tags asm("a0"), struct VideoCodecBase *base asm("a6"))
{
	struct VCImageParams params;
	return image_call(base, tags, VCK_Decoder, VCC_None, &params);
}

/**
 * VC_EncodeImageTagList() - Encode one picture into the caller's buffer (LVO).
 * @tags: VCA_Data (the picture) with VCA_Width/Height/InputFormat/
 *        InputPitch, VCA_Dest and VCA_DestSize (room for the compressed
 *        bytes); VCA_Codec [VCC_JPEG], VCA_Quality, VCA_Timeout,
 *        VCA_Backend, VCA_ErrorPtr
 * @base: the library base (A6)
 *
 * Return: the bytes written at VCA_Dest, or a VCERR_* (also at VCA_ErrorPtr).
 */
LONG VC_EncodeImageTagList(struct TagItem *tags asm("a0"), struct VideoCodecBase *base asm("a6"))
{
	struct VCImageParams params;
	LONG err = image_call(base, tags, VCK_Encoder, VCC_JPEG, &params);
	return err == 0 ? (LONG)params.Written : err;
}

/**
 * VC_ConvertImageTagList() - Convert one picture into the caller's picture (LVO).
 * @tags: VCA_Data (the source) with VCA_Width/Height/InputFormat/InputPitch
 *        and VCA_SourceRect; VCA_Dest (the destination) with
 *        VCA_OutputWidth/Height/Pitch/Format and VCA_DestRect or
 *        VCA_KeepAspect; VCA_Timeout, VCA_Backend, VCA_ErrorPtr
 * @base: the library base (A6)
 *
 * Return: 0, or a VCERR_* (also at VCA_ErrorPtr).
 */
LONG VC_ConvertImageTagList(struct TagItem *tags asm("a0"), struct VideoCodecBase *base asm("a6"))
{
	struct VCImageParams params;
	return image_call(base, tags, VCK_Converter, VCC_Raw, &params);
}

/**
 * VC_GetImageInfo() - What a still picture's header says (LVO).
 * @data:   the picture's bytes (all of them, for a GIF's frame count)
 * @length: their count
 * @info:   the caller's structure, Size and Codec set (VCC_None: told from the bytes)
 * @base:   the library base
 *
 * No backend takes part: the header is read here.
 *
 * Return: 0 with @info filled as far as its Size reaches, VCERR_STREAM for
 * bytes that are no picture this knows or too few to give its size, or
 * VCERR_BADPARAM.
 */
LONG VC_GetImageInfo(APTR data asm("a0"), ULONG length asm("d0"), struct VCImageInfo *info asm("a1"),
                     struct VideoCodecBase *base asm("a6"))
{
	(void)base;
	if (data == NULL || info == NULL || !VC_HAS_FIELD(info->Size, struct VCImageInfo, Codec))
		return VCERR_BADPARAM;
	struct image_facts facts;
	LONG rc = image_info(data, length, info->Codec, &facts);
	if (rc < 0 || facts.width == 0 || facts.height == 0)
		return VCERR_STREAM;
	struct VCImageInfo told = {.Size = sizeof(told), .Codec = facts.codec, .Width = facts.width, .Height = facts.height,
	                           .Transparency = facts.transparency, .Frames = facts.frames};
	ULONG size = info->Size < sizeof(told) ? info->Size : sizeof(told);
	memcpy(info, &told, size);
	info->Size = size;
	return 0;
}
