/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * videocodec_session.c - tags to parameters, a session's backend, and the
 * calls that go straight through to it.
 *
 * The frontend parses every tag list (backends never see one) into a
 * struct VCSessionParams, plus the call's other arguments.  A
 * session is a small record naming its backend and the backend's handle;
 * every other call checks the record and forwards to the backend, or to
 * the chain (videocodec_chain.c) when a converter stands behind a decoder.
 */
#include <exec/memory.h>

#include <memory.h>
#include <strutil.h>

#include <videocodec_private.h>

/* ---- tags --------------------------------------------------------------------------- */

/**
 * set_rect() - Copy a rectangle tag into the parameters.
 * @rect:  the parameters' rectangle
 * @have:  the parameters' flag saying the rectangle was given
 * @value: the tag's value, a struct VCRect pointer; NULL clears it
 */
static void set_rect(struct VCRect *rect, ULONG *have, ULONG value)
{
	if (value != 0)
		*rect = *(const struct VCRect *)value;
	*have = value != 0;
}

/**
 * session_tag() - Store one session tag in the parameters.
 * @params: the parameters
 * @tag:    the tag
 * @value:  its value
 *
 * Tags a running session can take (the output geometry, the bit rate, a
 * key-frame request) also set their VCSPF_* bit in @params->Changed.  Any
 * other tag is ignored, as tag lists allow: VCA_Backend and VCA_ErrorPtr are
 * the open's own, the codec configuration session_parse()'s.
 */
static void session_tag(struct VCSessionParams *params, Tag tag, ULONG value)
{
	switch (tag)
	{
	/* every session */
	case VCA_Codec:
		params->Codec = value;
		break;
	case VCA_Kind:
		params->Kind = value;
		break;
	case VCA_CodecTag:
		params->CodecTag = value;
		break;
	case VCA_SigTask:
		params->SigTask = (struct Task *)value;
		break;
	case VCA_SigMask:
		params->SigMask = value;
		break;

	/* input pictures */
	case VCA_Width:
		params->Width = value;
		break;
	case VCA_Height:
		params->Height = value;
		break;
	case VCA_InputFormat:
		params->InputFormat = value;
		break;
	case VCA_InputPitch:
		params->InputPitch = value;
		break;
	case VCA_SourceRect:
		set_rect(&params->SourceRect, &params->HaveSourceRect, value);
		break;

	/* output pictures: changeable while running */
	case VCA_OutputFormat:
		params->OutputFormat = value;
		params->Changed |= VCSPF_OutputGeometry;
		break;
	case VCA_OutputWidth:
		params->OutputWidth = value;
		params->Changed |= VCSPF_OutputGeometry;
		break;
	case VCA_OutputHeight:
		params->OutputHeight = value;
		params->Changed |= VCSPF_OutputGeometry;
		break;
	case VCA_OutputPitch:
		params->OutputPitch = value;
		params->Changed |= VCSPF_OutputGeometry;
		break;
	case VCA_DestRect:
		set_rect(&params->DestRect, &params->HaveDestRect, value);
		params->Changed |= VCSPF_OutputGeometry;
		break;
	case VCA_KeepAspect:
		params->KeepAspect = value != 0;
		params->Changed |= VCSPF_OutputGeometry;
		break;

	/* buffers */
	case VCA_InputCount:
		params->InputCount = value;
		break;
	case VCA_InputSize:
		params->InputSize = value;
		break;
	case VCA_OutputCount:
		params->OutputCount = value;
		break;
	case VCA_OutputSize:
		params->OutputSize = value;
		break;

	/* decoder */
	case VCA_Framed:
		params->Framed = value != 0;
		break;

	/* encoder; the bit rate and a key-frame request also while running */
	case VCA_FrameRateNum:
		params->FrameRate.Num = (LONG)value;
		break;
	case VCA_FrameRateDen:
		params->FrameRate.Den = (LONG)value;
		break;
	case VCA_Bitrate:
		params->Bitrate = value;
		params->Changed |= VCSPF_Bitrate;
		break;
	case VCA_RateControl:
		params->RateControl = value;
		break;
	case VCA_KeyInterval:
		params->KeyInterval = value;
		break;
	case VCA_Profile:
		params->Profile = value;
		break;
	case VCA_Level:
		params->Level = value;
		break;
	case VCA_Quality:
		params->Quality = value;
		break;
	case VCA_ColorSpace:
		params->ColorSpace = value;
		break;
	case VCA_RequestKeyFrame:
		params->RequestKeyFrame = value != 0;
		params->Changed |= VCSPF_RequestKeyFrame;
		break;
	default:
		break;
	}
}

/**
 * session_parse() - A session's tag list into its parameters.
 * @base:   the library base (utility.library walks the list)
 * @tags:   the caller's list
 * @params: the parameters, cleared by the caller with Size set
 * @open:   TRUE for a session open: every tag counts, Changed is cleared and
 *          the result is checked; FALSE for a running session (VC_SetAttrsTagList())
 *
 * The codec configuration (VCA_Extradata, VCA_ExtradataSize: a pointer and
 * its length, in any order) goes into @params as the caller gave it: the
 * backend reads it during the open only.
 *
 * Return: 0, or VCERR_BADPARAM at an open for a missing codec or kind, or a
 * signal task without a mask.
 */
static LONG session_parse(struct VideoCodecBase *base, struct TagItem *tags, struct VCSessionParams *params, BOOL open)
{
	struct Library *UtilityBase = base->UtilityBase;
	struct TagItem *state = tags, *tag;
	while ((tag = NextTagItem(&state)) != NULL) /* follows TAG_MORE, skips TAG_IGNORE and TAG_SKIP */
		session_tag(params, tag->ti_Tag, tag->ti_Data);

	CONST_APTR extradata = (CONST_APTR)GetTagData(VCA_Extradata, 0, tags);
	ULONG extradata_size = GetTagData(VCA_ExtradataSize, 0, tags);
	if (extradata != NULL && extradata_size != 0)
	{
		params->Extradata = extradata;
		params->ExtradataSize = extradata_size;
	}
	LONG err = 0;
	if (open)
	{
		params->Changed = 0;
		if (params->Codec == 0 || params->Kind < VCK_Decoder || params->Kind > VCK_Converter)
			err = VCERR_BADPARAM;
		if (params->SigTask != NULL && params->SigMask == 0)
			err = VCERR_BADPARAM; /* a task without a mask could never be signalled */
	}
	return err;
}

/* ---- sessions ----------------------------------------------------------------------- */

/**
 * session_open() - Open a session on the backend that should serve it.
 * @base:         the library base
 * @params:       the parsed parameters; a SigMask without a SigTask gets the caller's task
 * @backend_name: the backend the program asked for (VCA_Backend), or NULL
 * @err:          where the result goes: 0 or a VCERR_*
 *
 * The backend is the one whose codec row for the codec, its FourCC and the
 * kind serves sessions, takes the pictures asked for and has the highest
 * priority (vc_backend_choose()), or the one @backend_name means.
 *
 * Return: the session, or NULL with *@err set.
 */
static struct VCSession *session_open(struct VideoCodecBase *base, struct VCSessionParams *params, const char *backend_name, LONG *err)
{
	struct ExecBase *SysBase = base->SysBase;

	if (params->SigMask != 0 && params->SigTask == NULL)
		params->SigTask = FindTask(NULL);

	struct VCSession *session = AllocMem(sizeof(*session), MEMF_PUBLIC | MEMF_CLEAR);
	if (session == NULL)
	{
		*err = VCERR_NOMEM;
		return NULL;
	}
	const struct VCCodecInfo *row = NULL;
	ObtainSemaphore(&base->lock);
	vc_backends_load(base);
	struct vc_choice want = {
	    .codec = params->Codec,
	    .tag = params->CodecTag,
	    .kind = params->Kind,
	    .use = VCCF_Session,
	    .reads = params->Kind == VCK_Decoder ? 0u : vc_format_mask(params->InputFormat),
	    /* a decoder gets a converter behind it for a format it does not write (videocodec_chain.c) */
	    .writes = params->Kind == VCK_Decoder ? 0u : vc_format_mask(params->OutputFormat),
	};
	session->backend = vc_backend_choose(base, &want, backend_name, &row);
	*err = session->backend != NULL ? 0 : VCERR_NOBACKEND;
	if (session->backend != NULL)
		session->chain = chain_open(base, params, session->backend, row, err); /* a converter behind the decoder when the row lacks the format asked */
	ReleaseSemaphore(&base->lock);
	if (*err == 0)
	{
		struct Library *VCBackendBase = session->backend->library; /* the VCB_* calls go through it */
		LONG backend_err = 0;
		session->handle = VCB_OpenSession(params, &backend_err);
		if (session->handle == NULL)
			*err = backend_err != 0 ? backend_err : VCERR_UNSUPPORTED;
	}
	if (*err != 0)
	{
		if (session->chain != NULL)
			chain_close(base, session); /* its converter not open yet: the chain freed */
		FreeMem(session, sizeof(*session));
		return NULL;
	}
	session->magic = VC_SESSION_MAGIC;
	return session;
}

/**
 * session_close() - Close a session at its backend and free the record.
 * @base:    the library base
 * @session: an open session
 *
 * The magic is cleared first, so a stale pointer to the freed record is
 * refused by session_backend() for as long as the memory is not reused.
 */
static void session_close(struct VideoCodecBase *base, struct VCSession *session)
{
	struct ExecBase *SysBase = base->SysBase;
	struct Library *VCBackendBase = session->backend->library; /* the VCB_* calls go through it */
	session->magic = 0;
	if (session->chain != NULL)
		chain_close(base, session); /* the converter first: it holds the decoder's frames */
	VCB_CloseSession(session->handle);
	FreeMem(session, sizeof(*session));
}

/**
 * session_backend() - Check a client's session pointer and find its backend.
 * @session: the pointer the client passed
 *
 * Return: the backend's library base, or NULL for NULL or a pointer that is
 * not an open session.
 */
static struct Library *session_backend(const struct VCSession *session)
{
	return session != NULL && session->magic == VC_SESSION_MAGIC ? session->backend->library : NULL;
}

/* ---- the library's functions: libraries/videocodec.h documents what each does ------------- */

/**
 * VC_OpenSessionTagList() - Open a session (LVO).
 * @tags: the session's tags
 * @base: the library base (A6)
 *
 * Return: the session, or NULL with the reason at VCA_ErrorPtr.
 */
struct VCSession *VC_OpenSessionTagList(struct TagItem *tags asm("a0"), struct VideoCodecBase *base asm("a6"))
{
	struct Library *UtilityBase = base->UtilityBase;
	struct VCSessionParams params;
	memset(&params, 0, sizeof(params));
	params.Size = sizeof(params);

	LONG err = session_parse(base, tags, &params, TRUE);
	struct VCSession *session = err == 0 ? session_open(base, &params, (const char *)GetTagData(VCA_Backend, 0, tags), &err) : NULL;
	LONG *errp = (LONG *)GetTagData(VCA_ErrorPtr, 0, tags);
	if (errp != NULL)
		*errp = err;
	return session;
}

/**
 * VC_CloseSession() - Close a session and free all its objects (LVO).
 * @session: the session; NULL or a closed one is ignored
 * @base:    the library base (A6)
 */
VOID VC_CloseSession(struct VCSession *session asm("a0"), struct VideoCodecBase *base asm("a6"))
{
	if (session_backend(session) != NULL)
		session_close(base, session);
}

/**
 * VC_ImportPacket() - Make the caller's memory one of the session's packets (LVO).
 * @session:  the session
 * @side:     VCS_Input or VCS_Output
 * @data:     the memory
 * @capacity: its size in bytes
 * @base:     the library base (A6)
 *
 * Return: the packet object, or NULL.
 */
struct VCPacket *VC_ImportPacket(struct VCSession *session asm("a0"), ULONG side asm("d0"), APTR data asm("a1"),
                                 ULONG capacity asm("d1"), struct VideoCodecBase *base asm("a6"))
{
	(void)base;
	struct Library *VCBackendBase = session_backend(session);
	return VCBackendBase != NULL ? VCB_ImportPacket(session->handle, side, data, capacity) : NULL;
}

/**
 * VC_ImportFrame() - Make the caller's memory one of the session's frames (LVO).
 * @session: the session
 * @side:    VCS_Input or VCS_Output
 * @data:    the memory, of the session's picture geometry for that side
 * @base:    the library base (A6)
 *
 * Return: the frame object, or NULL.
 */
struct VCFrame *VC_ImportFrame(struct VCSession *session asm("a0"), ULONG side asm("d0"), APTR data asm("a1"),
                               struct VideoCodecBase *base asm("a6"))
{
	(void)base;
	struct Library *VCBackendBase = session_backend(session);
	if (VCBackendBase == NULL)
		return NULL;
	if (session->chain != NULL && side == VCS_Output)
		return chain_import_frame(session, data); /* the converter's; input frames would be the decoder's, which takes packets */
	return VCB_ImportFrame(session->handle, side, data);
}

/**
 * VC_SendPacket() - Hand a filled input packet to the session (LVO).
 * @session: the session
 * @packet:  the packet, Length, Pts and Flags set
 * @base:    the library base (A6)
 *
 * Return: 0, or a VCERR_*.
 */
LONG VC_SendPacket(struct VCSession *session asm("a0"), struct VCPacket *packet asm("a1"), struct VideoCodecBase *base asm("a6"))
{
	(void)base;
	struct Library *VCBackendBase = session_backend(session);
	return VCBackendBase != NULL ? VCB_SendPacket(session->handle, packet) : VCERR_BADPARAM;
}

/**
 * VC_SendFrame() - Hand a filled input frame to the session (LVO).
 * @session: the session
 * @frame:   the frame, Pts and Flags set
 * @base:    the library base (A6)
 *
 * Return: 0, or a VCERR_*.
 */
LONG VC_SendFrame(struct VCSession *session asm("a0"), struct VCFrame *frame asm("a1"), struct VideoCodecBase *base asm("a6"))
{
	(void)base;
	struct Library *VCBackendBase = session_backend(session);
	return VCBackendBase != NULL ? VCB_SendFrame(session->handle, frame) : VCERR_BADPARAM;
}

/**
 * VC_Poll() - Take the next item the session has for the client (LVO).
 * @session: the session
 * @item:    filled with the item
 * @base:    the library base (A6)
 *
 * Return: 1 when @item was filled, 0 when nothing is left (or for a bad
 * session or @item).
 */
LONG VC_Poll(struct VCSession *session asm("a0"), struct VCItem *item asm("a1"), struct VideoCodecBase *base asm("a6"))
{
	(void)base;
	struct Library *VCBackendBase = session_backend(session);
	if (VCBackendBase == NULL || item == NULL)
		return 0;
	if (session->chain != NULL)
		return chain_poll(session, item);
	return VCB_Poll(session->handle, item);
}

/**
 * VC_ReleaseFrame() - Give an output frame back to be filled again (LVO).
 * @session: the session
 * @frame:   the frame
 * @base:    the library base (A6)
 */
VOID VC_ReleaseFrame(struct VCSession *session asm("a0"), struct VCFrame *frame asm("a1"), struct VideoCodecBase *base asm("a6"))
{
	(void)base;
	struct Library *VCBackendBase = session_backend(session);
	if (VCBackendBase == NULL)
		return;
	if (session->chain != NULL)
		chain_release_frame(session, frame);
	else
		VCB_ReleaseFrame(session->handle, frame);
}

/**
 * VC_ReleasePacket() - Give an output packet back to be filled again (LVO).
 * @session: the session
 * @packet:  the packet
 * @base:    the library base (A6)
 */
VOID VC_ReleasePacket(struct VCSession *session asm("a0"), struct VCPacket *packet asm("a1"), struct VideoCodecBase *base asm("a6"))
{
	(void)base;
	struct Library *VCBackendBase = session_backend(session);
	if (VCBackendBase != NULL)
		VCB_ReleasePacket(session->handle, packet);
}

/**
 * VC_Flush() - Drop everything queued in the session, for a seek (LVO).
 * @session: the session
 * @base:    the library base (A6)
 *
 * Return: 0, or a VCERR_*.
 */
LONG VC_Flush(struct VCSession *session asm("a0"), struct VideoCodecBase *base asm("a6"))
{
	(void)base;
	struct Library *VCBackendBase = session_backend(session);
	if (VCBackendBase == NULL)
		return VCERR_BADPARAM;
	return session->chain != NULL ? chain_flush(session) : VCB_Flush(session->handle);
}

/**
 * VC_SetAttrsTagList() - Change a running session (LVO).
 * @session: the session
 * @tags:    the output geometry, VCA_Bitrate or VCA_RequestKeyFrame; others are ignored
 * @base:    the library base (A6)
 *
 * Only tags marked in Changed reach the backend; a list with none is a no-op.
 *
 * Return: 0, or a VCERR_*.
 */
LONG VC_SetAttrsTagList(struct VCSession *session asm("a0"), struct TagItem *tags asm("a1"), struct VideoCodecBase *base asm("a6"))
{
	struct Library *VCBackendBase = session_backend(session);
	if (VCBackendBase == NULL)
		return VCERR_BADPARAM;

	struct VCSessionParams params;
	memset(&params, 0, sizeof(params));
	params.Size = sizeof(params);

	LONG err = session_parse(base, tags, &params, FALSE);
	if (err != 0 || (params.Changed == 0 && params.SigMask == 0 && params.SigTask == NULL))
		return err;
	return session->chain != NULL ? chain_set_attrs(session, &params) : VCB_SetAttrs(session->handle, &params);
}
