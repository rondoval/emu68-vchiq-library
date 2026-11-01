/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * session_still.c - the still kind: the image decoder's pipeline, one
 * picture a session, for the image calls (videocore_image.c); and what the
 * image calls ask of the pipeline before they open one.
 *
 * The image decoder takes JPEG, PNG, GIF, BMP, TGA and PPM and joins the
 * pieces its input packets bring.  Alone it writes only its own format (I420
 * for a JPEG, RGB565 for a GIF, RGBA for the rest), whatever its port reports
 * after a commit, so it runs behind the firmware's own pipeline with
 * video_convert, which writes I420, RGB565 or RGBA from any of them.
 *
 * Its output starts at the port's default size (a kept pipeline's: its last
 * picture's).  FORMAT_CHANGED brings the picture's size, caught by the token
 * and applied with the decoder's shape machinery (session_decoder.c), and
 * the picture comes back with the end mark.  A port that already has the
 * picture's size raises no FORMAT_CHANGED and fills the first buffer: the
 * token, which is then the picture (still_token_returned()), or the caller's
 * own picture when the call decodes straight into it (still_start()).
 *
 * Only an image call opens one, after it checked the picture's whole header
 * (still_decodes()) and chose an output the pipeline writes (still_direct()).
 */
#include <videocore_private.h>
#include <session_internal.h>
#include <image_info.h>

#define STILL_PIPELINE "aggregator.pipeline:ril.image_decode:video_convert"
#define STILL_COUNT_DEFAULT 3           /* the input packets the decoder recommends */
#define STILL_HEAP_MARGIN (16u << 20)   /* codec heap left free beyond a still's pictures */
#define STILL_FRAMES 3u                 /* the most frames a call takes of the RAM the VPU reaches */

/* What the pipeline writes (video_convert writes no BGRA) */
static const ULONG still_formats[] = {VCF_I420, VCF_RGB16, VCF_RGBA32};

/* ---- what the pipeline does (the image calls ask) ---------------------------------- */

/**
 * still_decodes() - Whether the pipeline decodes a picture right, by its header.
 * @info: the header's facts
 *
 * What it decoded right on the hardware, each refusal with what it did
 * instead; what was not tried is refused too.
 *
 * Return: TRUE if it may go to the pipeline.
 */
BOOL still_decodes(const struct image_facts *info)
{
	if (info->header - (info->skip_to - info->skip_from) > STILL_PACKET)
		return FALSE; /* an error event, whatever the packet size (a JPEG's frame header 90 KB in; 79 KB decoded) */
	if (info->codec == VCC_GIF)
		return info->transparency == VCTR_None; /* a transparent colour comes out opaque */
	if (info->bits != 8u)
		return FALSE; /* 16 bits: an error event; the rest not tried */
	switch (info->codec)
	{
	case VCC_JPEG:
		return info->coding == IMAGE_CODING_SEQUENTIAL && info->model != IMAGE_MODEL_CMYK; /* else an error event */
	case VCC_PNG:
		if (info->interlaced || info->model == IMAGE_MODEL_GREY)
			return FALSE; /* an error event */
		if (info->height % 16u != 0)
			return FALSE; /* the last rows written too high, rows lost */
		return info->model == IMAGE_MODEL_PALETTE || info->transparency != VCTR_Mask; /* RGB: the colour comes out opaque */
	case VCC_TGA:
		return info->coding == IMAGE_CODING_NONE && info->model == IMAGE_MODEL_RGB; /* run-length takes the firmware down */
	case VCC_BMP:
		return info->coding == IMAGE_CODING_NONE;
	case VCC_PPM:
		return TRUE;
	default:
		return FALSE;
	}
}

/**
 * still_own_format() - The image decoder's own picture of a codec, which the pipeline holds whatever it writes.
 * @codec: VCC_*
 *
 * Return: VCF_I420 for a JPEG, VCF_RGB16 for a GIF, VCF_RGBA32 for the rest.
 */
static ULONG still_own_format(ULONG codec)
{
	return codec == VCC_JPEG ? VCF_I420 : codec == VCC_GIF ? VCF_RGB16 : VCF_RGBA32;
}

/**
 * still_strips() - Whether a picture's metadata stays out of what the pipeline is fed.
 * @info: the header's facts
 *
 * Only when its header would not fit the first packet otherwise: the bytes
 * of any other picture go as they are.
 *
 * Return: TRUE to feed it without info->skip_from to info->skip_to.
 */
BOOL still_strips(const struct image_facts *info)
{
	return info->header > STILL_PACKET;
}

/**
 * still_writes() - Whether the pipeline writes a format.
 * @format: VCF_*
 *
 * Return: TRUE for I420, RGB565 and RGBA.
 */
static BOOL still_writes(ULONG format)
{
	for (u32 i = 0; i < ARRAY_SIZE(still_formats); i++)
		if (still_formats[i] == format)
			return TRUE;
	return FALSE;
}

/**
 * still_direct() - Whether the pipeline can write the caller's picture itself.
 * @info:        the picture's header facts
 * @dest:        the caller's picture, its size and format filled in
 * @keep_aspect: the caller's VCA_KeepAspect
 *
 * The picture's own size, the whole frame, a format the pipeline writes, and
 * the layout it writes: rows of whole 32 pixels, whole 16 rows, the natural
 * pitch.  It learns its layout only with the picture, too late to refuse.
 *
 * Return: TRUE if the caller's picture can be the pipeline's output.
 */
BOOL still_direct(const struct image_facts *info, const struct VCImageSide *dest, ULONG keep_aspect)
{
	if (dest->Width != info->width || dest->Height != info->height || dest->HaveRect || keep_aspect)
		return FALSE;
	u32 pitch = dest->Width * format_bytes_per_pixel(dest->Format);
	return still_writes(dest->Format) && dest->Width % 32u == 0 && dest->Height % 16u == 0 && (dest->Pitch == 0 || dest->Pitch == pitch);
}

/**
 * still_between() - What the pipeline writes for the converter after it.
 * @info: the picture's header facts
 *
 * A JPEG's own picture is I420 in rows of whole 32 pixels, whatever rows its
 * output port is given; the ISP reads I420 in rows of whole 64.  A JPEG
 * whose rows are not goes over as RGBA (the pipeline's rows of whole 16,
 * which the ISP reads), at more of the VPU's memory; every other picture goes
 * over as RGBA too.
 *
 * Return: VCF_I420 or VCF_RGBA32.
 */
ULONG still_between(const struct image_facts *info)
{
	if (info->codec == VCC_JPEG && ALIGN_UP(info->width, 32u) % isp_row_pixels(VCF_I420) == 0)
		return VCF_I420;
	return VCF_RGBA32;
}

/**
 * picture_bytes() - A picture's bytes in a format, as a frame of it holds them.
 * @format: VCF_*
 * @width:  pixels
 * @height: rows
 * @pitch:  bytes per row, 0 for the natural one
 *
 * Return: the bytes, 0 for a picture that cannot be laid out.
 */
static u64 picture_bytes(ULONG format, u32 width, u32 height, u32 pitch)
{
	struct vc_geometry geometry;
	return picture_geometry(format, width, height, pitch, &geometry) ? geometry.size : 0u;
}

/**
 * still_bounces() - Whether a converter writes the backend's own frame for the caller's picture.
 * @base:        the library base, the service open
 * @dest:        the caller's picture, its size and format filled in
 * @keep_aspect: the caller's VCA_KeepAspect
 *
 * A picture the VPU cannot write itself (off the cache lines, as
 * picture.datatype's are) would go by bulk, with its copy in the codec heap.
 * The backend's own frame costs one 68k copy instead (still_bounce_copy()).
 * Packed formats only, and the part written must be known: the bars of
 * VCA_KeepAspect are the converter's to place.
 *
 * Return: TRUE if the converter's output is the backend's own frame.
 */
BOOL still_bounces(struct VideoCoreBase *base, const struct VCImageSide *dest, ULONG keep_aspect)
{
	u32 size = (u32)picture_bytes(dest->Format, dest->Width, dest->Height, dest->Pitch);
	if (keep_aspect || size == 0 || format_bytes_per_pixel(dest->Format) == 1u)
		return FALSE;
	return mmal_importable(base, NULL, size, TRUE) && !mmal_importable(base, dest->Data, size, TRUE);
}

/**
 * still_bounce_copy() - The part of a converter's own frame it wrote, into the caller's picture (still_bounces()).
 * @frame: the frame, laid out as the caller's picture
 * @dest:  the caller's picture
 */
void still_bounce_copy(const struct VCFrame *frame, const struct VCImageSide *dest)
{
	u32 bytes = format_bytes_per_pixel(dest->Format);
	u32 pitch = dest->Pitch != 0 ? dest->Pitch : dest->Width * bytes;
	u32 x = dest->HaveRect ? (u32)dest->Rect.X : 0u, y = dest->HaveRect ? (u32)dest->Rect.Y : 0u;
	u32 width = dest->HaveRect ? (u32)dest->Rect.Width : dest->Width, rows = dest->HaveRect ? (u32)dest->Rect.Height : dest->Height;
	const UBYTE *from = (const UBYTE *)frame->Data + y * frame->Pitch[0] + x * bytes;
	UBYTE *to = (UBYTE *)dest->Data + y * pitch + x * bytes;
	for (; rows != 0; rows--, from += frame->Pitch[0], to += pitch)
		memcpy(to, from, width * bytes);
}

/**
 * still_frames_take() - Take a call's frames from the RAM the VPU reaches, or say they are not there.
 * @base:  the library base, the service open
 * @sizes: each frame's bytes, 0 for none, the pipeline's first
 *
 * The pipeline's stays taken (still_frame_release()): its real frame comes at
 * the format change, when the VPU holds part of the picture and a refusal
 * leaves it dead, and until then any task's AllocMem() can take what is free.
 * The others are only tried: they come after the decode, and a refusal then
 * costs no more than the decode.
 *
 * Return: TRUE if they all came.
 */
static BOOL still_frames_take(struct VideoCoreBase *base, const u32 sizes[STILL_FRAMES])
{
	struct dma_pool *pool = base->mmal.pool;
	APTR frames[STILL_FRAMES] = {NULL, NULL, NULL};
	BOOL fit = TRUE;
	for (u32 i = 0; i < STILL_FRAMES && fit; i++)
	{
		if (sizes[i] != 0)
			frames[i] = dma_alloc(pool, DMA_ALIGN_MIN, sizes[i]);
		fit = sizes[i] == 0 || frames[i] != NULL;
	}
	for (u32 i = fit ? 1u : 0u; i < STILL_FRAMES; i++)
		dma_free(pool, frames[i]);
	dma_pool_shrink(pool);
	if (fit)
		base->still_frame = frames[0];
	return fit;
}

/**
 * still_frame_release() - Give the frame still_fits() took for the pipeline back to the DMA pool.
 * @base: the library base, its image_lock held
 *
 * To the pool, not the system: it keeps the arena, and session_lock keeps the
 * other sessions off it, so a frame made now of no more than these bytes
 * cannot fail.
 */
void still_frame_release(struct VideoCoreBase *base)
{
	dma_free(base->mmal.pool, base->still_frame);
	base->still_frame = NULL;
}

/**
 * still_fits() - Whether a decode fits one transfer per picture, the VPU's codec heap and the RAM the VPU reaches.
 * @base:        the library base
 * @info:        the picture's header facts
 * @dest:        the caller's picture, its size and format filled in
 * @direct:      the pipeline writes @dest itself; else its own frame, from which
 *               the converter writes @dest
 * @keep_aspect: the caller's VCA_KeepAspect
 *
 * The pipeline holds the decoder's own picture (still_own_format()); and a
 * copy of each picture that crosses to the host unless it is imported
 * (mmal_zero_copy.c): the pipeline's output, through the converter that same
 * frame again as its input, and the converter's output.  What goes by bulk
 * must fit one transfer.  Refusing here costs milliseconds, a
 * converter that finds no memory after the decode costs the decode, and a
 * frame that cannot be had at the pipeline's format change costs the firmware
 * its MMAL.  Through the converter the backend takes the pipeline's frame, in
 * the rows the pipeline writes (still_direct()), the converter's input until
 * the pipeline's replaces it, and its own output when it bounces.  With 0
 * returned the pipeline's stays taken (still_frames_take()), for the caller
 * to give back.  The service is opened first: whether memory can be imported
 * is known only then.
 *
 * Components kept from the last picture count as used until they are let
 * go: that costs the next call their creation, so only a picture that would
 * not fit pays it.
 *
 * Return: 0, or VCERR_UNSUPPORTED (past one transfer), VCERR_DEVMEM (past
 * the codec heap), VCERR_NOMEM (past the RAM the VPU reaches), or the
 * service's error.
 */
LONG still_fits(struct VideoCoreBase *base, const struct image_facts *info, const struct VCImageSide *dest, BOOL direct, ULONG keep_aspect)
{
	ULONG format = direct ? dest->Format : still_between(info);
	u64 decoded = picture_bytes(format, info->width, info->height, 0);
	u64 converted = direct ? 0u : picture_bytes(dest->Format, dest->Width, dest->Height, dest->Pitch);
	LONG error = mmal_service_open(base);
	if (error != 0)
		return error;
	BOOL decoded_bulk = !mmal_importable(base, direct ? dest->Data : NULL, (u32)decoded, TRUE);
	BOOL converted_bulk = !direct && !still_bounces(base, dest, keep_aspect) && !mmal_importable(base, dest->Data, (u32)converted, TRUE);
	if ((decoded_bulk && decoded > STILL_MAX_BYTES) || (converted_bulk && converted > STILL_MAX_BYTES))
		return VCERR_UNSUPPORTED;
	const u32 own = direct ? 0u : (u32)picture_bytes(format, ALIGN_UP(info->width, 32u), ALIGN_UP(info->height, 16u), 0);
	const u32 frames[STILL_FRAMES] = {own, own, !direct && still_bounces(base, dest, keep_aspect) ? (u32)converted : 0u};
	u64 need = picture_bytes(still_own_format(info->codec), info->width, info->height, 0) + STILL_HEAP_MARGIN;
	if (decoded_bulk)
		need += direct ? decoded : 2u * decoded;
	if (converted_bulk)
		need += converted;
	ULONG total, free_bytes;
	if (gencmd_codec_memory(base, &total, &free_bytes) && need > free_bytes)
	{
		mmal_kept_release(base); /* the last picture's: only image calls keep any, and this is the one running */
		if (gencmd_codec_memory(base, &total, &free_bytes) && need > free_bytes)
			return VCERR_DEVMEM;
	}
	return still_frames_take(base, frames) ? 0 : VCERR_NOMEM;
}

/* ---- open and start ----------------------------------------------------------------- */

/**
 * still_formats_commit() - The pipeline's formats: the picture's codec in, the format asked for out.
 * @session: a still session, the pipeline created
 *
 * The output keeps the port's default size: with none it reports no buffer
 * size but refuses to run with less than the default picture's, and the
 * token is made by the size the commit reports.
 *
 * Return: 0, VCERR_UNSUPPORTED for an output format the port does not list, or the commit's error.
 */
static LONG still_formats_commit(struct vc_session *session)
{
	struct VideoCoreBase *base = session->base;
	struct mmal_port *input = session->decoder_in, *output = session->decoder_out;

	input->format.encoding = image_codec_encoding(session->params.Codec);
	input->format.encoding_variant = 0;
	LONG error = mmal_port_commit(base, input);
	if (error != 0)
		return error;

	ULONG encoding = format_encoding(param_output_format(&session->params));
	if (!mmal_port_supports(base, output, encoding))
		return VCERR_UNSUPPORTED;
	output->format.encoding = encoding;
	output->format.encoding_variant = encoding;
	return mmal_port_commit(base, output);
}

/**
 * still_packets() - The backend's own input packets: VCA_InputCount of VCA_InputSize bytes, or the decoder's own figures.
 * @session: a still session, the input format committed
 *
 * Return: 0 or VCERR_NOMEM.
 */
static LONG still_packets(struct vc_session *session)
{
	const struct VCSessionParams *params = &session->params;
	const struct mmal_port *input = session->decoder_in;
	u32 size = umax(input->buffer_size_min, params->InputSize != 0 ? params->InputSize : STILL_PACKET);
	u32 count = tag_count(params->InputCount, umax(input->buffer_num_recommended, STILL_COUNT_DEFAULT));
	return set_fill(session, &session->input_set, FALSE, count, size);
}

/**
 * still_open() - Open a still picture's decoder: the image decoder's pipeline, the picture at its own size.
 * @session: a still session, opened by an image call
 *
 * VCA_Width/Height, when given, are the picture's: the caller's picture is
 * imported as the output, laid out as the pipeline writes it (the image call
 * chose it so, still_direct()).  Without them the frame's size comes with
 * the picture.
 *
 * Return: 0, VCERR_BADPARAM for a layout shape_set_native() cannot make, or the error.
 */
static LONG still_open(struct vc_session *session)
{
	const struct VCSessionParams *params = &session->params;
	LONG error = component_open(session, STILL_PIPELINE, &session->decoder, &session->decoder_in, &session->decoder_out);
	if (error != 0)
		return error;
	session->input_port = session->decoder_in;
	session->output_port = session->decoder_out;
	error = still_formats_commit(session);
	if (error == 0)
		error = still_packets(session);
	u32 width = params->Width, height = params->Height;
	if (error != 0 || width == 0 || height == 0)
		return error;
	ULONG format = param_output_format(params);
	struct VCRect whole = rect_whole(width, height);
	return shape_set_native(session, format, width, height, width * format_bytes_per_pixel(format), &whole) ? 0 : VCERR_BADPARAM;
}

/**
 * caller_first() - Whether the first output is the caller's own picture, not the token.
 * @session: a still session, its output port's needs read
 *
 * A decode into the caller's picture (its layout fixed at open) sends that
 * picture out first, if it is as big as the port wants: an output port that
 * already has the picture's size (a kept pipeline) raises no FORMAT_CHANGED
 * and writes the first buffer, which is then the caller's.  A change of
 * size is applied as with the token.
 *
 * Return: TRUE to send the caller's picture first.
 */
static BOOL caller_first(struct vc_session *session)
{
	struct vc_set *set = &session->output_set;
	if (!set->imported || set->count == 0 || !session->shape.resolved)
		return FALSE;
	set_size_from_imports(set);
	return set->size >= session->decoder_out->buffer_size_min;
}

/**
 * still_start() - The first send: the input set attached, the ports running, the caller's picture or the token out.
 * @session: a still session, its input set final
 *
 * Return: 0 or the error.
 */
static LONG still_start(struct vc_session *session)
{
	struct VideoCoreBase *base = session->base;
	LONG error = decoder_input_start(session);
	if (error != 0)
		return error;
	if (!caller_first(session))
		return token_start(session);

	const struct vc_shape *shape = &session->shape;
	set_shape_frames(&session->output_set, &shape->geometry, shape->format, &shape->dest);
	error = set_attach(session, &session->output_set, NULL, session->decoder_out);
	if (error == 0)
		error = mmal_port_enable(base, session->decoder_out);
	if (error != 0)
		return error;
	session->output_state = OUTPUT_RUNNING;
	return set_send_output(session, &session->output_set);
}

/* ---- the shape ------------------------------------------------------------------------- */

/**
 * still_native_apply() - The picture's size on the pipeline's output: one frame of the format asked for, out.
 * @session: a still session, its stream's shape taken
 * @change:  the format change
 *
 * The change sized the decoder's own picture (I420), not the format asked
 * for: the frame takes the larger of the two.  The image decoder keeps its
 * own format where video_convert does not write the one asked for (a GIF's
 * RGB565): the commit reads it back, and the picture is refused.
 *
 * Return: 0 or the error.
 */
static LONG still_native_apply(struct vc_session *session, const struct mmal_format_change *change)
{
	ULONG picture = param_output_format(&session->params);
	u32 bytes_per_pixel = format_bytes_per_pixel(picture); /* 1 for I420: its luma */
	u32 size = umax(change->buffer_size_min, change->buffer_size_recommended);
	if (size == 0) /* a change that names no size: the picture's own */
		size = change->format.es.video.width * change->format.es.video.height * (bytes_per_pixel == 1 ? 3u : 2u * bytes_per_pixel) / 2u;
	u32 count = output_set_count(session, umax(change->buffer_num_min, 1u));

	LONG error = native_output_commit(session, change, picture, count, size);
	if (error != 0)
		return error;
	const struct mmal_port *port = session->decoder_out;
	if (port->format.encoding != format_encoding(picture))
	{
		Kprintf("[videocore] image decoder output: %08lx, not the %08lx asked for\n", port->format.encoding, format_encoding(picture));
		return VCERR_UNSUPPORTED;
	}
	size = umax(size, umax(port->buffer_size_min, session->shape.geometry.size));
	if (size > STILL_MAX_BYTES && !mmal_importable(session->base, NULL, size, TRUE))
	{
		Kprintf("[videocore] image: a %ld KiB picture, past one transfer's %ld KiB\n", size >> 10, (ULONG)(STILL_MAX_BYTES >> 10));
		return VCERR_UNSUPPORTED;
	}
	still_frame_release(session->base); /* for the frame made now, which no refusal may touch */
	return native_output_run(session, count, size);
}

/**
 * still_format_changed() - The pipeline told the picture's size.
 * @session: the session
 * @change:  the format change
 *
 * The pipeline tells the shape once for each stage: the same size again
 * changes nothing (remaking the output stalls it).  A change waits while
 * the caller holds the frame (output_change_release()).  An error fails
 * the session.
 */
static void still_format_changed(struct vc_session *session, const struct mmal_format_change *change)
{
	if (session->failed)
		return;
	if (session->have_stream && session->output_set.port != NULL &&
	    change->format.es.video.width == session->decoder_out->format.es.video.width &&
	    change->format.es.video.height == session->decoder_out->format.es.video.height)
		return;
	if (output_change_waits(session, change))
		return;
	LONG error = output_shape_begin(session, change, param_output_format(&session->params));
	if (error == 0)
		error = still_native_apply(session, change);
	if (error == 0)
		events_push(session, VCEV_StreamInfo, 0);
	if (error != 0)
		session_fail(session, error, "format");
}

/**
 * still_token_returned() - The picture came in the token: the token is the output frame.
 * @session: a still session, the token back with the picture
 * @object:  the token
 *
 * An output port that already has the picture's size raises no
 * FORMAT_CHANGED (a kept pipeline keeps its last picture's), and the
 * token, made to the port's size, takes the picture.  The stream's shape is
 * the port's own format; the token is described as a frame of it, for the
 * client, instead of being given up as a video's is.
 *
 * Return: TRUE with the token described; FALSE with the session failed.
 */
static BOOL still_token_returned(struct vc_session *session, struct vc_object *object)
{
	const struct mmal_port *port = session->decoder_out;
	if (session->output_set.imported)
	{
		/* the caller's picture was too small to go first (caller_first()) and the port's size was the picture's */
		session_fail(session, VCERR_BADSTATE, "the still came in the token, not in the caller's picture");
		return FALSE;
	}
	struct mmal_format_change change;
	format_change_from_port(port, &change);
	ULONG picture = param_output_format(&session->params);
	LONG error = stream_format_take(session, &change, picture);
	u32 width = port->format.es.video.width, height = port->format.es.video.height;
	if (error == 0 && !shape_set_native(session, picture, width, height, width * format_bytes_per_pixel(picture), &session->stream.Crop))
		error = VCERR_BADSTATE;
	if (error != 0)
	{
		session_fail(session, error, "a still in the token");
		return FALSE;
	}
	frame_set_geometry(object, &session->shape.geometry, picture, &session->shape.dest);
	Kprintf("[videocore] the picture came in the token: %ldx%ld\n", width, height);
	return TRUE;
}

/* ---- the kind -------------------------------------------------------------------------- */

/**
 * still_send_packet() - Send a piece of the picture.
 * @session: a still session, started
 * @object:  the packet's object, in the client's hands
 * @packet:  the packet
 *
 * The end mark goes with the last piece; the picture comes back with it.
 *
 * Return: 0 or the error.
 */
static LONG still_send_packet(struct vc_session *session, struct vc_object *object, struct VCPacket *packet)
{
	LONG error = object_send(session, object, packet->Length, packet_flags_to_mmal(packet->Flags), packet->Pts);
	if (error == 0 && (packet->Flags & VCPF_EndOfStream))
		session->end_sent = TRUE;
	return error;
}

/**
 * still_output_item() - The picture for the client.
 * @session: the session
 * @object:  the frame object, back from the VPU
 * @item:    the poll's item
 */
static void still_output_item(struct vc_session *session, struct vc_object *object, struct VCItem *item)
{
	frame_from_native(object, &session->shape.geometry);
	output_item_frame(object, item);
}

const struct session_ops still_ops = {
    .open = still_open,
    .start = still_start,
    .close = token_free,
    .send_packet = still_send_packet,
    .output_item = still_output_item,
    .token_returned = still_token_returned,
    .format_changed = still_format_changed,
    .frame_released = output_change_release,
};
