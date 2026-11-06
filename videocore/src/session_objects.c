/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * session_objects.c - a session's objects and sets: the memory of the
 * client's frames and packets, attaching a set to its port, sending objects
 * to the VPU and describing them back; the components a session opens and
 * runs; and the input pictures of an encoder or converter.
 */
#include <exec/memory.h>

#include <memory.h>

#include <videocore_private.h>
#include <session_internal.h>

#define INPUT_PICTURES_DEFAULT 2 /* the backend's own input pictures, when the client asks for none */

/* ---- objects --------------------------------------------------------------------- */

/**
 * object_new() - A frame or packet object.
 * @session:  its session
 * @is_frame: a picture, else a packet
 * @data:     the client's memory, or NULL for the backend's own from the DMA pool
 * @capacity: its bytes
 *
 * The client's memory stays in its hands; the backend's is handed out (an
 * input object) or sent (an output object).
 *
 * Return: the object, NULL without memory.
 */
struct vc_object *object_new(struct vc_session *session, BOOL is_frame, APTR data, u32 capacity)
{
	struct VideoCoreBase *base = session->base;
	struct ExecBase *SysBase = base->SysBase;

	struct vc_object *object = AllocMem(sizeof(*object), MEMF_PUBLIC | MEMF_CLEAR);
	if (object == NULL)
		return NULL;
	BOOL owned = data == NULL;
	if (owned)
	{
		/* aligned to a cache line: the VPU's DMA writes whole lines */
		data = base->mmal.pool != NULL ? dma_alloc(base->mmal.pool, DMA_ALIGN_MIN, capacity) : NULL;
		if (data == NULL)
		{
			FreeMem(object, sizeof(*object));
			return NULL;
		}
	}
	Kprintf("[videocore] %s at %08lx, %ld bytes%s\n", is_frame ? "frame" : "packet", (ULONG)data, capacity, owned ? "" : ", the client's");
	object->buffer.data = data;
	object->buffer.room = owned ? ALIGN_UP(capacity, DMA_ALIGN_MIN) : capacity; /* dma_alloc() gives whole lines */
	object->capacity = capacity;
	object->is_frame = is_frame;
	object->owned = owned;
	object->at_client = !owned; /* an import is the client's until it sends or releases it */
	if (is_frame)
	{
		object->pub.frame.Size = sizeof(struct VCFrame);
		object->pub.frame.Data = data;
		object->pub.frame.Pts = VC_TIME_UNKNOWN;
	}
	else
	{
		object->pub.packet.Size = sizeof(struct VCPacket);
		object->pub.packet.Data = data;
		object->pub.packet.Capacity = capacity;
		object->pub.packet.Pts = VC_TIME_UNKNOWN;
	}
	return object;
}

/**
 * object_free() - Free an object, and its memory if the backend's.
 * @session: its session
 * @object:  the object, detached
 */
void object_free(struct vc_session *session, struct vc_object *object)
{
	struct VideoCoreBase *base = session->base;
	struct ExecBase *SysBase = base->SysBase;
	if (object->owned)
		dma_free(base->mmal.pool, object->buffer.data);
	FreeMem(object, sizeof(*object));
}

/**
 * object_lookup() - The object behind a client's frame or packet pointer.
 * @session:  the session
 * @pub:      the client's pointer
 * @side:     the side it must belong to
 * @is_frame: whether it must be a frame
 *
 * Return: the object, NULL if @pub is none of this session's on @side.
 */
struct vc_object *object_lookup(struct vc_session *session, const void *pub, ULONG side, BOOL is_frame)
{
	/* the client's pointer is the object's own address (pub comes first), but only one the set lists is trusted */
	struct vc_set *set = side == VCS_Input ? &session->input_set : &session->output_set;
	for (u32 i = 0; i < set->count; i++)
		if ((const void *)&set->objects[i]->pub == pub && set->objects[i]->is_frame == is_frame)
			return set->objects[i];
	return NULL;
}

/**
 * object_send() - Send an object to the VPU, through the port it is attached to.
 * @session: the session
 * @object:  an attached object
 * @length:  bytes of data in it; 0 for an output object, which the VPU fills
 * @flags:   MMAL_BUFFER_HEADER_FLAG_*
 * @pts:     its presentation time
 *
 * Sent, the object is out of the client's hands until it comes back.
 *
 * Return: 0, VCERR_BADSTATE if the object is not attached, or the send's
 * error.
 */
LONG object_send(struct vc_session *session, struct vc_object *object, u32 length, ULONG flags, VCTime pts)
{
	if (object->buffer.port == NULL)
		return VCERR_BADSTATE;

	LONG error = mmal_buffer_send(session->base, &object->buffer, length, flags, pts);
	if (error == 0)
	{
		object->at_client = FALSE;
		object->flushes = session->flushes;
	}
#ifdef PROFILE
	object->stamp = get_time();
#endif
	return error;
}

/**
 * output_item_frame() - Describe a filled output frame as a poll item.
 * @object: the frame object, back from the VPU, its layout described
 * @item:   the poll's item
 */
void output_item_frame(struct vc_object *object, struct VCItem *item)
{
	frame_from_return(object);
	item->Type = VCIT_Frame;
	item->u.Frame = &object->pub.frame;
}

/**
 * output_item_packet() - Describe a filled output packet as a poll item.
 * @object: the packet object, back from the VPU
 * @item:   the poll's item
 */
void output_item_packet(struct vc_object *object, struct VCItem *item)
{
	packet_from_return(object);
	item->Type = VCIT_Packet;
	item->u.Packet = &object->pub.packet;
}

/* ---- sets ------------------------------------------------------------------------ */

/**
 * set_new_object() - A new object, added to a set.
 * @session:  the session
 * @set:      the set
 * @is_frame: a picture, else a packet
 * @data:     the client's memory, or NULL for the backend's own
 * @capacity: its bytes
 *
 * Return: the object, NULL without memory or with the set full.
 */
struct vc_object *set_new_object(struct vc_session *session, struct vc_set *set, BOOL is_frame, APTR data, u32 capacity)
{
	if (set->count >= SESSION_SET_MAX)
		return NULL;
	struct vc_object *object = object_new(session, is_frame, data, capacity);
	if (object != NULL)
		set->objects[set->count++] = object;
	return object;
}

/**
 * set_fill() - Fill a set with objects of the backend's own memory, in place of what it held.
 * @session:  the session
 * @set:      the set, detached
 * @is_frame: frames, else packets
 * @count:    how many, at most SESSION_SET_MAX
 * @size:     the bytes of each, which becomes the set's buffer size
 *
 * The objects it held are freed first: kept, they would be counted with the
 * new ones and attached at the new size, which they may not hold.  The
 * client's pointers to them are invalid from here.
 *
 * Return: 0, or VCERR_NOMEM (the objects made so far stay in the set).
 */
LONG set_fill(struct vc_session *session, struct vc_set *set, BOOL is_frame, u32 count, u32 size)
{
	set_empty(session, set);
	set->imported = FALSE;
	for (u32 i = 0; i < count; i++)
		if (set_new_object(session, set, is_frame, NULL, size) == NULL)
			return VCERR_NOMEM;
	set->size = size;
	return 0;
}

/**
 * set_empty() - Free a set's objects, leaving it open for new ones.
 * @session: the session
 * @set:     the set, detached
 *
 * The client's pointers to the objects are invalid from here.
 */
void set_empty(struct vc_session *session, struct vc_set *set)
{
	for (u32 i = 0; i < set->count; i++)
		object_free(session, set->objects[i]);
	set->count = 0;
}

/**
 * set_holds() - Whether every object of a set holds a buffer of a size.
 * @set:  the set
 * @size: the bytes each must hold
 *
 * Return: TRUE if none is smaller, as for an empty set.
 */
static BOOL set_holds(const struct vc_set *set, u32 size)
{
	for (u32 i = 0; i < set->count; i++)
		if (set->objects[i]->capacity < size)
			return FALSE;
	return TRUE;
}

/**
 * set_at_client() - Whether the client holds any object of a set.
 * @set: the set
 *
 * Return: TRUE if one is in the client's hands.
 */
BOOL set_at_client(const struct vc_set *set)
{
	for (u32 i = 0; i < set->count; i++)
		if (set->objects[i]->at_client)
			return TRUE;
	return FALSE;
}

/**
 * set_attach() - Attach a set's objects to a port, one buffer each, and one more of the backend's.
 * @session: the session
 * @set:     the set, @set->size its buffer size
 * @extra:   an object of the backend's to attach after the set's (a decoder's codec configuration), or NULL
 * @port:    the port, disabled, none attached
 *
 * The port's buffer size is the set's, or @extra's if that is larger.  The
 * set is fixed from here: no import any more.
 *
 * Return: 0, VCERR_BADSTATE for an empty set, or the attach's error.
 */
LONG set_attach(struct vc_session *session, struct vc_set *set, struct vc_object *extra, struct mmal_port *port)
{
	struct mmal_buffer *buffers[SESSION_SET_MAX + 1];

	if (set->count == 0)
		return VCERR_BADSTATE;
	u32 count = 0, size = set->size;
	for (u32 i = 0; i < set->count; i++)
		buffers[count++] = &set->objects[i]->buffer;
	if (extra != NULL)
	{
		buffers[count++] = &extra->buffer;
		size = umax(size, extra->capacity);
	}
	LONG error = mmal_port_attach(session->base, port, buffers, count, size);
	if (error != 0)
	{
		Kprintf("[videocore] attach %ld x %ld bytes to %s port %ld: %ld\n", count, size, port->component->name,
		        (ULONG)port->index_all, error);
		return error;
	}
	set->port = port;
	return 0;
}

/**
 * set_size_from_imports() - An imported set's buffer size: its smallest object.
 * @set: the set; one of the backend's own keeps its size
 *
 * The port runs with one buffer size: the smallest import decides it (no
 * import has 0 bytes); an empty set has 0.
 */
void set_size_from_imports(struct vc_set *set)
{
	if (!set->imported)
		return;
	u32 size = set->count != 0 ? set->objects[0]->capacity : 0;
	for (u32 i = 1; i < set->count; i++)
		size = umin(size, set->objects[i]->capacity);
	set->size = size;
}

/**
 * set_check_minimum() - Whether a set can serve a port: enough objects, big enough.
 * @set:  the set
 * @port: the port, its minimum known
 * @what: the side, for the log
 *
 * Return: 0, or VCERR_BADPARAM below the VPU's minimum (logged).
 */
LONG set_check_minimum(const struct vc_set *set, const struct mmal_port *port, const char *what)
{
	(void)what; /* only the debug log reads it */
	if (set->count >= port->buffer_num_min && set->size >= port->buffer_size_min)
		return 0;
	Kprintf("[videocore] the %s set (%ld x %ld) is below the VPU's minimum of %ld x %ld\n", what, set->count, set->size,
	        port->buffer_num_min, port->buffer_size_min);
	return VCERR_BADPARAM;
}

/**
 * set_attach_checked() - A side at the first send: sized from its imports, checked against the port, attached.
 * @session: the session
 * @set:     the set, final
 * @port:    the port, disabled, none attached
 * @what:    the side, for the log
 *
 * Return: 0, VCERR_BADPARAM below the VPU's minimum, or the attach's error.
 */
LONG set_attach_checked(struct vc_session *session, struct vc_set *set, struct mmal_port *port, const char *what)
{
	set_size_from_imports(set);
	LONG error = set_check_minimum(set, port, what);
	if (error == 0)
		error = set_attach(session, set, NULL, port);
	return error;
}

/**
 * set_detach() - Stop a port and detach its objects (the output set, or the token).
 * @session: the session
 * @port:    the port
 *
 * Return: 0 or the error.
 */
LONG set_detach(struct vc_session *session, struct mmal_port *port)
{
	LONG error = mmal_port_disable(session->base, port);
	if (error == 0)
		error = mmal_port_detach(session->base, port);
	if (error != 0)
		return error;
	if (session->output_set.port == port)
		session->output_set.port = NULL; /* open for imports and new objects again; the token belongs to no set */
	return 0;
}

/**
 * set_send_output() - Send every output object not in the client's hands to the VPU.
 * @session: the session
 * @set:     the output set, attached to its running port
 *
 * Objects the client released before the set was attached go out here too.
 *
 * Return: 0, or the first send's error.
 */
LONG set_send_output(struct vc_session *session, struct vc_set *set)
{
	for (u32 i = 0; i < set->count; i++)
	{
		struct vc_object *object = set->objects[i];
		if (object->at_client)
			continue; /* the client still holds it: its release sends it */
		LONG error = object_send(session, object, 0, 0, MMAL_TIME_UNKNOWN);
		if (error != 0)
			return error;
	}
	return 0;
}

/**
 * set_shape_frames() - Give every frame of a set the same layout.
 * @set:      a set of frames
 * @geometry: the layout
 * @format:   VCF_*
 * @crop:     the visible picture in each
 */
void set_shape_frames(struct vc_set *set, const struct vc_geometry *geometry, ULONG format, const struct VCRect *crop)
{
	for (u32 i = 0; i < set->count; i++)
		frame_set_geometry(set->objects[i], geometry, format, crop);
}

/**
 * output_set_count() - How many output objects the session has.
 * @session:  the session
 * @fallback: the backend's own count when the client asked for none
 *
 * Return: the imported set's count, VCA_OutputCount (1..SESSION_SET_MAX), or @fallback.
 */
u32 output_set_count(const struct vc_session *session, u32 fallback)
{
	if (session->output_set.imported)
		return session->output_set.count;
	return tag_count(session->params.OutputCount, fallback);
}

/**
 * output_set_prepare() - The output frames for session->shape: the client's must hold them, the backend's are made again.
 * @session: the session, its shape resolved, its output set detached
 * @count:   how many of the backend's own
 * @size:    the bytes of each, which becomes the set's buffer size
 *
 * Either way the frames are described with the shape.
 *
 * Return: 0, VCERR_BADPARAM for an imported frame too small, or VCERR_NOMEM.
 */
LONG output_set_prepare(struct vc_session *session, u32 count, u32 size)
{
	struct vc_set *set = &session->output_set;
	const struct vc_shape *shape = &session->shape;
	if (set->imported)
	{
		if (!set_holds(set, size))
			return VCERR_BADPARAM;
		set->size = size;
	}
	else
	{
		LONG error = set_fill(session, set, TRUE, count, size); /* made again for the size: the old ones may be too small */
		if (error != 0)
			return error;
	}
	set_shape_frames(set, &shape->geometry, shape->format, &shape->dest);
	return 0;
}

/* ---- components -------------------------------------------------------------------- */

/**
 * component_open() - Create a VPU component for the session, or take a kept one, and take its first ports.
 * @session:   the session
 * @name:      the VPU's name for it
 * @component: where the component goes
 * @in:        where its first input port goes
 * @out:       where its first output port goes
 *
 * An image call's session takes the component an earlier call kept, if one
 * of the name is (mmal_component_take()); its ports then start from their
 * state at the first create, and component->reused says parameters of the
 * last use may still be set.
 *
 * Return: 0 or the create's error.
 */
LONG component_open(struct vc_session *session, const char *name, struct mmal_component **component, struct mmal_port **in,
                    struct mmal_port **out)
{
	const struct VCSessionParams *params = &session->params;
	*component = session->image_call ? mmal_component_take(session->base, name, params->SigTask, params->SigMask) : NULL;
	if (*component == NULL)
	{
		LONG error = mmal_component_create(session->base, name, params->SigTask, params->SigMask, component);
		if (error != 0)
			return error;
	}
	*in = (*component)->input;
	*out = (*component)->output;
	return 0;
}

/**
 * component_run() - Enable a component and its input and output ports, in that order.
 * @session:   the session
 * @component: the component, its formats committed and its buffers attached
 * @in:        its input port, or NULL to leave it
 * @out:       its output port, or NULL to leave it
 *
 * Return: 0 or the first error.
 */
LONG component_run(struct vc_session *session, struct mmal_component *component, struct mmal_port *in, struct mmal_port *out)
{
	struct VideoCoreBase *base = session->base;
	LONG error = mmal_component_enable(base, component);
	if (error == 0 && in != NULL)
		error = mmal_port_enable(base, in);
	if (error == 0 && out != NULL)
		error = mmal_port_enable(base, out);
	return error;
}

/* ---- the input pictures of an encoder or converter --------------------------------- */

/**
 * input_resolve() - The input pictures' layout, from VCA_Width/Height, VCA_InputFormat, VCA_InputPitch.
 * @session:  the session; VCA_InputFormat defaults to I420
 * @isp_rows: without a pitch, planar pictures get rows the ISP reads (isp_row_pixels()), else the natural ones
 *
 * The format chosen is written back to the parameters, which every later
 * step reads.  NV12 into the ISP keeps its own rows, the chroma right after
 * the picture's last, as a camera sends it; I420 keeps the VPU's multiple of
 * 16.
 *
 * Return: 0, VCERR_UNSUPPORTED for an unknown format, VCERR_BADPARAM for no
 * size or a pitch too small.
 */
LONG input_resolve(struct vc_session *session, BOOL isp_rows)
{
	struct VCSessionParams *params = &session->params;
	ULONG format = params->InputFormat != 0 ? params->InputFormat : VCF_I420;
	if (format_bytes_per_pixel(format) == 0)
		return VCERR_UNSUPPORTED;
	if (params->Width == 0 || params->Height == 0)
		return VCERR_BADPARAM;
	u32 pitch = params->InputPitch;
	if (pitch == 0 && isp_rows && format_is_planar(format))
		pitch = ALIGN_UP(params->Width, isp_row_pixels(format));
	if (!picture_geometry(format, params->Width, params->Height, pitch, &session->input_geometry))
		return VCERR_BADPARAM;
	params->InputFormat = format;
	return 0;
}

/**
 * input_frame_shape() - Describe an input picture to the client: the resolved layout, the whole picture visible.
 * @session: the session, its input resolved
 * @object:  an input frame
 */
void input_frame_shape(struct vc_session *session, struct vc_object *object)
{
	struct VCRect whole = rect_whole(session->params.Width, session->params.Height);
	frame_set_geometry(object, &session->input_geometry, session->params.InputFormat, &whole);
}

/**
 * input_set_fill() - The backend's own input pictures: VCA_InputCount of them, or two.
 * @session: the session, its input resolved and its input set empty
 *
 * Each is described as it is made, so the poll can hand it to the client.
 *
 * Return: 0 or VCERR_NOMEM.
 */
LONG input_set_fill(struct vc_session *session)
{
	struct vc_set *set = &session->input_set;
	u32 count = tag_count(session->params.InputCount, INPUT_PICTURES_DEFAULT);
	LONG error = set_fill(session, set, TRUE, count, session->input_geometry.size);
	for (u32 i = 0; i < set->count; i++)
		input_frame_shape(session, set->objects[i]);
	return error;
}

/**
 * input_format_commit() - A raw input port takes the input pictures' format.
 * @session: the session, its input resolved
 * @port:    the encoder's or the converter's input
 *
 * The picture's buffer, the whole picture visible, the rate; for an encoder
 * the colour space, which the ISP ignores (it reads YUV as full-range BT.601
 * whatever it is told).  The ISP reads
 * a 32-bit picture's byte order the other way round from how it writes it
 * (an RGBA input comes out with red and blue swapped, as does a BGRA one),
 * so its input gets the other encoding of the two.
 *
 * A port that reads the rows at another width than the pictures' (the ISP
 * rounds it up to its isp_row_pixels()) would read them skewed: refused.
 *
 * Return: 0, VCERR_UNSUPPORTED if the port does not take the pixel format,
 * VCERR_BADPARAM for rows it reads at another width, or the commit's error.
 */
LONG input_format_commit(struct vc_session *session, struct mmal_port *port)
{
	const struct VCSessionParams *params = &session->params;
	const struct vc_geometry *geometry = &session->input_geometry;
	u32 encoding = format_encoding(params->InputFormat);
	/* Firmware quirk: the ISP's input reads RGBA as BGRA and the reverse (its output is right), so commit the other one */
	if (port == session->isp_in && (encoding == MMAL_ENCODING_RGBA || encoding == MMAL_ENCODING_BGRA))
		encoding = encoding == MMAL_ENCODING_RGBA ? MMAL_ENCODING_BGRA : MMAL_ENCODING_RGBA;

	if (!mmal_port_supports(session->base, port, encoding))
	{
		Kprintf("[videocore] %s input does not list %08lx among its %ld encodings\n", port->component->name, encoding,
		        (ULONG)port->encoding_count);
		return VCERR_UNSUPPORTED;
	}

	struct VCRect whole = rect_whole(params->Width, params->Height);
	video_format_set(&port->format, encoding, geometry->width, geometry->height, &whole, &params->FrameRate);
	u32 width = geometry->width;
	if (session->kind == SESSION_ENCODER)
		port->format.es.video.color_space = color_space_mmal(params->ColorSpace); /* the encoder writes it into the SPS */

	/* The column format goes as its width and rows like any picture: the ISP refuses the column
	 * stride as the width (the COL_FMTS_WIDTH_IS_COL_STRIDE flag, EINVAL) and implies the stride
	 * from the rows, 16-aligned, as picture_geometry() lays it out (docs/firmware-facts.md). */
	LONG error = mmal_port_commit(session->base, port);
	const struct mmal_format *taken = &port->format; /* as the VPU read it back */
	if (error != 0)
		Kprintf("[videocore] commit input format %ld (%ldx%ld) on %s: %ld\n", params->InputFormat, width, geometry->height,
		        port->component->name, error);
	else if (taken->es.video.width != width || taken->es.video.height != geometry->height)
	{
		Kprintf("[videocore] %s input read back as %ldx%ld, the pictures are %ldx%ld\n", port->component->name, taken->es.video.width,
		        taken->es.video.height, width, geometry->height);
		/* the VPU's stride follows its width; more rows only make the buffer bigger, which the set check sees */
		if (taken->es.video.width != width)
			error = VCERR_BADPARAM;
	}
	return error;
}

/**
 * input_frame_send() - Send an input picture of the client's (a converter's or encoder's send_frame).
 * @session: the session, started
 * @object:  the frame's object, in the client's hands
 * @frame:   the frame
 *
 * A picture is always whole: its length is the input layout's size, or
 * nothing with VCFF_NoPicture.
 *
 * Return: 0 or the error.
 */
LONG input_frame_send(struct vc_session *session, struct vc_object *object, struct VCFrame *frame)
{
	ULONG flags = (frame->Flags & VCFF_EndOfStream) ? MMAL_BUFFER_HEADER_FLAG_EOS : 0;
	u32 length = (frame->Flags & VCFF_NoPicture) ? 0 : session->input_geometry.size;
	LONG error = object_send(session, object, length, flags, frame->Pts);
	if (error == 0 && flags != 0)
		session->end_sent = TRUE;
	return error;
}
