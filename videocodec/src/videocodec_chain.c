/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * videocodec_chain.c - a converter behind a decoder (architecture.md 5.1).
 *
 * A decoder opened for an output format its row does not write runs with
 * its native output, and a converter session behind it makes the client's
 * format, size, place and aspect.  The converter is opened when the
 * stream's shape arrives, with that shape on its input; the decoder's
 * frames, listed by VCB_OutputFrame(), become its input frames by import
 * before its first send, so no pixel is copied.  What a chain would add to
 * a decoder's formats is reported by the same rule (chain_output_formats()).
 *
 * The pump runs inside VC_Poll(), in the client's task: the decoder's
 * frames go into the converter, the converter's returned input frames go
 * back to the decoder, and the client gets the converter's frames and
 * events and the decoder's packets.  Both backends signal the client's
 * task.  A new shape opens a new converter; the old one stays open while
 * the client holds frames of it, and closes when the last comes back.
 *
 * Every function calls one backend: the generated VCB_* calls go through a
 * local VCBackendBase, so the other backend is reached through a helper
 * (decoder_release(), converter_send() and so on).
 */
#include <exec/memory.h>
#include <memory.h>
#include <videocodec_private.h>

#define CHAIN_FRAMES_MAX 32       /* decoder frames a chain imports into its converter: a DPB of 16 and the client's few */
#define CHAIN_DECODER_ABI_MINOR 1 /* the backend ABI 1.x a chained decoder needs: VCB_OutputFrame() */

/* One of the decoder's frames as one of the converter's input frames */
struct chain_import
{
	struct VCFrame *decoder_frame;
	struct VCFrame *converter_frame;
	BOOL sent; /* in the converter until it returns it, with the decoder's picture or the end mark */
};

/* One of the converters' output frames in the client's hands */
struct chain_held
{
	struct VCFrame *frame;
	APTR converter; /* the one it came from: the current one, or one of an old shape kept open for it */
};

/*
 * A converter behind a decoder whose native output the client cannot take:
 * the pump moves the decoder's frames through it, and the client sees one
 * session.
 */
struct vc_chain
{
	struct vc_backend *converter_backend;    /* the converter's backend (the decoder's is the session's) */
	APTR converter;                          /* its session; NULL until the stream's shape is known */
	struct VCSessionParams converter_params; /* its open but the input side: the client's output side and signal */
	struct VCStreamInfo shape;               /* the decoder's last stream info: the converter's input side */
	BOOL info_pending;                       /* VCEV_StreamInfo for the client, with the converter's output */
	BOOL end_pending;                        /* the decoder's end came; it goes on once an import is free */
	struct chain_import *end_import;         /* the import carrying the end mark through the converter, or NULL */
	struct chain_import imports[CHAIN_FRAMES_MAX];
	ULONG import_count;
	struct chain_held held[CHAIN_FRAMES_MAX]; /* the frames the client holds */
	ULONG held_count;
};

/* What one poll of one backend gave */
enum poll_result
{
	POLL_NOTHING,    /* the backend had nothing */
	POLL_FOR_CLIENT, /* an item for the client: VC_Poll() returns it */
	POLL_USED,       /* an item the chain used itself (a frame passed on, an event acted on): poll again */
};

/* ---- the open ----------------------------------------------------------------------- */

/**
 * row_chains() - Whether a converter may be put behind a row's sessions.
 * @row: the row
 *
 * A decoder's row that serves sessions and names the formats it writes; one
 * that names none writes them all, and needs no converter.
 *
 * Return: TRUE when it may.
 */
static BOOL row_chains(const struct VCCodecInfo *row)
{
	return row->Kind == VCK_Decoder && (row->Flags & VCCF_Session) != 0 && row->OutputFormats != 0;
}

/**
 * converter_for() - The converter backend that joins a decoder's output to the format asked.
 * @base:    the library base, lock held, list loaded
 * @decoder: the decoder's backend
 * @row:     its row that won
 * @asked:   VCF_MASK() of the client's format; 0 for one past the masks
 *
 * The decoder must list its frames (VCB_OutputFrame()); the converter's row
 * must read one of the formats the decoder writes and write @asked.
 *
 * Return: the converter's backend, or NULL when there is none.
 */
static struct vc_backend *converter_for(struct VideoCodecBase *base, const struct vc_backend *decoder, const struct VCCodecInfo *row,
                                        ULONG asked)
{
	if (asked == 0 || VCB_ABI_MINOR(decoder->abi) < CHAIN_DECODER_ABI_MINOR)
		return NULL;
	struct vc_choice want = {
	    .codec = VCC_Raw,
	    .kind = VCK_Converter,
	    .use = VCCF_Session,
	    .reads = row->OutputFormats,
	    .writes = asked,
	};
	const struct VCCodecInfo *converter_row;
	struct vc_backend *converter = vc_backend_choose(base, &want, NULL, &converter_row);
	return converter != NULL && vc_row_takes(converter_row, &want) ? converter : NULL;
}

/**
 * chain_output_formats() - The formats a converter behind a row's sessions adds (VCCodecInfo.ChainOutputFormats).
 * @base:    the library base, lock held, list loaded
 * @decoder: the row's backend
 * @row:     the row
 *
 * Asks converter_for() of each format the row does not write, so the answer
 * is the one chain_open() would give.
 *
 * Return: VCF_MASK()s of the formats a chain would write; 0 for a row whose
 * sessions get none.
 */
ULONG chain_output_formats(struct VideoCodecBase *base, const struct vc_backend *decoder, const struct VCCodecInfo *row)
{
	ULONG formats = 0;
	if (!row_chains(row))
		return formats;
	for (ULONG format = 0; format < VC_FORMAT_BITS; format++)
	{
		ULONG asked = vc_format_mask(format);
		if (asked != 0 && (row->OutputFormats & asked) == 0 && converter_for(base, decoder, row, asked) != NULL)
			formats |= asked;
	}
	return formats;
}

/**
 * chain_open() - The chain a session needs, if any.
 * @base:    the library base, lock held, list loaded
 * @params:  the session's parameters; with a chain, the decoder's: its native output
 * @decoder: the decoder's backend
 * @row:     its row that won
 * @err:     where the error goes: 0 when no chain is needed, else why none could be made
 *
 * A decoder asked for an output format its row does not write needs one;
 * any other session, no format (the native output), one of the row's, or a
 * row that names none (it writes all) needs none.  The converter gets the
 * client's output side and signal; the decoder writes its own frames at the
 * stream's size.
 *
 * Return: the chain, or NULL.
 */
struct vc_chain *chain_open(struct VideoCodecBase *base, struct VCSessionParams *params, const struct vc_backend *decoder,
                            const struct VCCodecInfo *row, LONG *err)
{
	*err = 0;
	ULONG asked = vc_format_mask(params->OutputFormat);
	if (!row_chains(row) || params->OutputFormat == 0 || (row->OutputFormats & asked) != 0)
		return NULL;
	struct vc_backend *converter = converter_for(base, decoder, row, asked);
	if (converter == NULL)
	{
		*err = VCERR_UNSUPPORTED;
		return NULL;
	}

	struct ExecBase *SysBase = base->SysBase;
	struct vc_chain *chain = AllocMem(sizeof(*chain), MEMF_PUBLIC | MEMF_CLEAR);
	if (chain == NULL)
	{
		*err = VCERR_NOMEM;
		return NULL;
	}

	chain->converter_backend = converter;
	chain->converter_params = (struct VCSessionParams){
	    .Size = sizeof(struct VCSessionParams),
	    .Codec = VCC_Raw,
	    .Kind = VCK_Converter,
	    .SigTask = params->SigTask,
	    .SigMask = params->SigMask,
	    .OutputFormat = params->OutputFormat,
	    .OutputWidth = params->OutputWidth,
	    .OutputHeight = params->OutputHeight,
	    .OutputPitch = params->OutputPitch,
	    .DestRect = params->DestRect,
	    .HaveDestRect = params->HaveDestRect,
	    .KeepAspect = params->KeepAspect,
	    .OutputCount = params->OutputCount,
	    .OutputSize = params->OutputSize,
	};
	/* the decoder: its native output at the stream's size, its own frames */
	params->OutputFormat = 0;
	params->OutputWidth = 0;
	params->OutputHeight = 0;
	params->OutputPitch = 0;
	params->HaveDestRect = 0;
	params->KeepAspect = 0;
	params->OutputCount = 0;
	params->OutputSize = 0;
	return chain;
}

/* ---- the decoder -------------------------------------------------------------------- */

/**
 * decoder_frame() - One of the decoder's frames, by index (VCB_OutputFrame()).
 * @session: the session
 * @index:   0, 1, 2 and so on
 *
 * Return: the frame, NULL past the last.
 */
static struct VCFrame *decoder_frame(struct VCSession *session, ULONG index)
{
	struct Library *VCBackendBase = session->backend->library;
	return VCB_OutputFrame(session->handle, index);
}

/**
 * decoder_release() - Give the decoder one of its frames back (VCB_ReleaseFrame()).
 * @session: the session
 * @frame:   the decoder's frame
 */
static void decoder_release(struct VCSession *session, struct VCFrame *frame)
{
	struct Library *VCBackendBase = session->backend->library;
	VCB_ReleaseFrame(session->handle, frame);
}

/**
 * decoder_signal() - Pass a new signal task or mask on to the decoder (VCB_SetAttrs()).
 * @session: the session
 * @params:  the change; only its signal reaches the decoder, which keeps its native output
 *
 * Return: 0 or the decoder's error.
 */
static LONG decoder_signal(struct VCSession *session, const struct VCSessionParams *params)
{
	struct Library *VCBackendBase = session->backend->library;
	struct VCSessionParams signal = {.Size = sizeof(signal), .SigTask = params->SigTask, .SigMask = params->SigMask};
	return VCB_SetAttrs(session->handle, &signal);
}

/* ---- the converter ------------------------------------------------------------------ */

/**
 * import_of() - The import a frame belongs to.
 * @chain: the chain
 * @frame: one of the decoder's frames or of the converter's input frames
 *
 * The frame's UserData holds the import's index (imports_make()); a frame
 * that is none of them fails the check against the import's own frames.
 *
 * Return: the import, NULL for a frame that is none of them.
 */
static struct chain_import *import_of(struct vc_chain *chain, const struct VCFrame *frame)
{
	ULONG index = (ULONG)frame->UserData;
	if (index >= chain->import_count)
		return NULL;
	struct chain_import *import = &chain->imports[index];
	return import->decoder_frame == frame || import->converter_frame == frame ? import : NULL;
}

/**
 * import_returned() - Take an import back from the converter.
 * @session: the session
 * @import:  the import, sent
 *
 * The decoder gets back the picture it held.  The end mark held none: its
 * frame stayed the decoder's, and the end is through (end_under_way()).
 */
static void import_returned(struct VCSession *session, struct chain_import *import)
{
	struct vc_chain *chain = session->chain;
	import->sent = FALSE;
	if (import == chain->end_import)
		chain->end_import = NULL;
	else
		decoder_release(session, import->decoder_frame);
}

/**
 * held_add() - Note one of the converter's frames going to the client.
 * @chain: the chain
 * @frame: the frame
 *
 * A frame past the table's room is not noted: its release goes to the
 * current converter, and an old one is not kept open for it.
 */
static void held_add(struct vc_chain *chain, struct VCFrame *frame)
{
	if (chain->held_count < CHAIN_FRAMES_MAX)
		chain->held[chain->held_count++] = (struct chain_held){.frame = frame, .converter = chain->converter};
}

/**
 * held_take() - Take a frame the client gave back off the table.
 * @chain: the chain
 * @frame: the frame
 *
 * Return: the converter it came from, NULL for a frame not noted.
 */
static APTR held_take(struct vc_chain *chain, const struct VCFrame *frame)
{
	for (ULONG i = 0; i < chain->held_count; i++)
	{
		if (chain->held[i].frame != frame)
			continue;
		APTR converter = chain->held[i].converter;
		chain->held[i] = chain->held[--chain->held_count];
		return converter;
	}
	return NULL;
}

/**
 * held_from() - Whether the client holds a frame of a converter.
 * @chain:     the chain
 * @converter: the converter's session
 *
 * Return: TRUE when it does.
 */
static BOOL held_from(const struct vc_chain *chain, APTR converter)
{
	for (ULONG i = 0; i < chain->held_count; i++)
		if (chain->held[i].converter == converter)
			return TRUE;
	return FALSE;
}

/**
 * held_forget() - Take every frame of a converter off the table.
 * @chain:     the chain
 * @converter: the converter's session
 */
static void held_forget(struct vc_chain *chain, APTR converter)
{
	ULONG kept = 0;
	for (ULONG i = 0; i < chain->held_count; i++)
		if (chain->held[i].converter != converter)
			chain->held[kept++] = chain->held[i];
	chain->held_count = kept;
}

/**
 * converter_retire() - Let the converter go, if open (VCB_CloseSession()).
 * @session: the session
 *
 * Every import it held is taken back (import_returned()); what it was
 * still converting is dropped.  It closes now unless the client holds
 * frames of it: then it stays open until the last comes back
 * (chain_release_frame()).
 */
static void converter_retire(struct VCSession *session)
{
	struct vc_chain *chain = session->chain;
	if (chain->converter == NULL)
		return;
	struct Library *VCBackendBase = chain->converter_backend->library;
	if (!held_from(chain, chain->converter))
		VCB_CloseSession(chain->converter);
	chain->converter = NULL;
	for (ULONG i = 0; i < chain->import_count; i++)
		if (chain->imports[i].sent)
			import_returned(session, &chain->imports[i]);
	chain->import_count = 0;
}

/**
 * imports_make() - Import every one of the decoder's frames into the converter (VCB_ImportFrame()).
 * @session: the session, its converter just opened, no imports
 *
 * Return: 0; VCERR_BADSTATE when the decoder lists no frame, VCERR_NOMEM
 * when the converter refuses one or there are more than CHAIN_FRAMES_MAX.
 */
static LONG imports_make(struct VCSession *session)
{
	struct vc_chain *chain = session->chain;
	struct Library *VCBackendBase = chain->converter_backend->library;
	struct VCFrame *frame;
	while ((frame = decoder_frame(session, chain->import_count)) != NULL)
	{
		if (chain->import_count == CHAIN_FRAMES_MAX)
			return VCERR_NOMEM;
		struct chain_import *import = &chain->imports[chain->import_count];
		import->converter_frame = VCB_ImportFrame(chain->converter, VCS_Input, frame->Data);
		if (import->converter_frame == NULL)
			return VCERR_NOMEM;
		import->decoder_frame = frame;
		import->sent = FALSE;
		/* the chain is the only client of both frames, so their UserData is its own: the index, for import_of() */
		frame->UserData = (APTR)chain->import_count;
		import->converter_frame->UserData = (APTR)chain->import_count;
		chain->import_count++;
	}
	return chain->import_count != 0 ? 0 : VCERR_BADSTATE; /* the decoder's frames exist by its VCEV_StreamInfo */
}

/**
 * converter_open() - Open the converter for the stream's shape, the old one retired first (VCB_OpenSession()).
 * @session: the session, its chain's shape just set
 *
 * The input side is the decoder's frames: their format and size, the
 * visible picture as the part to take.
 *
 * Return: 0, or the converter's error.
 */
static LONG converter_open(struct VCSession *session)
{
	struct vc_chain *chain = session->chain;
	struct Library *VCBackendBase = chain->converter_backend->library;
	converter_retire(session);

	struct VCSessionParams params = chain->converter_params;
	params.Width = chain->shape.Width;
	params.Height = chain->shape.Height;
	params.InputFormat = chain->shape.Format;
	params.InputPitch = 0; /* the backend's for the format and size: the decoder lays its frames out so */
	params.SourceRect = chain->shape.Crop;
	params.HaveSourceRect = 1;

	LONG err = 0;
	chain->converter = VCB_OpenSession(&params, &err);
	if (chain->converter == NULL)
		return err != 0 ? err : VCERR_UNSUPPORTED;
	err = imports_make(session);
	if (err != 0)
	{
		converter_retire(session); /* none of its frames went out: it closes */
		return err;
	}
	chain->info_pending = TRUE;
	return 0;
}

/**
 * converter_send() - Send an import into the converter (VCB_SendFrame()).
 * @session: the session, its converter open
 * @import:  the import, not sent
 * @pts:     the picture's time
 * @flags:   VCFF_*
 *
 * Return: 0 with the import sent, or the converter's error.
 */
static LONG converter_send(struct VCSession *session, struct chain_import *import, VCTime pts, ULONG flags)
{
	struct vc_chain *chain = session->chain;
	struct Library *VCBackendBase = chain->converter_backend->library;
	import->converter_frame->Pts = pts;
	import->converter_frame->Flags = flags;
	LONG err = VCB_SendFrame(chain->converter, import->converter_frame);
	if (err == 0)
		import->sent = TRUE;
	return err;
}

/**
 * converter_flush() - Flush the converter (VCB_Flush()).
 * @session: the session, its converter open
 *
 * Return: 0 or the converter's error.
 */
static LONG converter_flush(struct VCSession *session)
{
	struct vc_chain *chain = session->chain;
	struct Library *VCBackendBase = chain->converter_backend->library;
	return VCB_Flush(chain->converter);
}

/**
 * converter_set_attrs() - Pass a change on to the converter (VCB_SetAttrs()).
 * @session: the session, its converter open
 * @params:  the change
 *
 * Return: 0 or the converter's error.
 */
static LONG converter_set_attrs(struct VCSession *session, struct VCSessionParams *params)
{
	struct vc_chain *chain = session->chain;
	struct Library *VCBackendBase = chain->converter_backend->library;
	return VCB_SetAttrs(chain->converter, params);
}

/* ---- the pump ----------------------------------------------------------------------- */

/**
 * event_item() - Make an item an event.
 * @item:   the item
 * @event:  VCEV_*
 * @status: its status
 */
static void event_item(struct VCItem *item, ULONG event, LONG status)
{
	item->Type = VCIT_Event;
	memset(&item->u.Event, 0, sizeof(item->u.Event));
	item->u.Event.Event = event;
	item->u.Event.Status = status;
}

/**
 * info_item() - Make an item the client's VCEV_StreamInfo: the stream's shape as the converter writes it.
 * @chain: the chain
 * @item:  the item
 *
 * The frames are of the client's format and size (the visible picture's
 * when it gave none); the visible picture is its destination rectangle,
 * else the whole frame, of square pixels.  The rate and the interlacing
 * are the stream's.
 */
static void info_item(const struct vc_chain *chain, struct VCItem *item)
{
	const struct VCSessionParams *out = &chain->converter_params;
	struct VCStreamInfo *info = &item->u.Event.Stream;
	event_item(item, VCEV_StreamInfo, 0);
	info->Format = out->OutputFormat;
	info->Width = out->OutputWidth != 0 ? out->OutputWidth : (ULONG)chain->shape.Crop.Width;
	info->Height = out->OutputHeight != 0 ? out->OutputHeight : (ULONG)chain->shape.Crop.Height;
	if (out->HaveDestRect)
		info->Crop = out->DestRect;
	else
	{
		info->Crop.Width = (LONG)info->Width;
		info->Crop.Height = (LONG)info->Height;
	}
	info->FrameRate = chain->shape.FrameRate;
	info->Par.Num = 1;
	info->Par.Den = 1;
	info->Flags = chain->shape.Flags;
}

/**
 * end_send() - Send the decoder's end on into the converter, on an import not in it.
 * @session: the session, its end pending
 * @item:    the client's item
 *
 * The end mark is a frame of VCFF_EndOfStream with no picture
 * (VCFF_NoPicture), so it may go on an import whose frame the decoder
 * holds.  With every import in the converter it waits for one to return.
 * Without a converter (no picture came) the end is the client's at once.
 *
 * Return: TRUE with @item filled: the end, or the converter's refusal of the end mark.
 */
static BOOL end_send(struct VCSession *session, struct VCItem *item)
{
	struct vc_chain *chain = session->chain;
	if (chain->converter == NULL)
	{
		chain->end_pending = FALSE;
		event_item(item, VCEV_EndOfStream, 0);
		return TRUE;
	}
	for (ULONG i = 0; i < chain->import_count; i++)
	{
		struct chain_import *import = &chain->imports[i];
		if (import->sent)
			continue;
		chain->end_pending = FALSE;
		LONG err = converter_send(session, import, VC_TIME_UNKNOWN, VCFF_EndOfStream | VCFF_NoPicture);
		if (err != 0)
		{
			event_item(item, VCEV_Error, err);
			return TRUE;
		}
		chain->end_import = import;
		return FALSE;
	}
	return FALSE;
}

/**
 * picture_send() - Send a decoder's picture on into the converter, on its import.
 * @session: the session
 * @item:    the decoder's VCIT_Frame; the client's error when the picture cannot go
 *
 * A picture that cannot go goes back to the decoder.
 *
 * Return: POLL_USED when it went, POLL_FOR_CLIENT with the error.
 */
static enum poll_result picture_send(struct VCSession *session, struct VCItem *item)
{
	struct VCFrame *frame = item->u.Frame;
	struct chain_import *import = import_of(session->chain, frame);
	/* none: a frame the decoder did not list, or one before its stream info */
	LONG err = import != NULL ? converter_send(session, import, frame->Pts, frame->Flags & VCFF_KeyFrame) : VCERR_BADSTATE;
	if (err == 0)
		return POLL_USED;
	decoder_release(session, frame);
	event_item(item, VCEV_Error, err);
	return POLL_FOR_CLIENT;
}

/**
 * converter_poll() - Poll the converter once (VCB_Poll()).
 * @session: the session, its converter open
 * @item:    the client's item
 *
 * A frame (noted: held_add()) and an event are the client's; a returned
 * input frame is taken back (import_returned()).
 *
 * Return: what the poll gave.
 */
static enum poll_result converter_poll(struct VCSession *session, struct VCItem *item)
{
	struct vc_chain *chain = session->chain;
	struct Library *VCBackendBase = chain->converter_backend->library;
	if (VCB_Poll(chain->converter, item) == 0)
		return POLL_NOTHING;
	if (item->Type == VCIT_Frame)
		held_add(chain, item->u.Frame);
	if (item->Type == VCIT_Frame || item->Type == VCIT_Event)
		return POLL_FOR_CLIENT;
	if (item->Type == VCIT_FrameReturned)
	{
		struct chain_import *import = import_of(chain, item->u.Frame);
		if (import != NULL && import->sent)
			import_returned(session, import);
	}
	return POLL_USED; /* a returned input frame; a converter gives no packets */
}

/**
 * decoder_poll() - Poll the decoder once (VCB_Poll()).
 * @session: the session
 * @item:    the client's item
 *
 * A frame goes into the converter (picture_send()); the stream info opens
 * the converter for its shape; the end waits for an import (end_send()); a
 * packet and any other event are the client's.
 *
 * Return: what the poll gave.
 */
static enum poll_result decoder_poll(struct VCSession *session, struct VCItem *item)
{
	struct vc_chain *chain = session->chain;
	struct Library *VCBackendBase = session->backend->library;
	if (VCB_Poll(session->handle, item) == 0)
		return POLL_NOTHING;
	if (item->Type == VCIT_Frame)
		return picture_send(session, item);
	if (item->Type != VCIT_Event)
		return POLL_FOR_CLIENT; /* a packet back */
	switch (item->u.Event.Event)
	{
	case VCEV_StreamInfo:
	{
		chain->shape = item->u.Event.Stream;
		LONG err = converter_open(session);
		if (err == 0)
			return POLL_USED; /* the client's info is next (chain_poll()) */
		event_item(item, VCEV_Error, err);
		return POLL_FOR_CLIENT;
	}
	case VCEV_EndOfStream:
		chain->end_pending = TRUE;
		return POLL_USED;
	default:
		return POLL_FOR_CLIENT;
	}
}

/**
 * end_under_way() - Whether the decoder's end is pending or in the converter.
 * @chain: the chain
 *
 * The decoder is not polled meanwhile: no picture of what follows may pass
 * its end, and the end mark's frame may be the one the decoder fills next.
 *
 * Return: TRUE until the end mark is back from the converter.
 */
static BOOL end_under_way(const struct vc_chain *chain)
{
	return chain->end_pending || chain->end_import != NULL;
}

/**
 * chain_poll() - Take the next item a chained session has for the client (VC_Poll()).
 * @session: the session
 * @item:    the client's item
 *
 * The stream info first when one is due; then the decoder's end on into
 * the converter; then the converter's items, then the decoder's (unless
 * end_under_way()).  An item the chain uses itself is consumed and the
 * next looked at, so one call finds the first the client can have.
 *
 * Return: 1 with @item filled, 0 when nothing is ready.
 */
LONG chain_poll(struct VCSession *session, struct VCItem *item)
{
	struct vc_chain *chain = session->chain;
	for (;;)
	{
		if (chain->info_pending)
		{
			chain->info_pending = FALSE;
			info_item(chain, item);
			return 1;
		}
		if (chain->end_pending && end_send(session, item))
			return 1;
		enum poll_result result = chain->converter != NULL ? converter_poll(session, item) : POLL_NOTHING;
		if (result == POLL_NOTHING && !end_under_way(chain))
			result = decoder_poll(session, item);
		if (result == POLL_FOR_CLIENT)
			return 1;
		if (result == POLL_NOTHING)
			return 0;
	}
}

/* ---- the session's other calls ------------------------------------------------------ */

/**
 * chain_import_frame() - Make the client's memory one of the converter's output frames (VC_ImportFrame()).
 * @session: the session
 * @data:    the memory, of the output geometry
 *
 * The converter exists once the stream's shape is known (VCEV_StreamInfo);
 * before that there is nothing to import into.
 *
 * Return: the frame, or NULL.
 */
struct VCFrame *chain_import_frame(struct VCSession *session, APTR data)
{
	struct vc_chain *chain = session->chain;
	struct Library *VCBackendBase = chain->converter_backend->library;
	return chain->converter != NULL ? VCB_ImportFrame(chain->converter, VCS_Output, data) : NULL;
}

/**
 * chain_release_frame() - Give a converter one of its output frames back (VC_ReleaseFrame()).
 * @session: the session
 * @frame:   the frame
 *
 * The frame goes to the converter it came from; one of an old shape closes
 * with the last of its frames.  A frame not noted goes to the current one.
 */
void chain_release_frame(struct VCSession *session, struct VCFrame *frame)
{
	struct vc_chain *chain = session->chain;
	struct Library *VCBackendBase = chain->converter_backend->library;
	APTR converter = held_take(chain, frame);
	if (converter == NULL)
		converter = chain->converter;
	if (converter == NULL)
		return;
	VCB_ReleaseFrame(converter, frame);
	if (converter != chain->converter && !held_from(chain, converter))
		VCB_CloseSession(converter);
}

/**
 * chain_flush() - Flush both sessions, for a seek (VC_Flush()).
 * @session: the session
 *
 * A pending end is dropped.  What the converter held comes back by its
 * poll and goes back to the decoder then.
 *
 * Return: 0, or the first error of the two.
 */
LONG chain_flush(struct VCSession *session)
{
	struct vc_chain *chain = session->chain;
	struct Library *VCBackendBase = session->backend->library;
	LONG err = VCB_Flush(session->handle);
	LONG converter_err = chain->converter != NULL ? converter_flush(session) : 0;
	chain->end_pending = FALSE;
	return err != 0 ? err : converter_err;
}

/**
 * output_merge() - Merge a change of the output geometry into the converter's parameters.
 * @params:  the converter's parameters
 * @changes: the change, VCSPF_OutputGeometry set
 *
 * As a backend merges it (VCSPF_OutputGeometry): a field given replaces the
 * one kept, 0 keeps it; a new format or width without a pitch drops the
 * pitch kept; a destination rectangle and KeepAspect replace each other.
 */
static void output_merge(struct VCSessionParams *params, const struct VCSessionParams *changes)
{
	if (changes->OutputFormat != 0)
		params->OutputFormat = changes->OutputFormat;
	if (changes->OutputWidth != 0)
		params->OutputWidth = changes->OutputWidth;
	if (changes->OutputHeight != 0)
		params->OutputHeight = changes->OutputHeight;
	if (changes->OutputPitch != 0)
		params->OutputPitch = changes->OutputPitch;
	else if (changes->OutputWidth != 0 || changes->OutputFormat != 0)
		params->OutputPitch = 0;
	if (changes->HaveDestRect)
	{
		params->DestRect = changes->DestRect;
		params->HaveDestRect = 1;
		params->KeepAspect = 0;
	}
	if (changes->KeepAspect)
	{
		params->KeepAspect = 1;
		params->HaveDestRect = 0;
	}
}

/**
 * chain_set_attrs() - Change a chained session (VC_SetAttrs()).
 * @session: the session
 * @params:  the change
 *
 * The change is the converter's; the converter's parameters keep what it
 * took, for its next open.  A new signal reaches the decoder too.
 *
 * Return: 0, or the first error of the two.
 */
LONG chain_set_attrs(struct VCSession *session, struct VCSessionParams *params)
{
	struct vc_chain *chain = session->chain;
	LONG err = chain->converter != NULL ? converter_set_attrs(session, params) : 0;
	if (err == 0 && (params->Changed & VCSPF_OutputGeometry) != 0)
		output_merge(&chain->converter_params, params);

	if (params->SigTask == NULL && params->SigMask == 0)
		return err;
	if (params->SigTask != NULL)
		chain->converter_params.SigTask = params->SigTask;
	if (params->SigMask != 0)
		chain->converter_params.SigMask = params->SigMask;

	LONG decoder_err = decoder_signal(session, params);
	return err != 0 ? err : decoder_err;
}

/**
 * chain_close() - Close every converter and free the chain; the decoder is the caller's.
 * @base:    the library base
 * @session: the session, its chain set; its decoder open, or not yet (a failed open)
 *
 * The client's frames go with the session, so the converters kept open
 * for them close too.
 */
void chain_close(struct VideoCodecBase *base, struct VCSession *session)
{
	struct ExecBase *SysBase = base->SysBase;
	struct vc_chain *chain = session->chain;
	struct Library *VCBackendBase = chain->converter_backend->library;
	converter_retire(session);
	while (chain->held_count != 0)
	{
		APTR converter = chain->held[0].converter;
		held_forget(chain, converter);
		VCB_CloseSession(converter);
	}
	FreeMem(chain, sizeof(*chain));
	session->chain = NULL;
}
