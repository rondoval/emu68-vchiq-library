/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * session_decoder.c - the decoder kind: ril.video_decode, alone (native
 * I420 frames) or tunnelled into ril.isp; and the shape machinery the
 * still kind shares (session_still.c).
 *
 * The decoder starts at a size no stream has, with one token object
 * attached to its output.  The stream's headers bring FORMAT_CHANGED with
 * the real shape; a frame in the token instead means the port's own format
 * is the shape (that picture is lost).  Either way the output then gets its
 * real set, or the tunnel and the ISP come up.  A later change of shape
 * waits until the client has given back every frame of the old one; inside
 * a running tunnel the ISP follows it unseen (isp_tunnel_prepare()).  An
 * H.264 stream's pixel aspect comes from its SPS (h264_sps.c): the decoder
 * reports square pixels whatever the stream says.
 */
#include <memory.h>

#include <videocore_private.h>
#include <session_internal.h>
#include <h264_sps.h>
#include <image_info.h>

#define INPUT_CHUNK_DEFAULT 65536u       /* the backend's own input packets */
#define INPUT_COUNT_DEFAULT 4
#define NATIVE_FRAMES_MIN 2              /* the backend's own native frames, within the decoder's needs */
#define NATIVE_FRAMES_MAX 6
#define GUESS_W 16                       /* a size no stream has, on purpose: the stream's headers raise FORMAT_CHANGED */
#define GUESS_H 16
#define BLIND_MEM_CHECK (4u << 20)       /* input without a picture size: read the codec heap */
#define BLIND_GIVE_UP (16u << 20)        /* ... and this much: the stream is refused */
#define DECODER_MEM_FLOOR (24u << 20)    /* a codec heap with less free than this cannot hold a decoder's frames */
#define DECODER_EXTRA_FRAMES 4u          /* frames a decoder holds beyond the stream's reference frames */
#define DECODER_BYTES_PER_MB 448u        /* of the codec heap, per macroblock of each (measured: 1.75 bytes per pixel) */
#define H264_MAX_MB_WIDTH 127            /* 2032 pixels: a wider picture decodes, its predicted pictures wrong */
#define H264_MAX_MACROBLOCKS 8192        /* level 4's frame size: the rows past it come out wrong */
#define MJPEG_MAX_WIDTH 4096u            /* mjpeg_size_ok() */
#define MJPEG_MAX_HEIGHT 1920u
#define H264_PROFILE_HIGH 100            /* profile_idc; the later ones allow 4:2:2, 4:4:4 or more than 8 bits */
#define H264_PROFILE_CAVLC444 44         /* CAVLC 4:4:4 Intra, numbered below High */

/* ---- what the decoder decodes right ------------------------------------------------ */

/*
 * Past these limits the VPU's H.264 decoder does not refuse: it gives wrong
 * pictures, or approximate ones (10-bit), or none (4:2:2), without a word.
 * The level the stream declares plays no part.
 */

/**
 * h264_size_ok() - Whether the decoder decodes an H.264 picture of a size right.
 * @crop: the visible picture (the frame in memory is wider: rows of whole 32 pixels)
 *
 * Return: TRUE within both the width and the macroblock limit, the coded picture's.
 */
static BOOL h264_size_ok(const struct VCRect *crop)
{
	u32 mb_width = (u32)(crop->X + crop->Width + 15) / 16, mb_height = (u32)(crop->Y + crop->Height + 15) / 16;
	return mb_width <= H264_MAX_MB_WIDTH && mb_width * mb_height <= H264_MAX_MACROBLOCKS;
}

/**
 * mjpeg_size_ok() - Whether the decoder decodes an MJPEG picture of its size right.
 * @data:   the picture
 * @length: its bytes
 *
 * Past MJPEG_MAX_WIDTH it gives a plain grey picture, past MJPEG_MAX_HEIGHT
 * first a row short and then no picture at all, all without a word.  A header
 * this cannot read is the VPU's to judge.
 *
 * Return: FALSE for a picture past either limit.
 */
static BOOL mjpeg_size_ok(const UBYTE *data, ULONG length)
{
	struct image_facts facts;
	if (image_info(data, length, VCC_JPEG, &facts) != 0)
		return TRUE;
	return facts.width <= MJPEG_MAX_WIDTH && facts.height <= MJPEG_MAX_HEIGHT;
}

/**
 * h264_profile_ok() - Whether the decoder decodes an H.264 profile right: 8-bit 4:2:0 only.
 * @profile: profile_idc, 0 for none known
 *
 * Return: TRUE for Baseline, Main, Extended and High, or none known.
 */
static BOOL h264_profile_ok(u32 profile)
{
	return profile <= H264_PROFILE_HIGH && profile != H264_PROFILE_CAVLC444;
}

/**
 * decoder_heap_check() - Whether the codec heap holds a decoder for a stream, by its SPS.
 * @session: a decoder session
 * @sps:     the SPS, read
 *
 * A decoder with no room says nothing: it takes its input and gives nothing,
 * so a client would wait for ever.  Its need was measured at 1080p, 720p and
 * 480p with 1 to 16 reference frames (docs/architecture.md section 9).
 * Components kept from the last picture go first if they are in the way.
 *
 * Return: 0, or VCERR_DEVMEM.
 */
static LONG decoder_heap_check(struct vc_session *session, const struct h264_sps *sps)
{
	if (session->sps_checked || sps->macroblocks == 0)
		return 0;
	session->sps_checked = TRUE;
	u64 need = (u64)(umin(sps->ref_frames, 16u) + DECODER_EXTRA_FRAMES) * sps->macroblocks * DECODER_BYTES_PER_MB;
	ULONG total, free_bytes;
	if (!gencmd_codec_memory(session->base, &total, &free_bytes) || need <= free_bytes)
		return 0;
	mmal_kept_release(session->base);
	if (!gencmd_codec_memory(session->base, &total, &free_bytes) || need <= free_bytes)
		return 0;
	Kprintf("[videocore] decoder: %ld reference frames of %ld macroblocks, %ld KiB of the codec heap free of %ld KiB wanted\n",
	        sps->ref_frames, sps->macroblocks, free_bytes >> 10, (ULONG)(need >> 10));
	return VCERR_DEVMEM;
}

/* ---- the stream's shape (shared with the still) ------------------------------------- */

/**
 * format_change_size() - The decoder output's buffer size for a format change.
 * @change: the format change
 *
 * Return: the larger of the minimum and the recommended size.
 */
static u32 format_change_size(const struct mmal_format_change *change)
{
	return umax(change->buffer_size_min, change->buffer_size_recommended);
}

/**
 * format_change_from_port() - A format change as a port's own format and needs.
 * @port:   the port, no FORMAT_CHANGED came for it
 * @change: where the change goes
 */
void format_change_from_port(const struct mmal_port *port, struct mmal_format_change *change)
{
	change->buffer_size_min = port->buffer_size_min;
	change->buffer_num_min = port->buffer_num_min;
	change->buffer_size_recommended = port->buffer_size_recommended;
	change->buffer_num_recommended = port->buffer_num_recommended;
	change->format = port->format;
}

/**
 * shape_set_native() - The decoder's own output: a picture of a format and the visible part of it.
 * @session: an untunnelled decoder or still session
 * @format:  VCF_*: I420 for video, the format asked for of a still picture
 * @width:   the frame's pixels per row
 * @height:  its rows
 * @pitch:   its (first plane's) bytes per row, 0 for the natural one
 * @dest:    the visible picture in the frame
 *
 * Return: TRUE, FALSE for a size picture_geometry() refuses.
 */
BOOL shape_set_native(struct vc_session *session, ULONG format, u32 width, u32 height, u32 pitch, const struct VCRect *dest)
{
	struct vc_shape *shape = &session->shape;
	if (!picture_geometry(format, width, height, pitch, FALSE, &shape->geometry))
		return FALSE;
	shape->format = format;
	shape->dest = *dest;
	shape->resolved = TRUE;
	return TRUE;
}

/**
 * stream_format_take() - The decoder's output takes the stream's format; the session learns its shape.
 * @session: the session
 * @change:  the format change
 * @picture: VCF_* the output is a picture of: I420 for video (passed down a tunnel as itself), a still's format asked for
 *
 * Return: 0, or VCERR_STREAM for an empty picture or one the decoder gets wrong.
 */
LONG stream_format_take(struct vc_session *session, const struct mmal_format_change *change, ULONG picture)
{
	struct mmal_format *format = &session->decoder_out->format;
	*format = change->format;
	format->encoding = format_encoding(picture);
	format->encoding_variant = format->encoding;
	if (session->tunnelled)
		format->encoding = MMAL_ENCODING_OPAQUE;
	if (format->es.video.width == 0 || format->es.video.height == 0)
		return VCERR_STREAM;

	/* the client's view: the frame in memory, the visible part of it (all of it when the change names none) */
	struct VCStreamInfo *stream = &session->stream;
	memset(stream, 0, sizeof(*stream));
	stream->Width = format->es.video.width;
	stream->Height = format->es.video.height;
	stream->Crop.X = format->es.video.crop.x;
	stream->Crop.Y = format->es.video.crop.y;
	stream->Crop.Width = format->es.video.crop.width;
	stream->Crop.Height = format->es.video.crop.height;
	if (stream->Crop.Width <= 0 || stream->Crop.Height <= 0)
		stream->Crop = rect_whole(stream->Width, stream->Height);
	if (session->params.Codec == VCC_H264 && !h264_size_ok(&stream->Crop))
	{
		Kprintf("[videocore] stream: %ldx%ld, past the decoder's %ld macroblocks' width or %ld macroblocks\n", stream->Crop.Width,
		        stream->Crop.Height, (ULONG)H264_MAX_MB_WIDTH, (ULONG)H264_MAX_MACROBLOCKS);
		return VCERR_STREAM;
	}
	stream->FrameRate.Num = format->es.video.frame_rate.num;
	stream->FrameRate.Den = format->es.video.frame_rate.den;
	stream->Par.Num = format->es.video.par.num;
	stream->Par.Den = format->es.video.par.den;
	if (session->sps_par.Num != 0)
		stream->Par = session->sps_par; /* the decoder says 1/1 whatever the stream says */
	stream->ColorSpace = format->es.video.color_space;
	stream->Format = session->tunnelled ? param_output_format(&session->params) : picture;
	session->have_stream = TRUE;
	Kprintf("[videocore] stream: %ldx%ld (%ldx%ld in memory), %ld/%ld fps\n", stream->Crop.Width, stream->Crop.Height, stream->Width,
	        stream->Height, stream->FrameRate.Num, stream->FrameRate.Den);
	return 0;
}

/**
 * output_shape_begin() - The start of a new shape: the output stopped and bare, the token gone, the stream's shape taken.
 * @session: a decoder or still session, its tunnel (if any) not running
 * @change:  the format change
 * @picture: VCF_* the output is a picture of (stream_format_take())
 *
 * The output port stops and its objects are detached: the token, freed now
 * (the shape is known: its work is done), or the last set, which the new
 * shape reuses or remakes.
 *
 * Return: 0 or the error.
 */
LONG output_shape_begin(struct vc_session *session, const struct mmal_format_change *change, ULONG picture)
{
	LONG error = set_detach(session, session->decoder_out);
	if (error != 0)
		return error;
	session->output_state = OUTPUT_DOWN;
	token_free(session);
	return stream_format_take(session, change, picture);
}

/**
 * native_output_commit() - Commit the decoder's output for a picture format and buffer shape.
 * @session: an untunnelled decoder or still session, its stream's shape taken
 * @change:  the format change
 * @picture: VCF_* of the frames
 * @count:   the buffers
 * @size:    the bytes of each
 *
 * The frames' layout follows the VPU's answer: rows of the VPU's width,
 * which gives the pitch.
 *
 * Return: 0, VCERR_BADSTATE for a layout it cannot make, or the commit's error.
 */
LONG native_output_commit(struct vc_session *session, const struct mmal_format_change *change, ULONG picture, u32 count, u32 size)
{
	(void)change;
	struct mmal_port *port = session->decoder_out;
	port->buffer_num = count;
	port->buffer_size = size;
	LONG error = mmal_port_commit(session->base, port);
	if (error != 0)
	{
		Kprintf("[videocore] commit decoder output (%ld x %ld): %ld\n", count, size, error);
		return error;
	}
	u32 width = port->format.es.video.width, height = port->format.es.video.height;
	return shape_set_native(session, picture, width, height, width * format_bytes_per_pixel(picture), &session->stream.Crop)
	           ? 0
	           : VCERR_BADSTATE;
}

/**
 * native_output_run() - The output frames made, attached and sent: the native output runs.
 * @session: an untunnelled decoder or still session, its output committed
 * @count:   the backend's own frames, if the client imported none
 * @size:    the bytes of each, which imported ones must hold
 *
 * Return: 0 or the error.
 */
LONG native_output_run(struct vc_session *session, u32 count, u32 size)
{
	struct mmal_port *port = session->decoder_out;
	LONG error = output_set_prepare(session, count, size);
	if (error == 0)
		error = set_attach(session, &session->output_set, NULL, port);
	if (error == 0)
		error = mmal_port_enable(session->base, port);
	if (error != 0)
		return error;
	session->output_port = port;
	session->output_state = OUTPUT_RUNNING;
	Kprintf("[videocore] decoder output: %ld buffers of %ld bytes, %ldx%ld\n", count, size, session->shape.geometry.width,
	        session->shape.geometry.height);
	return set_send_output(session, &session->output_set);
}

/**
 * output_change_waits() - Whether a new shape has to wait for the client's frames.
 * @session: a decoder or still session
 * @change:  the format change
 *
 * Return: TRUE with the change kept and the output OUTPUT_CHANGE_WAITING
 * (output_change_release() applies it once the frames are home); FALSE to
 * apply it now.
 */
BOOL output_change_waits(struct vc_session *session, const struct mmal_format_change *change)
{
	if (session->output_set.port == NULL || !set_at_client(&session->output_set))
		return FALSE;
	session->format_change = *change; /* a copy: @change lives in an event buffer or on the stack */
	session->output_state = OUTPUT_CHANGE_WAITING;
	return TRUE;
}

/**
 * output_change_release() - A frame came home: apply a waiting shape once all are.
 * @session: a decoder or still session
 */
void output_change_release(struct vc_session *session)
{
	if (session->failed || session->output_state != OUTPUT_CHANGE_WAITING || set_at_client(&session->output_set))
		return;
	session->output_state = OUTPUT_RUNNING; /* the frames are home: the port still runs until the change stops it */
	session->ops->format_changed(session, &session->format_change);
}

/* ---- the token ---------------------------------------------------------------------- */

/**
 * token_start() - The decoder output's token: one object of the backend's, attached, the port running, the token out.
 * @session: a decoder or still session, its output port committed
 *
 * Sized for the guessed picture, or the port's minimum if that is larger:
 * a guess that was right lands its first picture in it.
 *
 * Return: 0, VCERR_NOMEM, or the attach's, enable's or send's error.
 */
LONG token_start(struct vc_session *session)
{
	struct VideoCoreBase *base = session->base;
	struct mmal_port *port = session->decoder_out;
	u32 size = umax(port->buffer_size_min, GUESS_W * GUESS_H * 3u / 2u);
	session->token = object_new(session, TRUE, NULL, size);
	if (session->token == NULL)
		return VCERR_NOMEM;

	struct mmal_buffer *buffer = &session->token->buffer;
	LONG error = mmal_port_attach(base, port, &buffer, 1, size);
	if (error == 0)
		error = mmal_port_enable(base, port);
	if (error == 0)
		error = object_send(session, session->token, 0, 0, MMAL_TIME_UNKNOWN);
	return error;
}

/**
 * token_free() - Free the token, if there is one.
 * @session: a decoder or still session, the token detached
 */
void token_free(struct vc_session *session)
{
	if (session->token != NULL)
		object_free(session, session->token);
	session->token = NULL;
}

/**
 * decoder_input_start() - The input set attached, the component and its input running, the codec configuration sent.
 * @session: a decoder or still session, its input set final
 *
 * The input is not checked against the port's minimum: the decoder joins
 * packets of any size the client chose.  A decoder's codec configuration
 * (session->config) is attached beside the set and goes first, flagged as
 * configuration, ahead of the client's first packet.
 *
 * Return: 0 or the error.
 */
LONG decoder_input_start(struct vc_session *session)
{
	set_size_from_imports(&session->input_set);
	LONG error = set_attach(session, &session->input_set, session->config, session->decoder_in);
	if (error == 0)
		error = component_run(session, session->decoder, session->decoder_in, NULL);
	if (error == 0 && session->config != NULL)
		error = object_send(session, session->config, session->config->capacity, MMAL_BUFFER_HEADER_FLAG_CONFIG,
		                    MMAL_TIME_UNKNOWN);
	return error;
}

/* ---- the video decoder's shape ------------------------------------------------------- */

/**
 * decoder_native_apply() - The stream's shape on the decoder's native output: frames attached, port running, frames out.
 * @session: an untunnelled decoder session, its stream's shape taken
 * @change:  the format change
 *
 * The backend's own frames are made again for the new size; imported ones
 * must hold it.
 *
 * Return: 0 or the error.
 */
static LONG decoder_native_apply(struct vc_session *session, const struct mmal_format_change *change)
{
	u32 size = format_change_size(change);
	if (size == 0) /* a change that names no size: an I420 picture's own */
		size = change->format.es.video.width * change->format.es.video.height * 3u / 2u;
	u32 needed = umax(change->buffer_num_min, change->buffer_num_recommended);
	u32 count = output_set_count(session, clamp_val(needed, NATIVE_FRAMES_MIN, NATIVE_FRAMES_MAX));
	LONG error = native_output_commit(session, change, VCF_I420, count, size);
	if (error == 0)
		error = native_output_run(session, count, size);
	return error;
}

/**
 * decoder_format_apply() - The stream's shape for the first time, or a new one.
 * @session: a decoder session, its tunnel (if any) not running
 * @change:  the format change
 *
 * Native output gets its real set; a tunnel commits both ends
 * (isp_tunnel_prepare()).
 *
 * Return: 0 with VCEV_StreamInfo queued, or the error.
 */
static LONG decoder_format_apply(struct vc_session *session, const struct mmal_format_change *change)
{
	LONG error = output_shape_begin(session, change, VCF_I420);
	if (error == 0)
		error = session->tunnelled ? isp_tunnel_prepare(session, change) : decoder_native_apply(session, change);
	if (error == 0)
		events_push(session, VCEV_StreamInfo, 0);
	return error;
}

/**
 * decoder_format_changed() - The decoder told the stream's shape, or a new one.
 * @session: the session; a running tunnel hears of no change
 * @change:  the format change
 *
 * A change waits while the client holds frames of the old shape
 * (output_change_release() applies it once they are home).  An error fails
 * the session; a failed session takes no new shape.
 */
static void decoder_format_changed(struct vc_session *session, const struct mmal_format_change *change)
{
	if (session->failed || session->output_port != session->decoder_out)
		return; /* the ISP's output raises its own: the session set that format itself */
	if (output_change_waits(session, change))
		return;
	LONG error = decoder_format_apply(session, change);
	if (error != 0)
		session_fail(session, error, "format");
}

/**
 * decoder_token_returned() - The token brought a picture: the guessed size was right.
 * @session: a decoder session, its token back with data (that picture is lost)
 * @object:  the token
 *
 * No FORMAT_CHANGED comes: the output port's own format and needs are the
 * stream's shape.
 *
 * Return: FALSE: the token is not for the client.
 */
static BOOL decoder_token_returned(struct vc_session *session, struct vc_object *object)
{
	(void)object;
	struct mmal_format_change change;
	format_change_from_port(session->decoder_out, &change);
	decoder_format_changed(session, &change);
	return FALSE;
}

/* ---- open and start ---------------------------------------------------------------- */

/**
 * decoder_input_format() - The decoder's input port: the codec, a size no stream has, the headers.
 * @session:  the session, the decoder created
 * @encoding: the codec's MMAL_ENCODING_*
 *
 * Not VCA_Width/Height: a size that is the stream's raises no
 * FORMAT_CHANGED, and the first picture would land in the token.
 *
 * Return: 0 or the error.
 */
static LONG decoder_input_format(struct vc_session *session, ULONG encoding)
{
	const struct VCSessionParams *params = &session->params;
	struct mmal_format *format = &session->decoder_in->format;

	struct VCRect whole = rect_whole(GUESS_W, GUESS_H);
	video_format_set(format, encoding, GUESS_W, GUESS_H, &whole, &params->FrameRate);
	/* whole pictures per packet (MJPEG's always): no ends to look for */
	format->flags = params->Framed || params->Codec == VCC_MJPEG ? MMAL_ES_FORMAT_FLAG_FRAMED : 0;

	format->extradata_size = 0; /* the codec configuration goes as a buffer of its own (decoder_input_start()) */
	return mmal_port_commit(session->base, session->decoder_in);
}

/**
 * decoder_formats() - The decoder's formats: the input's codec and guessed size, the output's I420 picture.
 * @session:  the session, the decoder created
 * @encoding: the codec's MMAL_ENCODING_*
 *
 * The output's frames keep the times their packets brought: the decoder
 * makes none up for frames without one.
 *
 * Return: 0 or the error.
 */
static LONG decoder_formats(struct vc_session *session, ULONG encoding)
{
	LONG error = decoder_input_format(session, encoding);
	if (error != 0)
		return error;

	struct mmal_port *output = session->decoder_out;
	output->format.encoding = MMAL_ENCODING_I420; /* on the format the input's commit read back */
	output->format.encoding_variant = MMAL_ENCODING_I420;
	error = mmal_port_commit(session->base, output);
	if (error == 0)
		(void)mmal_port_param_set_u32(session->base, output, MMAL_PARAMETER_VIDEO_INTERPOLATE_TIMESTAMPS, 0);
	return error;
}

/**
 * decoder_packets() - The backend's own input packets: VCA_InputCount of VCA_InputSize bytes, or four of 64 KiB.
 * @session: the session, the decoder's input format committed
 *
 * A packet is never smaller than the decoder's minimum.
 *
 * Return: 0 or VCERR_NOMEM.
 */
static LONG decoder_packets(struct vc_session *session)
{
	const struct VCSessionParams *params = &session->params;
	u32 size = umax(session->decoder_in->buffer_size_min, params->InputSize != 0 ? params->InputSize : INPUT_CHUNK_DEFAULT);
	return set_fill(session, &session->input_set, FALSE, tag_count(params->InputCount, INPUT_COUNT_DEFAULT), size);
}

/**
 * decoder_config() - The codec configuration's own object: the caller's bytes, kept for the start.
 * @session: a decoder session at its open, params.Extradata still the caller's
 *
 * Any size goes: the configuration travels as a buffer of its own
 * (decoder_input_start()), not in the port's format, which holds at most
 * MMAL_EXTRADATA_MAX bytes.
 *
 * Return: 0 or VCERR_NOMEM.
 */
static LONG decoder_config(struct vc_session *session)
{
	const struct VCSessionParams *params = &session->params;
	if (params->Extradata == NULL || params->ExtradataSize == 0)
		return 0;
	session->config = object_new(session, FALSE, NULL, params->ExtradataSize);
	if (session->config == NULL)
		return VCERR_NOMEM;
	memcpy(session->config->buffer.data, params->Extradata, params->ExtradataSize);
	return 0;
}

/**
 * decoder_open() - Open a decoder: ril.video_decode, and ril.isp behind it if the output needs it.
 * @session: a decoder session
 *
 * The input set is the backend's own packets until the client imports.
 * With the ISP, a geometry fixed at open is final now: imports are checked
 * against it.  Without the ISP, VCA_Width/Height give the native frame a
 * native imported frame is checked against, until the stream's shape
 * replaces it.
 *
 * Return: 0 or the error.
 */
static LONG decoder_open(struct vc_session *session)
{
	const struct VCSessionParams *params = &session->params;
	ULONG encoding = video_codec_encoding(params->Codec);
	if (encoding == 0)
		return VCERR_UNSUPPORTED;
	if (params->Codec == VCC_H264)
	{
		struct h264_sps sps;
		h264_sps_read(params->Extradata, params->ExtradataSize, &sps);
		if (!h264_profile_ok(sps.profile))
			return VCERR_STREAM;
		session->sps_par = sps.par;
		LONG heap = decoder_heap_check(session, &sps);
		if (heap != 0)
			return heap;
	}

	LONG error = component_open(session, "ril.video_decode", &session->decoder, &session->decoder_in, &session->decoder_out);
	if (error != 0)
		return error;
	session->input_port = session->decoder_in;
	session->output_port = session->decoder_out; /* until a tunnel's ISP output takes over */
	error = decoder_formats(session, encoding);
	if (error == 0)
		error = decoder_packets(session);
	if (error == 0)
		error = decoder_config(session);
	if (error != 0)
		return error;

	session->tunnelled = param_wants_isp(params);
	if (session->tunnelled)
	{
		error = isp_open(session);
		if (error != 0)
			return error;
		return param_output_size_fixed(params) ? shape_resolve(session) : 0;
	}

	if (params->Width == 0 || params->Height == 0)
		return 0; /* the native frame's size comes with the stream */
	struct VCRect whole = rect_whole(params->Width, params->Height);
	return shape_set_native(session, VCF_I420, params->Width, params->Height, 0, &whole) ? 0 : VCERR_BADPARAM;
}

/**
 * decoder_start() - The first send: the input set attached, the ports running, the token out.
 * @session: a decoder session, its input set final
 *
 * Return: 0 or the error.
 */
static LONG decoder_start(struct vc_session *session)
{
	LONG error = decoder_input_start(session);
	if (error == 0)
		error = token_start(session);
	if (error != 0)
		return error;
	Kprintf("[videocore] decoder running: %ld input buffers of %ld bytes%s\n", session->input_set.count, session->input_set.size,
	        session->tunnelled ? ", ISP to follow" : "");
	return 0;
}

/* ---- input ---------------------------------------------------------------------------- */

/**
 * decoder_check_input() - Whether a decoder that has given no picture size yet should give up.
 * @session: a decoder session
 * @data:    the packet about to go
 * @length:  its bytes
 *
 * An H.264 stream past High profile is refused at its SPS; the SPS's pixel
 * aspect is kept (stream_format_take()).  Beyond that the VPU's decoder
 * says nothing when it cannot decode: a stream it does not take, or no room
 * in the codec heap for its frames (a second 1080p decoder at the Pi 4's
 * default gpu_mem).  It hands the input back undecoded, as fast as it
 * comes.  decoder_heap_check() catches what the SPS tells.  For the rest the
 * input is counted until the stream's shape is known.  Past BLIND_MEM_CHECK
 * bytes the codec heap is read once: nearly full means the
 * decoder starved.  Past BLIND_GIVE_UP bytes the stream is taken as refused.
 *
 * Return: 0 to go on; else VCERR_DEVMEM or VCERR_STREAM, the session failed.
 */
static LONG decoder_check_input(struct vc_session *session, const UBYTE *data, u32 length)
{
	if (session->have_stream)
		return 0;

	if (session->params.Codec == VCC_H264)
	{
		struct h264_sps sps;
		h264_sps_read(data, length, &sps);
		if (!h264_profile_ok(sps.profile))
		{
			session_fail(session, VCERR_STREAM, "past High profile: the decoder writes 8-bit 4:2:0 only");
			return VCERR_STREAM;
		}
		if (sps.profile != 0)
			session->sps_par = sps.par;
		LONG heap = decoder_heap_check(session, &sps);
		if (heap != 0)
		{
			session_fail(session, heap, "no room in the codec heap");
			return heap;
		}
	}
	session->blind_bytes += length;
	if (!session->memory_checked && session->blind_bytes >= BLIND_MEM_CHECK)
	{
		session->memory_checked = TRUE;
		ULONG total, free_bytes;
		if (gencmd_codec_memory(session->base, &total, &free_bytes))
		{
			Kprintf("[videocore] no picture size after %ld KiB of input; codec heap %ld KiB free of %ld KiB\n",
			        session->blind_bytes >> 10, free_bytes >> 10, total >> 10);
			if (free_bytes < DECODER_MEM_FLOOR)
			{
				session_fail(session, VCERR_DEVMEM, "no picture: the codec heap is full");
				return VCERR_DEVMEM;
			}
		}
	}
	if (session->blind_bytes < BLIND_GIVE_UP)
		return 0;
	session_fail(session, VCERR_STREAM, "no picture: the stream is refused");
	return VCERR_STREAM;
}

/**
 * decoder_send_packet() - Send a packet of the stream.
 * @session: a decoder session, started
 * @object:  the packet's object, in the client's hands
 * @packet:  the packet
 *
 * MJPEG goes one whole picture per packet.  The end mark of a stream that
 * never carried a byte ends it here: nothing went in, so nothing can come
 * out, and the decoder was seen never to answer that mark.  The mark still
 * goes to the VPU, to come back as any packet does.
 *
 * Return: 0 or the error: VCERR_STREAM for an MJPEG picture too large, which stays the client's.
 */
static LONG decoder_send_packet(struct vc_session *session, struct vc_object *object, struct VCPacket *packet)
{
	LONG error = decoder_check_input(session, packet->Data, packet->Length);
	if (error != 0)
		return error;
	ULONG flags = packet_flags_to_mmal(packet->Flags);
	if (session->params.Codec == VCC_MJPEG && packet->Length != 0)
	{
		if (!mjpeg_size_ok(packet->Data, packet->Length))
			return VCERR_STREAM;
		flags |= MMAL_BUFFER_HEADER_FLAG_FRAME_START | MMAL_BUFFER_HEADER_FLAG_FRAME_END;
	}
	error = object_send(session, object, packet->Length, flags, packet->Pts);
	if (error != 0 || !(packet->Flags & VCPF_EndOfStream))
		return error;
	if (!session->have_stream && session->blind_bytes == 0)
		events_push(session, VCEV_EndOfStream, 0); /* an empty stream; end_sent stays clear, so a late answer is ignored */
	else
		session->end_sent = TRUE;
	return 0;
}

/* ---- the kind -------------------------------------------------------------------------- */

/**
 * decoder_output_item() - A decoded frame for the client.
 * @session: the session
 * @object:  the frame object, back from the VPU
 * @item:    the poll's item
 *
 * A native frame is described as the VPU laid it out; the ISP's frames were
 * described when they were made.
 */
static void decoder_output_item(struct vc_session *session, struct vc_object *object, struct VCItem *item)
{
	if (!session->tunnelled)
		frame_from_native(object, &session->shape.geometry);
	output_item_frame(object, item);
}

/**
 * decoder_flush() - The decoder's ports in the pipeline's order.
 * @session: a decoder session, started
 *
 * The input, the tunnel's ISP end while it runs (the decoded pictures
 * between the decoder and the ISP), the output while it runs.  The ISP end
 * clears the tunnel; its decoder end never answers a flush, and the VPU's
 * MMAL service is gone.
 *
 * Return: 0 or the error.
 */
static LONG decoder_flush(struct vc_session *session)
{
	BOOL tunnel = session->tunnelled && output_running(session);
	struct mmal_port *const ports[] = {session->input_port, tunnel ? session->isp_in : NULL,
	                                   session->output_port->enabled ? session->output_port : NULL};
	return ports_flush(session, ports, ARRAY_SIZE(ports));
}

/**
 * decoder_set_attrs() - A decoder's running controls: the output geometry, with the ISP.
 * @session: the session
 * @changes: the parameters, Changed naming which
 *
 * Return: 0, VCERR_UNSUPPORTED for anything but the geometry (or without the ISP), or the error.
 */
static LONG decoder_set_attrs(struct vc_session *session, const struct VCSessionParams *changes)
{
	if (changes->Changed & VCSPF_OutputGeometry)
		return isp_geometry_change(session, changes);
	return VCERR_UNSUPPORTED;
}

/**
 * decoder_close() - What the decoder holds besides its sets: the token and the codec configuration.
 * @session: a decoder session, its components closed
 */
static void decoder_close(struct vc_session *session)
{
	token_free(session);
	if (session->config != NULL)
		object_free(session, session->config);
	session->config = NULL;
}

const struct session_ops decoder_ops = {
    .open = decoder_open,
    .start = decoder_start,
    .close = decoder_close,
    .send_packet = decoder_send_packet,
    .output_item = decoder_output_item,
    .token_returned = decoder_token_returned,
    .format_changed = decoder_format_changed,
    .frame_released = output_change_release,
    .output_start = isp_output_start,
    .flush = decoder_flush,
    .set_attrs = decoder_set_attrs,
};
