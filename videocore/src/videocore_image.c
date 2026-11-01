/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * videocore_image.c - the image calls (VCB_Image()): one picture each,
 * blocking, in the caller's task.
 *
 * Each runs one or two of the backend's own sessions, with the caller's
 * memory imported wherever the hardware can use it as it is:
 *
 *   converter   ril.isp: the caller's picture in, the caller's picture out,
 *               or its own frame and one copy when the VPU cannot write
 *               the caller's (still_bounces())
 *   encoder     ril.image_encode: the caller's picture in, its buffer out
 *   decoder     the still kind (session_still.c), straight into the
 *               caller's picture when the pipeline writes that size, format
 *               and layout (still_direct()); otherwise into its own frame,
 *               and from there by the converter
 *
 * This file runs the calls: it opens and closes the sessions, feeds a
 * decoder its bytes, waits for the picture and sorts the outcome.  What the
 * pipeline can do is the still kind's to say (still_decodes(),
 * still_direct(), still_between(), still_fits()).  A picture is checked
 * from its whole header before anything opens: the image decoder must not
 * fail once it holds part of a picture (that has left the firmware's MMAL
 * dead until a Pi restart).  Its bytes are copied into the session's own
 * input packets as they come back: the decoder wants at least two, and
 * joins them.  The session calls run under session_lock, the waits between
 * them without it: the one place a VCB_* call does not hold it throughout.
 *
 * The calls queue on image_lock, one on the VPU at a time.  Side by side they
 * gain nothing, the VPU working through them in turn, and each sees the
 * memory as it was before the others took theirs (still_fits()): twenty 1080p
 * JPEGs ran the codec heap out, and a decoder that fails holding a picture
 * can stop the firmware until a Pi restart.  Sessions do not queue here: a
 * video plays on beside a picture.
 */
#include <memory.h>
#include <drv_timer.h>

#include <videocore_private.h>
#include <image_info.h>

#define IMAGE_TIMEOUT_MS 5000 /* each pass's wait when the caller gives none */
#define IMAGE_RUN_WAITING 1   /* image_drain(): nothing ended the wait yet */

/* One image call's waiting: the caller's task, woken by its sessions' signal or the timer */
struct image_run
{
	struct VideoCoreBase *base;
	struct Task *task;      /* the caller's: its sessions signal it */
	ULONG sigmask;          /* the caller's signal bit the sessions raise */
	struct drv_timer timer; /* each pass's timeout */
	ULONG timeout_ms;
};

/* A decoder's bytes, copied piece by piece into its own input packets as they come back */
struct image_feed
{
	const UBYTE *data;
	u32 length, fed;
	u32 skip_from, skip_to; /* bytes left out, still_strips() */
};

/* ---- the picture ------------------------------------------------------------------- */

/**
 * image_check() - A still picture's header, read whole and judged before any session opens.
 * @source: the picture's bytes
 * @codec:  VCC_* to read them as
 * @info:   where the header's facts go
 *
 * A picture cut short is refused too: the decoder would wait for the rest
 * and be closed holding part of it.
 *
 * Return: 0, VCERR_STREAM for bytes that are no picture this knows or a
 * picture cut short, or VCERR_UNSUPPORTED for a picture the pipeline does not decode right.
 */
static LONG image_check(const struct VCImageSide *source, ULONG codec, struct image_facts *info)
{
	if (image_info(source->Data, source->Length, codec, info) != 0 || info->width == 0 || info->height == 0 || info->header == 0)
		return VCERR_STREAM;
	if (!still_decodes(info))
	{
		Kprintf("[videocore] image: %ldx%ld, colour %ld, %ld bits, transparency %ld, coding %ld, interlaced %ld, header %ld: "
		        "not for the decoder\n",
		        info->width, info->height, info->model, info->bits, info->transparency, info->coding, info->interlaced, info->header);
		return VCERR_UNSUPPORTED;
	}
	if (!info->whole)
	{
		Kprintf("[videocore] image: %ldx%ld in %ld bytes: its data cut short\n", info->width, info->height, source->Length);
		return VCERR_STREAM;
	}
	return 0;
}

/**
 * image_dest() - The caller's picture for a decode, its defaults filled in: I420, the picture's own size.
 * @dest: the caller's destination
 * @info: the picture's header facts
 *
 * Return: the destination.
 */
static struct VCImageSide image_dest(const struct VCImageSide *dest, const struct image_facts *info)
{
	struct VCImageSide filled = *dest;
	if (filled.Format == 0)
		filled.Format = VCF_I420;
	if (filled.Width == 0 && filled.Height == 0)
	{
		filled.Width = info->width;
		filled.Height = info->height;
	}
	return filled;
}

/* ---- the sessions and the wait ------------------------------------------------------- */

/**
 * image_params() - The parameters of one of the call's sessions, before its own.
 * @run:    the call
 * @params: the parameters
 * @kind:   VCK_*
 * @codec:  VCC_*
 */
static void image_params(const struct image_run *run, struct VCSessionParams *params, ULONG kind, ULONG codec)
{
	memset(params, 0, sizeof(*params));
	params->Size = sizeof(*params);
	params->Kind = kind;
	params->Codec = codec;
	params->SigTask = run->task;
	params->SigMask = run->sigmask;
	params->InputCount = 1; /* the caller's memory: one object each side */
	params->OutputCount = 1;
}

/**
 * image_params_input() - The caller's picture as the parameters' input.
 * @params: the session's parameters
 * @source: the caller's picture
 */
static void image_params_input(struct VCSessionParams *params, const struct VCImageSide *source)
{
	params->Width = source->Width;
	params->Height = source->Height;
	params->InputFormat = source->Format;
	params->InputPitch = source->Pitch;
}

/**
 * image_params_output() - The caller's picture as the parameters' output.
 * @params:      the session's parameters
 * @dest:        the caller's picture
 * @keep_aspect: the caller's VCA_KeepAspect
 */
static void image_params_output(struct VCSessionParams *params, const struct VCImageSide *dest, ULONG keep_aspect)
{
	params->OutputFormat = dest->Format;
	params->OutputWidth = dest->Width;
	params->OutputHeight = dest->Height;
	params->OutputPitch = dest->Pitch;
	params->DestRect = dest->Rect;
	params->HaveDestRect = dest->HaveRect;
	params->KeepAspect = keep_aspect;
}

/**
 * image_frame_side() - A session's frame as one side of a call: its memory, layout and visible part.
 * @frame: the frame
 *
 * Return: the side.
 */
static struct VCImageSide image_frame_side(const struct VCFrame *frame)
{
	struct VCImageSide side = {.Data = frame->Data,
	                           .Format = frame->Format,
	                           .Width = frame->Width,
	                           .Height = frame->Height,
	                           .Pitch = frame->Pitch[0],
	                           .Rect = frame->Crop,
	                           .HaveRect = TRUE};
	return side;
}

/**
 * image_open_session() - Open one of the call's sessions.
 * @run:     the call
 * @params:  its parameters
 * @session: where the session goes
 *
 * Return: 0 or the open's error.
 */
static LONG image_open_session(struct image_run *run, const struct VCSessionParams *params, struct vc_session **session)
{
	struct ExecBase *SysBase = run->base->SysBase;
	LONG error;
	PERF_T0(t0);
	ObtainSemaphore(&run->base->session_lock);
	*session = session_open(run->base, params, TRUE, &error);
	ReleaseSemaphore(&run->base->session_lock);
	PERF_ADD(&run->base->perf, VCPERF_IMG_OPEN, t0);
	return error;
}

/**
 * image_close_session() - Close one of the call's sessions.
 * @run:     the call
 * @session: the session, or NULL
 * @outcome: how its pass ended: only one that did its work hands its
 *           components on to the next call (a kept one must not stop mid-picture)
 */
static void image_close_session(struct image_run *run, struct vc_session *session, LONG outcome)
{
	struct ExecBase *SysBase = run->base->SysBase;
	if (session == NULL)
		return;
	PERF_T0(t0);
	ObtainSemaphore(&run->base->session_lock);
	session_close(session, outcome == 0);
	ReleaseSemaphore(&run->base->session_lock);
	PERF_ADD(&run->base->perf, VCPERF_IMG_CLOSE, t0);
}

/**
 * image_attach() - The caller's memory as a session's output, and its input sent.
 * @run:     the call
 * @session: the session, open
 * @in:      the source picture, or NULL when the input is fed
 * @out:     the destination: a picture, room for compressed bytes, or NULL for the session's own frame
 * @room:    the room at @out when the output is compressed (an encoder), else 0
 *
 * The output goes into service first; the input goes with the end mark.
 *
 * Return: 0, VCERR_BADPARAM if the session takes either memory block not, or the send's error.
 */
static LONG image_attach(struct image_run *run, struct vc_session *session, APTR in, APTR out, u32 room)
{
	struct ExecBase *SysBase = run->base->SysBase;
	LONG error = 0;
	ObtainSemaphore(&run->base->session_lock);
	if (out == NULL)
	{
		/* the session's own output, still_bounces() */
	}
	else if (room != 0)
	{
		struct VCPacket *packet = session_import_packet(session, VCS_Output, out, room);
		if (packet != NULL)
			session_release_packet(session, packet); /* into service */
		else
			error = VCERR_BADPARAM;
	}
	else
	{
		struct VCFrame *frame = session_import_frame(session, VCS_Output, out);
		if (frame != NULL)
			session_release_frame(session, frame);
		else
			error = VCERR_BADPARAM;
	}
	if (error == 0 && in != NULL)
	{
		struct VCFrame *frame = session_import_frame(session, VCS_Input, in);
		if (frame == NULL)
			error = VCERR_BADPARAM;
		else
		{
			frame->Pts = VC_TIME_UNKNOWN;
			frame->Flags = VCFF_EndOfStream;
			error = session_send_frame(session, frame);
		}
	}
	ReleaseSemaphore(&run->base->session_lock);
	return error;
}

/**
 * image_feed_next() - Fill one of a decoder's own input packets with the next piece of the bytes and send it.
 * @feed:    the bytes
 * @session: the decoder's session
 * @packet:  a packet the session handed back
 *
 * The end mark goes with the last piece; once everything went, the packet stays idle.
 *
 * Return: 0 or the send's error.
 */
static LONG image_feed_next(struct image_feed *feed, struct vc_session *session, struct VCPacket *packet)
{
	if (feed->fed >= feed->length)
		return 0;
	u32 n = 0;
	while (n < packet->Capacity && feed->fed < feed->length)
	{
		if (feed->fed == feed->skip_from)
			feed->fed = feed->skip_to; /* the same packet goes on behind it: the frame header must be in the first */
		u32 stop = feed->fed < feed->skip_from ? feed->skip_from : feed->length;
		u32 piece = umin(stop - feed->fed, packet->Capacity - n);
		memcpy((UBYTE *)packet->Data + n, feed->data + feed->fed, piece);
		feed->fed += piece;
		n += piece;
	}
	packet->Length = n;
	packet->Pts = VC_TIME_UNKNOWN;
	packet->Flags = feed->fed == feed->length ? VCPF_EndOfStream : 0;
	return session_send_packet(session, packet);
}

/**
 * image_outcome() - What one item means for a call waiting on its output.
 * @item: the item
 * @out:  where the output's item goes
 *
 * Return: 0 for the output (stored at @out), the session's error for
 * VCEV_Error, VCERR_STREAM for an end of stream without output, or
 * IMAGE_RUN_WAITING for anything else (an input back, the stream's shape).
 */
static LONG image_outcome(const struct VCItem *item, struct VCItem *out)
{
	switch (item->Type)
	{
	case VCIT_Frame:
	case VCIT_Packet:
		*out = *item;
		return 0;
	case VCIT_Event:
		if (item->u.Event.Event == VCEV_Error)
			return item->u.Event.Status != 0 ? item->u.Event.Status : VCERR_STREAM;
		return item->u.Event.Event == VCEV_EndOfStream ? VCERR_STREAM : IMAGE_RUN_WAITING;
	default:
		return IMAGE_RUN_WAITING;
	}
}

/**
 * image_drain() - Take everything a session has, feeding it, until something ends the wait.
 * @run:     the call
 * @session: the session
 * @feed:    the bytes for its own input packets, or NULL (its input was sent)
 * @out:     where the output's item goes
 *
 * Return: what ended the wait (image_outcome()), a feed's send error, or
 * IMAGE_RUN_WAITING when the session has nothing more for now.
 */
static LONG image_drain(struct image_run *run, struct vc_session *session, struct image_feed *feed, struct VCItem *out)
{
	struct ExecBase *SysBase = run->base->SysBase;
	LONG result = IMAGE_RUN_WAITING;
	struct VCItem item;
	ObtainSemaphore(&run->base->session_lock);
	while (result == IMAGE_RUN_WAITING && session_poll(session, &item))
	{
		if (item.Type == VCIT_PacketReturned && feed != NULL)
		{
			LONG error = image_feed_next(feed, session, item.u.Packet);
			if (error != 0)
				result = error;
		}
		else
			result = image_outcome(&item, out);
	}
	ReleaseSemaphore(&run->base->session_lock);
	return result;
}

/**
 * image_wait() - Wait for a session's output, at most the call's timeout.
 * @run:     the call
 * @session: the session, its input sent or to be fed
 * @feed:    the bytes for its own input packets, or NULL
 * @out:     where the output's item goes
 *
 * The timer is armed once, so it bounds the whole wait however often the
 * session signals.  Each round takes what the session has before it looks at
 * the timer, so an output that comes with the timeout is not lost.  The timer
 * is collected before returning, which also clears its signal: the next pass
 * waits on the same bit.
 *
 * Return: 0 with @out filled, the session's error, or VCERR_TIMEOUT.
 */
static LONG image_wait(struct image_run *run, struct vc_session *session, struct image_feed *feed, struct VCItem *out)
{
	struct ExecBase *SysBase = run->base->SysBase;
	ULONG timer_signal = drv_timer_sigmask(&run->timer);
	PERF_T0(t0);
	drv_timer_arm_ms(&run->timer, run->timeout_ms);
	LONG result;
	for (;;)
	{
		result = image_drain(run, session, feed, out);
		if (result != IMAGE_RUN_WAITING)
			break;
		if (Wait(run->sigmask | timer_signal) & timer_signal)
		{
			result = image_drain(run, session, feed, out);
			if (result == IMAGE_RUN_WAITING)
				result = VCERR_TIMEOUT;
			break;
		}
	}
	drv_timer_cancel(&run->timer);
	PERF_ADD(&run->base->perf, VCPERF_IMG_WAIT, t0);
	return result;
}

/* ---- the three calls ---------------------------------------------------------------- */

/**
 * image_convert() - One picture into another size, format or place: ril.isp over the caller's memory.
 * @run:         the call
 * @source:      the picture: its memory, format, size, pitch and part
 * @dest:        the destination: its memory, format, size (0x0: the part's), pitch and place
 * @keep_aspect: the caller's VCA_KeepAspect
 *
 * Return: 0 or the error.
 */
static LONG image_convert(struct image_run *run, const struct VCImageSide *source, const struct VCImageSide *dest, ULONG keep_aspect)
{
	struct VCSessionParams params;
	image_params(run, &params, VCK_Converter, VCC_Raw);
	image_params_input(&params, source);
	params.SourceRect = source->Rect;
	params.HaveSourceRect = source->HaveRect;
	image_params_output(&params, dest, keep_aspect);

	struct vc_session *session;
	LONG error = image_open_session(run, &params, &session);
	BOOL bounce = error == 0 && still_bounces(run->base, dest, keep_aspect);
	if (error == 0)
		error = image_attach(run, session, source->Data, bounce ? NULL : dest->Data, 0);
	struct VCItem item;
	if (error == 0)
		error = image_wait(run, session, NULL, &item);
	if (error == 0 && bounce)
		still_bounce_copy(item.u.Frame, dest);
	image_close_session(run, session, error);
	return error;
}

/**
 * image_encode() - One picture into a JPEG: ril.image_encode over the caller's memory.
 * @run:    the call
 * @params: the call's parameters; Written is set
 *
 * Return: 0, VCERR_UNSUPPORTED for a JPEG the encoder split over several
 * packets (one does not fit the caller's room), or the error.
 */
static LONG image_encode(struct image_run *run, struct VCImageParams *params)
{
	const struct VCImageSide *source = &params->Source;
	struct VCSessionParams session_params;
	image_params(run, &session_params, VCK_Encoder, params->Codec);
	image_params_input(&session_params, source);
	session_params.Quality = params->Quality;

	struct vc_session *session;
	LONG error = image_open_session(run, &session_params, &session);
	if (error == 0)
		error = image_attach(run, session, source->Data, params->Dest.Data, params->Dest.Length);
	struct VCItem item;
	if (error == 0)
		error = image_wait(run, session, NULL, &item);
	if (error == 0 && (item.u.Packet->Flags & (VCPF_FrameEnd | VCPF_EndOfStream)) == 0)
		error = VCERR_UNSUPPORTED;
	if (error == 0)
		params->Written = item.u.Packet->Length;
	image_close_session(run, session, error);
	return error;
}

/**
 * image_run_decoder() - The still pipeline over a picture's bytes: into the caller's picture, or into its own frame.
 * @run:     the call
 * @source:  the picture's bytes
 * @info:    their header's facts
 * @format:  what the pipeline writes
 * @out:     the caller's picture (still_direct() said so), or NULL for the pipeline's own frame
 * @session: where the decoder's session goes; the caller closes it, after using its frame
 * @item:    where the picture's item goes
 *
 * Return: 0 or the error.
 */
static LONG image_run_decoder(struct image_run *run, const struct VCImageSide *source, const struct image_facts *info, ULONG format,
                              APTR out, struct vc_session **session, struct VCItem *item)
{
	struct VCSessionParams params;
	image_params(run, &params, VCK_Decoder, info->codec);
	params.OutputFormat = format;
	params.InputCount = 0; /* its own packets, as many as the decoder wants */
	if (out != NULL)
	{
		params.Width = info->width; /* the caller's picture is the pipeline's output */
		params.Height = info->height;
	}
	LONG error = image_open_session(run, &params, session);
	if (error == 0 && out != NULL)
		error = image_attach(run, *session, NULL, out, 0);
	struct image_feed feed = {.data = source->Data, .length = source->Length};
	if (still_strips(info))
	{
		feed.skip_from = info->skip_from;
		feed.skip_to = info->skip_to;
	}
	if (error == 0)
		error = image_wait(run, *session, &feed, item);
	return error;
}

/**
 * image_decode() - One still picture into the caller's picture: the pipeline, straight or through the converter.
 * @run:    the call
 * @params: the call's parameters
 *
 * Return: 0, VCERR_STREAM for bytes that are no picture this knows or a
 * picture cut short, VCERR_UNSUPPORTED for a picture the pipeline does not
 * decode right or past one transfer, VCERR_DEVMEM past the codec heap, or
 * the error.
 */
static LONG image_decode(struct image_run *run, const struct VCImageParams *params)
{
	struct image_facts info;
	LONG error = image_check(&params->Source, params->Codec, &info);
	if (error != 0)
		return error;
	struct VCImageSide dest = image_dest(&params->Dest, &info);
	BOOL direct = still_direct(&info, &dest, params->KeepAspect);
	ULONG format = direct ? dest.Format : still_between(&info);
	error = still_fits(run->base, &info, &dest, direct, params->KeepAspect);
	if (error != 0)
		return error;

	struct vc_session *session;
	struct VCItem item;
	LONG decoded = image_run_decoder(run, &params->Source, &info, format, direct ? dest.Data : NULL, &session, &item);
	still_frame_release(run->base); /* a kept pipeline raises no format change, and a failure may come before it */
	error = decoded;
	if (error == 0 && !direct)
	{
		struct VCImageSide side = image_frame_side(item.u.Frame); /* the pipeline's own: valid while its session is open */
		error = image_convert(run, &side, &dest, params->KeepAspect);
	}
	image_close_session(run, session, decoded); /* the decoder did its work even if the converter did not */
	return error;
}

/**
 * VCB_Image() - One picture decoded, encoded or converted, blocking (LVO).
 * @params: the call (A0): its kind, codec, both sides; Written set for an encoder
 * @base:   the library base (A6)
 *
 * A decoder takes the still pictures (JPEG, PNG, GIF, BMP, TGA, PPM), an
 * encoder makes a JPEG, a converter takes VCC_Raw (the codec table's
 * VCCF_Image rows).  The caller's task waits: first its turn, however long
 * the calls before it take, then one of its signal bits for the sessions, a
 * timer for the timeout.
 *
 * Return: 0 or a VCERR_*.
 */
LONG VCB_Image(struct VCImageParams *params asm("a0"), struct VideoCoreBase *base asm("a6"))
{
	struct ExecBase *SysBase = base->SysBase;
	if (params == NULL || !VC_HAS_FIELD(params->Size, struct VCImageParams, Written) || params->Source.Data == NULL ||
	    params->Dest.Data == NULL || (params->Kind == VCK_Encoder && params->Dest.Length == 0))
		return VCERR_BADPARAM;
	params->Written = 0;
	if (!codec_image_allowed(params->Kind, params->Codec))
		return VCERR_UNSUPPORTED;

	BYTE bit = AllocSignal(-1);
	if (bit < 0)
		return VCERR_NOMEM;
	struct image_run run = {.base = base,
	                        .task = FindTask(NULL),
	                        .sigmask = 1UL << bit,
	                        .timeout_ms = params->TimeoutMs != 0 ? params->TimeoutMs : IMAGE_TIMEOUT_MS};
	LONG error = VCERR_NOMEM;
	if (drv_timer_open(&run.timer, SysBase))
	{
		ObtainSemaphore(&base->image_lock);
		PERF_T0(t0);
		error = params->Kind == VCK_Decoder ? image_decode(&run, params)
		        : params->Kind == VCK_Encoder ? image_encode(&run, params)
		                                      : image_convert(&run, &params->Source, &params->Dest, params->KeepAspect);
		PERF_ADD(&base->perf, VCPERF_IMG_CALL, t0);
		perf_report(&base->perf); /* the call's own lines (profile tier) */
		ReleaseSemaphore(&base->image_lock);
		drv_timer_close(&run.timer);
	}
	FreeSignal(bit);
	return error;
}
