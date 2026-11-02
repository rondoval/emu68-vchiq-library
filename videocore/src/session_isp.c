/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * session_isp.c - ril.isp: the output shape, the ISP's output side, the
 * decoder-to-ISP tunnel, geometry changes, and the converter kind (ril.isp
 * alone, pictures in and pictures out).
 */
#include <memory.h>

#include <videocore_private.h>
#include <session_internal.h>

#define OUTPUT_COUNT_DEFAULT 3 /* the backend's own output frames, when the client asks for none */
#define TUNNEL_BUFFERS 3       /* decoded frames between the decoder and the ISP, unless the decoder needs more */

/* ---- the output shape ------------------------------------------------------------- */

/**
 * isp_row_pixels() - The ISP's rows of a format, read or written: whole groups of this many pixels.
 * @format: VCF_*
 *
 * Planar 4:2:0 rows are whole 64 bytes, so that the chroma rows, half as
 * long, are whole 32: a row of 4000 pixels reads as 4032.  Every other
 * format: whole 32 pixels (24-bit rows need it; 16- and 32-bit ones would
 * take 16).
 *
 * Return: the pixels.
 */
u32 isp_row_pixels(ULONG format)
{
	return format_is_planar(format) ? 64u : 32u;
}

/**
 * param_wants_isp() - Whether a decoder's output needs the ISP.
 * @params: the session's parameters
 *
 * Return: TRUE for anything but the decoder's native I420 picture: another
 * format, a size, a pitch, a place in the frame.
 */
BOOL param_wants_isp(const struct VCSessionParams *params)
{
	return param_output_format(params) != VCF_I420 || params->OutputWidth != 0 || params->OutputHeight != 0 || params->OutputPitch != 0 ||
	       params->HaveDestRect || params->KeepAspect;
}

/**
 * shape_dest() - Where the picture goes in an output frame.
 * @params:     the session's parameters
 * @source_w:   the picture's width as shown, its pixels' aspect taken in (0 while unknown)
 * @source_h:   its height
 * @width:      the frame's width
 * @height:     its height
 * @row_pixels: pixels per row of the frame's buffer
 * @dest:       where the rectangle goes
 *
 * The tags it takes, and what each combination gives:
 *
 *   neither              the whole frame: the picture stretched to fill it
 *   VCA_DestRect         the client's rectangle, inside the frame's buffer
 *                        (@row_pixels wide, so it may reach past @width into
 *                        the pitch)
 *   VCA_KeepAspect       the picture fitted inside the whole frame with its
 *                        aspect as shown kept, centred, size and position even
 *   both                 as VCA_KeepAspect: the rectangle is ignored
 *
 * While the picture's size is unknown, VCA_KeepAspect falls back to the
 * other two; the shape is resolved again when the ISP's output is
 * configured, once the stream's shape is known.
 *
 * Return: 0, or VCERR_BADPARAM for a rectangle outside the frame's buffer.
 */
static LONG shape_dest(const struct VCSessionParams *params, u32 source_w, u32 source_h, u32 width, u32 height, u32 row_pixels,
                       struct VCRect *dest)
{
	if (params->KeepAspect && source_w != 0 && source_h != 0)
	{
		u32 fit_w = width, fit_h = source_h * width / source_w;
		if (fit_h > height)
		{
			fit_h = height;
			fit_w = source_w * height / source_h;
		}
		fit_w &= ~1u;
		fit_h &= ~1u;
		dest->X = (LONG)(((width - fit_w) / 2u) & ~1u);
		dest->Y = (LONG)(((height - fit_h) / 2u) & ~1u);
		dest->Width = (LONG)fit_w;
		dest->Height = (LONG)fit_h;
		return 0;
	}
	if (params->HaveDestRect)
	{
		*dest = params->DestRect;
		return rect_inside(dest, row_pixels, height) ? 0 : VCERR_BADPARAM;
	}
	*dest = rect_whole(width, height);
	return 0;
}

/**
 * shown_width() - The picture's width as shown: its pixels' aspect taken in.
 * @stream: the stream's shape
 *
 * A 720x576 picture of 64:45 pixels shows 1024 wide (16:9).  The width
 * and the aspect's terms (16 bits in H.264) stay within 16 bits, so the
 * product does within 32; anything else counts as square pixels.
 *
 * Return: the visible width, as is for square pixels or none given.
 */
static u32 shown_width(const struct VCStreamInfo *stream)
{
	u32 width = (u32)stream->Crop.Width;
	if (stream->Par.Num <= 0 || stream->Par.Den <= 0 || stream->Par.Num > 0xffff || width > 0xffff)
		return width;
	return width * (u32)stream->Par.Num / (u32)stream->Par.Den;
}

/**
 * shape_resolve() - The output frames' shape: what the tags fixed, the stream's shape for the rest.
 * @session: the session
 *
 * Final only once the stream's shape is known, unless the tags give the size.
 *
 * Return: 0 with session->shape resolved; VCERR_BADSTATE while the size
 * waits for the stream; VCERR_UNSUPPORTED for an unknown format;
 * VCERR_BADPARAM for a geometry the ISP cannot write (rows of whole groups of
 * isp_row_pixels(), an even number of them).
 */
LONG shape_resolve(struct vc_session *session)
{
	const struct VCSessionParams *params = &session->params;
	ULONG format = param_output_format(params);
	u32 bytes_per_pixel = format_bytes_per_pixel(format);
	if (bytes_per_pixel == 0)
		return VCERR_UNSUPPORTED;

	u32 source_w = session->have_stream ? (u32)session->stream.Crop.Width : 0;
	u32 source_h = session->have_stream ? (u32)session->stream.Crop.Height : 0;
	u32 width = params->OutputWidth != 0 ? params->OutputWidth : source_w & ~1u;
	u32 height = params->OutputHeight != 0 ? params->OutputHeight : source_h & ~1u;
	if (width == 0 || height == 0)
		return VCERR_BADSTATE;

	u32 pitch = params->OutputPitch;
	if (pitch == 0)
		pitch = format_is_planar(format) ? ALIGN_UP(width, isp_row_pixels(format)) : width * bytes_per_pixel;

	u32 row_pixels = pitch / bytes_per_pixel; /* picture_geometry() refuses a pitch of part pixels */
	if (row_pixels < width || row_pixels % isp_row_pixels(format) != 0 || (height & 1u) != 0)
		return VCERR_BADPARAM;

	struct vc_geometry geometry;
	if (!picture_geometry(format, row_pixels, height, pitch, FALSE, &geometry))
		return VCERR_BADPARAM;

	struct VCRect dest;
	LONG error = shape_dest(params, session->have_stream ? shown_width(&session->stream) : 0, source_h, width, height, row_pixels, &dest);
	if (error != 0)
		return error;

	struct vc_shape *shape = &session->shape;
	shape->format = format;
	shape->dest = dest;
	shape->geometry = geometry;
	shape->resolved = TRUE;
	return 0;
}

/* ---- the ISP's output side ---------------------------------------------------------- */

/**
 * isp_open() - Create the ISP (or take a kept one) and check it writes the output format.
 * @session: the session, its output format in the parameters
 *
 * Return: 0, VCERR_UNSUPPORTED for a format this backend or the ISP does not write, or the create's error.
 */
LONG isp_open(struct vc_session *session)
{
	ULONG format = param_output_format(&session->params);
	if (format_bytes_per_pixel(format) == 0)
		return VCERR_UNSUPPORTED;
	LONG error = component_open(session, "ril.isp", &session->isp, &session->isp_in, &session->isp_out);
	if (error != 0)
		return error;
	return mmal_port_supports(session->base, session->isp_out, format_encoding(format)) ? 0 : VCERR_UNSUPPORTED;
}

/**
 * isp_output_configure() - The ISP's output side: the format, the frames, attached.
 * @session: the session, the ISP created
 *
 * Run when the ISP is set up, and again after a geometry change (the port
 * stopped, its frames detached and the backend's own gone).  Imported frames
 * must hold the geometry.
 *
 * Return: 0 or the error.
 */
static LONG isp_output_configure(struct vc_session *session)
{
	struct VideoCoreBase *base = session->base;
	const struct vc_shape *shape = &session->shape;
	const struct vc_geometry *geometry = &shape->geometry;

	LONG error = shape_resolve(session);
	if (error == 0)
		error = output_set_prepare(session, output_set_count(session, OUTPUT_COUNT_DEFAULT), geometry->size);
	if (error != 0)
		return error;

	struct mmal_port *port = session->isp_out;
	video_format_set(&port->format, format_encoding(shape->format), geometry->width, geometry->height, &shape->dest,
	                 &session->stream.FrameRate);
	(void)mmal_port_param_set_u32(base, port, MMAL_PARAMETER_NO_IMAGE_PADDING, 1);
	port->buffer_num = session->output_set.count;
	port->buffer_size = geometry->size;
	error = mmal_port_commit(base, port);
	if (error != 0)
	{
		Kprintf("[videocore] commit ISP output format %ld (%ldx%ld, pitch %ld): %ld\n", shape->format, geometry->width, geometry->height,
		        geometry->pitch[0], error);
		return error;
	}

	Kprintf("[videocore] ISP: format %ld, %ldx%ld at %ld,%ld in %ldx%ld rows of %ld bytes; buffers min %ld x %ld, %ld x %ld used\n",
	        shape->format, shape->dest.Width, shape->dest.Height, shape->dest.X, shape->dest.Y, geometry->width, geometry->height,
	        geometry->pitch[0], port->buffer_num_min, port->buffer_size_min, session->output_set.count, geometry->size);

	if (port->buffer_size_min > geometry->size)
	{
		Kprintf("[videocore] the ISP wants %ld bytes per frame, the frames have %ld\n", port->buffer_size_min, geometry->size);
		return VCERR_BADPARAM;
	}
	return set_attach(session, &session->output_set, NULL, port);
}

/**
 * isp_tunnel_prepare() - A tunnel's ends take the stream's first shape; the ISP's output follows.
 * @session: a tunnelled decoder session, the tunnel not running yet, the stream's shape taken
 * @change:  the format change
 *
 * The tunnel passes the decoder's own pictures (opaque), each describing
 * itself: a later change of shape goes down the running tunnel with them
 * and the ISP follows it, so the host hears of none and sets nothing again.
 * (With I420 between them the ISP keeps the first shape: it gives back
 * smaller pictures empty and garbles bigger ones.)  The tunnel holds
 * TUNNEL_BUFFERS pictures, or the decoder's minimum if that is more; the
 * size of one comes back from the opaque format's commit.
 *
 * The ISP's output starts now if its geometry was fixed at open, else at the
 * poll after the client heard the stream's shape (and set its geometry and
 * imported its frames).
 *
 * Return: 0 or the error.
 */
LONG isp_tunnel_prepare(struct vc_session *session, const struct mmal_format_change *change)
{
	struct VideoCoreBase *base = session->base;
	struct mmal_port *out = session->decoder_out;

	LONG error = mmal_port_commit(base, out);
	if (error == 0)
	{
		out->buffer_num = umax(umax(change->buffer_num_min, out->buffer_num_min), TUNNEL_BUFFERS);
		out->buffer_size = out->buffer_size_min; /* an opaque picture's, from the commit */
		error = mmal_port_commit(base, out);
	}
	if (error == 0)
	{
		session->isp_in->format = out->format; /* as the commit read it back */
		error = mmal_port_commit(base, session->isp_in);
	}
	if (error != 0)
		return error;
	session->output_state = OUTPUT_START_PENDING;
	return param_output_size_fixed(&session->params) ? isp_output_start(session) : 0;
}

/**
 * isp_tunnel_start() - Connect the decoder to the ISP and run everything, the ISP's output first.
 * @session: a tunnelled decoder session, the ISP's output configured
 *
 * The decoder gives its first picture as soon as its end runs.  The ISP's
 * output has to be running with its frames out by then: with the input end
 * first, about every second start lost that picture and gave the next one
 * half a second late.
 *
 * Return: 0 with the output frames sent, or the error.
 */
static LONG isp_tunnel_start(struct vc_session *session)
{
	struct VideoCoreBase *base = session->base;
	LONG error = mmal_port_connect(base, session->decoder_out, session->isp_in);
	if (error == 0)
		error = component_run(session, session->isp, NULL, session->isp_out);
	if (error != 0)
		return error;
	session->output_port = session->isp_out;
	session->output_state = OUTPUT_RUNNING;
	error = set_send_output(session, &session->output_set);
	if (error == 0)
		error = mmal_port_enable(base, session->isp_in); /* the VPU enables the decoder's end with the input */
	if (error == 0)
		error = mmal_port_enable(base, session->decoder_out); /* a no-op: its state was read back */
	return error;
}

/**
 * isp_output_start() - Make the ISP's output side and run it.
 * @session: a tunnelled decoder session, or a converter after a geometry change (OUTPUT_START_PENDING)
 *
 * At a decoder's first start the whole tunnel comes up; after a geometry
 * change the output port alone restarts.  With an imported set that is
 * still empty (the client imports again after a change) nothing happens yet.
 *
 * Return: 0 or the error.
 */
LONG isp_output_start(struct vc_session *session)
{
	struct VideoCoreBase *base = session->base;

	if (!session->shape.resolved)
	{
		LONG error = shape_resolve(session);
		if (error != 0)
			return error;
	}
	if (session->output_set.imported && session->output_set.count == 0)
		return 0;

	LONG error = isp_output_configure(session);
	if (error != 0)
		return error;

	if (session->tunnelled && session->isp_in->tunnel == NULL)
		return isp_tunnel_start(session);

	error = mmal_port_enable(base, session->isp_out);
	if (error != 0)
		return error;
	session->output_state = OUTPUT_RUNNING;
	return set_send_output(session, &session->output_set);
}

/**
 * isp_output_restart() - Stop the ISP's output and drop its frames: the next poll makes them again.
 * @session: a session with a running ISP output
 *
 * The client's pointers to the frames go with them.
 *
 * Return: 0 or the error.
 */
static LONG isp_output_restart(struct vc_session *session)
{
	LONG error = set_detach(session, session->isp_out);
	if (error != 0)
		return error;

	set_empty(session, &session->output_set);
	session->shape.resolved = FALSE;
	session->output_state = OUTPUT_START_PENDING;
	return shape_resolve(session);
}

/**
 * same_output_request() - Whether two parameter sets ask for the same output.
 * @first:  one
 * @second: the other
 *
 * Return: TRUE if format, size, pitch and placement all match.
 */
static BOOL same_output_request(const struct VCSessionParams *first, const struct VCSessionParams *second)
{
	return first->OutputFormat == second->OutputFormat && first->OutputWidth == second->OutputWidth &&
	       first->OutputHeight == second->OutputHeight && first->OutputPitch == second->OutputPitch &&
	       first->HaveDestRect == second->HaveDestRect && first->KeepAspect == second->KeepAspect &&
	       rect_equal(&first->DestRect, &second->DestRect);
}

/**
 * isp_set_geometry() - A new output geometry for the ISP.
 * @session: a session with the ISP
 * @changes: the output tags the client set
 *
 * Nothing new: the frames stay.  Before the ISP's output runs the shape is
 * only resolved again (a size given now lets frames be imported already).
 * While it runs, the output is restarted at the next poll
 * (isp_output_restart()).
 *
 * Return: 0 or the error.
 */
static LONG isp_set_geometry(struct vc_session *session, const struct VCSessionParams *changes)
{
	struct VCSessionParams *params = &session->params;
	struct VCSessionParams before = *params;
	if (changes->OutputFormat != 0)
		params->OutputFormat = changes->OutputFormat;
	if (changes->OutputWidth != 0)
		params->OutputWidth = changes->OutputWidth;
	if (changes->OutputHeight != 0)
		params->OutputHeight = changes->OutputHeight;
	if (changes->OutputPitch != 0)
		params->OutputPitch = changes->OutputPitch;
	else if (changes->OutputWidth != 0 || changes->OutputFormat != 0)
		params->OutputPitch = 0; /* a new width: the natural pitch again unless told */
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

	if (same_output_request(&before, params))
		return 0;

	session->shape.resolved = FALSE;
	if (output_running(session))
		return isp_output_restart(session);
	if (session->have_stream || param_output_size_fixed(params))
		return shape_resolve(session);
	return 0;
}

/**
 * isp_geometry_change() - A new output geometry for a session with the ISP (VC_SetAttrs()).
 * @session: the session
 * @changes: the output tags the client set
 *
 * A failure ends the session: its parameters have changed already.
 *
 * Return: 0, VCERR_UNSUPPORTED without the ISP (an encoder, or a decoder
 * that gives its native frames), or the error.
 */
LONG isp_geometry_change(struct vc_session *session, const struct VCSessionParams *changes)
{
	if (session->isp == NULL)
		return VCERR_UNSUPPORTED;
	LONG error = isp_set_geometry(session, changes);
	if (error != 0)
		session_fail(session, error, "geometry change");
	return error;
}

/* ---- the converter kind -------------------------------------------------------------- */

/**
 * converter_source() - The part of the input picture a converter reads (VCA_SourceRect).
 * @session: a converter session, its ISP created
 * @source:  where the rectangle goes: the whole picture unless the client named one
 *
 * The rectangle goes to the ISP's input as the firmware's crop parameter;
 * a kept ISP (an image call's) holds its last call's, so it gets the whole
 * picture's when the client names none.
 *
 * Return: 0, VCERR_BADPARAM for a rectangle outside the picture, or the error.
 */
static LONG converter_source(struct vc_session *session, struct VCRect *source)
{
	const struct VCSessionParams *params = &session->params;

	*source = rect_whole(params->Width, params->Height);
	if (!params->HaveSourceRect && !session->isp->reused)
		return 0;

	const struct VCRect *rect = params->HaveSourceRect ? &params->SourceRect : source;
	if (!rect_inside(rect, params->Width, params->Height))
		return VCERR_BADPARAM;

	const u32 crop[4] = {(u32)rect->X, (u32)rect->Y, (u32)rect->Width, (u32)rect->Height};
	LONG error = mmal_port_param_set(session->base, session->isp_in, MMAL_PARAMETER_CROP, crop, 4);
	if (error != 0)
		return error;
	*source = *rect;
	return 0;
}

/**
 * converter_stream() - The input picture stands for the stream: its shape is the stream's.
 * @session: a converter session, its input resolved
 * @source:  the part of the picture read
 */
static void converter_stream(struct vc_session *session, const struct VCRect *source)
{
	struct VCStreamInfo *stream = &session->stream;
	memset(stream, 0, sizeof(*stream));
	stream->Width = session->input_geometry.width;
	stream->Height = session->input_geometry.height;
	stream->Crop = *source;
	stream->FrameRate = session->params.FrameRate;
	stream->Par.Num = 1;
	stream->Par.Den = 1;
	stream->Format = session->params.OutputFormat;
	session->have_stream = TRUE;
}

/**
 * converter_open() - Open a converter: ril.isp alone, pictures in, pictures out.
 * @session: a converter session
 *
 * The output shape is resolved from the input picture as a decoder's would
 * be from the stream's shape.  The output format defaults to the input's.
 *
 * Return: 0, VCERR_UNSUPPORTED for an input format the converter does not
 * read (codec_reads()), or the error.
 */
static LONG converter_open(struct vc_session *session)
{
	struct VCSessionParams *params = &session->params;
	if (params->Codec != VCC_Raw)
		return VCERR_UNSUPPORTED;

	LONG error = input_resolve(session, TRUE);
	if (error != 0)
		return error;
	if (!codec_reads(VCK_Converter, VCC_Raw, params->InputFormat))
		return VCERR_UNSUPPORTED;
	if (params->OutputFormat == 0)
		params->OutputFormat = params->InputFormat;

	error = isp_open(session);
	if (error != 0)
		return error;
	session->input_port = session->isp_in;
	session->output_port = session->isp_out;

	error = input_format_commit(session, session->isp_in);
	if (error != 0)
		return error;

	struct VCRect source;
	error = converter_source(session, &source);
	if (error != 0)
		return error;
	converter_stream(session, &source);
	error = shape_resolve(session);
	if (error != 0)
		return error;
	return input_set_fill(session);
}

/**
 * converter_start() - The first send: both sets attached, the output configured, everything running.
 * @session: a converter session, its sets final
 *
 * Return: 0 with the output frames sent, or the error.
 */
static LONG converter_start(struct vc_session *session)
{
	LONG error = set_attach_checked(session, &session->input_set, session->isp_in, "input");
	if (error == 0)
		error = isp_output_configure(session);
	if (error == 0)
		error = component_run(session, session->isp, session->isp_in, session->isp_out);
	if (error != 0)
		return error;
	session->output_state = OUTPUT_RUNNING;
	Kprintf("[videocore] converter running: %ld input pictures of %ld bytes\n", session->input_set.count, session->input_set.size);
	return set_send_output(session, &session->output_set);
}

/**
 * converter_output_item() - A converted picture for the client.
 * @session: the session
 * @object:  the frame object, back from the VPU
 * @item:    the poll's item
 */
static void converter_output_item(struct vc_session *session, struct vc_object *object, struct VCItem *item)
{
	(void)session;
	output_item_frame(object, item);
}

/**
 * converter_set_attrs() - A converter's running controls: the output geometry.
 * @session: the session
 * @changes: the parameters, Changed naming which
 *
 * Return: 0, VCERR_UNSUPPORTED for anything but the geometry, or the error.
 */
static LONG converter_set_attrs(struct vc_session *session, const struct VCSessionParams *changes)
{
	if (changes->Changed & VCSPF_OutputGeometry)
		return isp_geometry_change(session, changes);
	return VCERR_UNSUPPORTED;
}

const struct session_ops converter_ops = {
    .open = converter_open,
    .start = converter_start,
    .send_frame = input_frame_send,
    .output_item = converter_output_item,
    .output_start = isp_output_start,
    .set_attrs = converter_set_attrs,
};
