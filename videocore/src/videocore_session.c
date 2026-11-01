/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * videocore_session.c - the session core: open and close, imports, sends
 * and releases, the poll and its events.  It knows no kind: where the kinds
 * differ it calls the session's operations (session_internal.h has the
 * model, the kinds and the other session sources).
 */
#include <exec/memory.h>

#include <memory.h>
#include <minlist.h>
#include <timing.h>

#include <videocore_private.h>
#include <session_internal.h>

#define RESET_LOCK_MS 4000u    /* each; a call the VPU does not answer gives up within 3 s */
#define SR_SUPERVISOR 0x2000u /* in GetCC() */

#define PERF_REPORT_US 2000000u /* the frame path's timings, reported from the poll at most this often (profile tier) */

/* ---- events -------------------------------------------------------------------------- */

/**
 * events_push() - Queue an event for the client's poll.
 * @session: the session
 * @event:   VCEV_*; VCEV_StreamInfo carries the session's stream
 * @status:  the VCERR_* of a VCEV_Error, else 0
 *
 * A full queue drops the event (logged): the client does not poll.
 */
void events_push(struct vc_session *session, ULONG event, LONG status)
{
	if (session->event_count == SESSION_EVENTS_MAX)
	{
		Kprintf("[videocore] event %ld dropped: the client does not poll\n", event);
		return;
	}
	struct VCEvent *slot = &session->events[(session->event_head + session->event_count) % SESSION_EVENTS_MAX];
	memset(slot, 0, sizeof(*slot));
	slot->Event = event;
	slot->Status = status;
	if (event == VCEV_StreamInfo)
		slot->Stream = session->stream;
	session->event_count++;
}

/**
 * events_pop() - The oldest queued event, as a poll item.
 * @session: the session
 * @item:    the poll's item
 *
 * Return: TRUE with @item filled, FALSE if none waits.
 */
static BOOL events_pop(struct vc_session *session, struct VCItem *item)
{
	if (session->event_count == 0)
		return FALSE;
	item->Type = VCIT_Event;
	item->u.Event = session->events[session->event_head];
	session->event_head = (session->event_head + 1u) % SESSION_EVENTS_MAX;
	session->event_count--;
	return TRUE;
}

/**
 * session_fail() - End the session: the client learns why at its next poll.
 * @session: the session
 * @error:   the VCERR_*
 * @what:    what failed, for the log
 *
 * Only the first failure counts.  VCERR_DEVMEM is the VPU's own memory:
 * the log says where it is set.
 */
void session_fail(struct vc_session *session, LONG error, const char *what)
{
	(void)what; /* only the debug log reads it */
	if (session->failed)
		return;
	Kprintf("[videocore] session %08lx failed: %s (%ld)%s\n", (ULONG)session, what, error,
	        error == VCERR_DEVMEM ? ", the VPU's memory: gpu_mem in config.txt" : "");
	session->failed = TRUE;
	events_push(session, VCEV_Error, error);
}

/* ---- the poll ------------------------------------------------------------------------ */

/**
 * input_item() - Give an input object to the client as a poll item.
 * @object: an input object, home from the VPU or not sent yet
 * @item:   the poll's item
 *
 * Its length and flags are cleared: the client sets them for its next send.
 */
static void input_item(struct vc_object *object, struct VCItem *item)
{
	object->at_client = TRUE;
	if (object->is_frame)
	{
		object->pub.frame.Flags = 0;
		item->Type = VCIT_FrameReturned;
		item->u.Frame = &object->pub.frame;
	}
	else
	{
		object->pub.packet.Length = 0;
		object->pub.packet.Flags = 0;
		item->Type = VCIT_PacketReturned;
		item->u.Packet = &object->pub.packet;
	}
}

/**
 * input_hand_out() - Give the client one of the backend's input objects it has not had yet.
 * @session: the session
 * @item:    the poll's item
 *
 * The backend's own input objects reach the client this way, once each:
 * before the start, and after it those the client had not polled for when
 * it first sent.
 *
 * Return: TRUE with @item filled, FALSE if every one was handed out.
 */
static BOOL input_hand_out(struct vc_session *session, struct VCItem *item)
{
	for (u32 i = 0; i < session->input_set.count; i++)
	{
		struct vc_object *object = session->input_set.objects[i];
		if (object->owned && !object->handed)
		{
			object->handed = TRUE;
			input_item(object, item);
			return TRUE;
		}
	}
	return FALSE;
}

/**
 * poll_controls() - The components' errors.
 * @session: a started session
 */
static void poll_controls(struct vc_session *session)
{
	struct mmal_component *const components[] = {session->decoder, session->isp, session->encoder};
	for (u32 i = 0; i < ARRAY_SIZE(components); i++)
	{
		LONG error = components[i] != NULL ? mmal_component_error(session->base, components[i]) : 0;
		if (error != 0)
			session_fail(session, error, "error event");
	}
}

/**
 * output_object_send() - Send an output object to the VPU, to be filled.
 * @session: the session
 * @object:  an object attached to the running output port, or the token
 * @what:    what sends it, for the log
 *
 * A failed send fails the session: the object would be lost to the client
 * and the VPU alike.
 */
static void output_object_send(struct vc_session *session, struct vc_object *object, const char *what)
{
	LONG error = object_send(session, object, 0, 0, MMAL_TIME_UNKNOWN);
	if (error != 0)
		session_fail(session, error, what);
}

/**
 * poll_format_changed() - A FORMAT_CHANGED from the output port goes to the kind.
 * @session: the session
 * @event:   the event, taken from the output port
 *
 * The change is copied out, so the event buffer goes back to its pool before
 * the change applies.  A kind without a format_changed hears nothing (the
 * ISP's output: the session sets that output's format itself).
 */
static void poll_format_changed(struct vc_session *session, struct mmal_buffer *event)
{
	struct mmal_format_change change = *(const struct mmal_format_change *)event->data;
	mmal_event_done(session->base, event);
	if (session->ops->format_changed != NULL)
		session->ops->format_changed(session, &change);
}

/**
 * poll_output_returned() - Sort out one buffer the output port gave back.
 * @session: a started session
 * @buffer:  the buffer, taken from the output port
 *
 * A FORMAT_CHANGED goes to the kind.  An empty object goes straight back to
 * the VPU, the token included; the token with a picture is the kind's
 * (token_returned).  An object filled before the latest flush (still in
 * transfer, or queued here, when the VPU let go of the rest) goes back too:
 * the client sees nothing from before a flush.  An end of stream is queued
 * as an event, after the picture it came with, once the client has sent its
 * end mark: before that it is the VPU's answer to a port being disabled.
 * In a failed session every object stays home.
 *
 * Return: the object for the client, NULL if none.
 */
static struct vc_object *poll_output_returned(struct vc_session *session, struct mmal_buffer *buffer)
{
	if (buffer->cmd != 0) /* a FORMAT_CHANGED: the only event a port queues */
	{
		poll_format_changed(session, buffer);
		return NULL;
	}

	if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_TRANSMISSION_FAILED)
		session_fail(session, VCERR_TRANSPORT, "an output transfer failed");

	if (session->failed)
		return NULL;

	struct vc_object *object = CONTAINER_OF(buffer, struct vc_object, buffer);
	PERF_ADD(&session->base->perf, VCPERF_OUT_VPU, object->stamp);
	BOOL end = (buffer->flags & MMAL_BUFFER_HEADER_FLAG_EOS) != 0; /* read now: the token may be freed below */
	BOOL stale = object->flushes != session->flushes;
	BOOL for_client = FALSE;
	if (buffer->length == 0)
		output_object_send(session, object, "an empty output back"); /* nothing in it: back to be filled */
	else if (object == session->token)
		for_client = session->ops->token_returned(session, object);
	else if (stale)
		output_object_send(session, object, "a picture from before a flush");
	else
		for_client = TRUE;
	/* before the client's end mark, an end of stream is the VPU's answer to a port disabled */
	if (end && session->end_sent && !stale)
		events_push(session, VCEV_EndOfStream, 0);
	return for_client ? object : NULL;
}

/**
 * poll_output() - The next filled output object for the client.
 * @session: a started session
 * @item:    the poll's item
 *
 * Takes the output port's buffers until one is for the client.  Stops once
 * an event is queued, so the client hears of it before what follows, and
 * while a new shape waits for the client's frames: what the port holds
 * after the change waits with it.
 *
 * Return: TRUE with @item filled.
 */
static BOOL poll_output(struct vc_session *session, struct VCItem *item)
{
	struct mmal_buffer *buffer;
	while (session->event_count == 0 && session->output_state != OUTPUT_CHANGE_WAITING &&
	       (buffer = mmal_port_take(session->base, session->output_port)) != NULL)
	{
		struct vc_object *object = poll_output_returned(session, buffer);
		if (object != NULL)
		{
			session->ops->output_item(session, object, item);
			object->at_client = TRUE;
#ifdef PROFILE
			object->stamp = get_time();
#endif
			return TRUE;
		}
	}
	return FALSE;
}

/**
 * poll_input() - The next input object back from the VPU, or one the client has not had yet.
 * @session: a started session
 * @item:    the poll's item
 *
 * Return: TRUE with @item filled.
 */
static BOOL poll_input(struct vc_session *session, struct VCItem *item)
{
	struct mmal_buffer *buffer;
	while ((buffer = mmal_port_take(session->base, session->input_port)) != NULL) /* an input port queues no event */
	{
		struct vc_object *object = CONTAINER_OF(buffer, struct vc_object, buffer);
		if (object == session->config)
			continue; /* the codec configuration, read: the backend's until the close */
		PERF_ADD(&session->base->perf, VCPERF_IN_VPU, object->stamp);
		input_item(object, item);
		return TRUE;
	}
	return input_hand_out(session, item);
}

/**
 * poll_next() - The next thing for the client.
 * @session: the session
 * @item:    the poll's item
 *
 * In order: a queued event; before the start, the backend's input objects.
 * Once started: the output's start if it waits for this poll; the
 * components' errors and the output port's format changes; an output
 * object; an input object back.  An event queued on the way goes out before
 * anything after it.
 *
 * Return: TRUE with @item filled, FALSE if nothing is ready.
 */
static BOOL poll_next(struct vc_session *session, struct VCItem *item)
{
	if (events_pop(session, item))
		return TRUE;

	if (!session->started)
		return input_hand_out(session, item);

	if (session->output_state == OUTPUT_START_PENDING && !session->failed)
	{
		LONG error = session->ops->output_start != NULL ? session->ops->output_start(session) : VCERR_BADSTATE;
		if (error != 0)
			session_fail(session, error, "output start");
	}
	poll_controls(session);
	if (events_pop(session, item) || poll_output(session, item) || events_pop(session, item))
		return TRUE;
	return poll_input(session, item);
}

/**
 * session_poll() - The next thing for the client (VC_Poll()).
 * @session: the session
 * @item:    the poll's item
 *
 * At the profile tier it times itself and reports the frame path's timings
 * every PERF_REPORT_US.
 *
 * Return: TRUE with @item filled, FALSE if nothing is ready.
 */
BOOL session_poll(struct vc_session *session, struct VCItem *item)
{
	PERF_T0(t0);
	BOOL ready = poll_next(session, item);
	PERF_ADD(&session->base->perf, VCPERF_POLL, t0);
#ifdef PROFILE
	struct VideoCoreBase *base = session->base;
	if (get_time() - base->perf_reported_at >= PERF_REPORT_US)
	{
		perf_report(&base->perf);
		base->perf_reported_at = get_time();
	}
#endif
	return ready;
}

/* ---- open and close ------------------------------------------------------------------- */

/**
 * sessions_init() - No sessions yet.
 * @base: the library base, at init
 */
void sessions_init(struct VideoCoreBase *base)
{
	_NewMinList(&base->sessions);
	base->session_count = 0;
}

/**
 * session_lookup() - The session behind a handle, without touching the handle.
 * @base:   the library base
 * @handle: what the frontend passed
 *
 * Return: the session, NULL if @handle is no open session (a closed one included).
 */
struct vc_session *session_lookup(struct VideoCoreBase *base, APTR handle)
{
	for (struct MinNode *node = base->sessions.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
	{
		struct vc_session *session = CONTAINER_OF(node, struct vc_session, node);
		if ((APTR)session == handle && session->magic == SESSION_MAGIC)
			return session;
	}
	return NULL;
}

/**
 * kind_of() - The kind of session the client's parameters name.
 * @params: the parameters
 * @kind:   where the kind goes
 *
 * Return: TRUE, or FALSE for a VCK_* this backend has no kind for.
 */
static BOOL kind_of(const struct VCSessionParams *params, enum session_kind *kind)
{
	switch (params->Kind)
	{
	case VCK_Decoder:
		*kind = codec_still(params->Codec) ? SESSION_STILL : SESSION_DECODER;
		return TRUE;
	case VCK_Converter:
		*kind = SESSION_CONVERTER;
		return TRUE;
	case VCK_Encoder:
		*kind = SESSION_ENCODER;
		return TRUE;
	default:
		return FALSE;
	}
}

/**
 * components_close() - Stop and destroy a session's components, or keep an image call's.
 * @session: the session, about to be freed
 * @keep:    keep what can be kept for the next image call
 *
 * Every port stops before any component goes, so the VPU's DMA into the
 * session's memory has ended: an encoder's packets first, then the tunnel's
 * ends, its output end before its input, then each component's output before
 * its input.  Destroying a
 * component breaks its tunnel.  An image call's image decoder or converter
 * that ended well is kept for the next call (mmal_component_keep()), which
 * saves creating and destroying it; mmal_idle() destroys it in the end.
 */
static void components_close(struct vc_session *session, BOOL keep)
{
	struct VideoCoreBase *base = session->base;

	/* NULL for a component the session lacks or did not get to create */
	struct mmal_port *const ports[] = {session->decoder_out, session->encoder_out, session->isp_in,
	                                   session->isp_out,     session->decoder_in,  session->encoder_in};
	for (u32 i = 0; i < ARRAY_SIZE(ports); i++)
		if (ports[i] != NULL)
			(void)mmal_port_disable(base, ports[i]);

	struct mmal_component *const components[] = {session->isp, session->decoder, session->encoder};
	for (u32 i = 0; i < ARRAY_SIZE(components); i++)
	{
		struct mmal_component *component = components[i];
		if (component == NULL)
			continue;
		BOOL keepable = keep && !session->failed && component != session->encoder;
		if (!keepable || !mmal_component_keep(base, component))
			mmal_component_destroy(base, component);
	}
}

/**
 * reset_lock() - Take a lock before a reset, its holder given some time to leave.
 * @base: the library base
 * @lock: the lock
 *
 * A holder is at most waiting for the VPU, and should it not come out the
 * reset goes on without the lock: nothing here may keep the machine up.
 *
 * Return: TRUE with the lock held.
 */
static BOOL reset_lock(struct VideoCoreBase *base, struct SignalSemaphore *lock)
{
	struct ExecBase *SysBase = base->SysBase;
	for (u32 ms = 0; ms < RESET_LOCK_MS; ms++)
	{
		if (AttemptSemaphore(lock))
			return TRUE;
		delay_us(1000);
	}
	return FALSE;
}

/**
 * sessions_reset() - Before a reset: every session's components off the VPU (the reset guard's work).
 * @user: the library base
 *
 * A reset by the 68k leaves the VPU running, and a service's close does not
 * take its components with it: they stay, with their memory and the imports
 * they hold.  After a recording the next open of the SMEM service then gets no
 * answer, nor does anything after it, until the Pi restarts.  So they go here,
 * while the link still works: this guard came after vchiq.device's and runs
 * before it, which then closes the services.
 *
 * ColdReboot() comes here in its caller's task, where calls can wait for the
 * VPU.  The locks stay held, so no client gets in again, and no client memory
 * is freed, as its task may be writing there until the reset.  A keyboard reset
 * may come in an interrupt, where none of this can run; it restarts the Pi,
 * and the VPU with it.
 */
void sessions_reset(APTR user)
{
	struct VideoCoreBase *base = user;
	struct ExecBase *SysBase = base->SysBase;
	if ((GetCC() & SR_SUPERVISOR) != 0)
		return;
	if (!reset_lock(base, &base->image_lock) || !reset_lock(base, &base->session_lock))
		return;
	for (struct MinNode *node = base->sessions.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
		components_close(CONTAINER_OF(node, struct vc_session, node), FALSE);
	mmal_kept_release(base);
}

/**
 * session_open() - Open a session of the kind the parameters name (VCB_OpenSession(), the image calls).
 * @base:       the library base
 * @params:     the frontend's parameters, copied as far as their Size reaches
 * @image_call: one of an image call's sessions: it may take a kept component
 * @error:      where the error goes, 0 on success
 *
 * A still picture's decoder is the image calls' only.
 *
 * Return: the session, NULL with *@error set (VCERR_UNSUPPORTED for a still picture's codec from a client).
 */
struct vc_session *session_open(struct VideoCoreBase *base, const struct VCSessionParams *params, BOOL image_call, LONG *error)
{
	struct ExecBase *SysBase = base->SysBase;
	static const struct session_ops *const ops_of[] = {
	    [SESSION_DECODER] = &decoder_ops,
	    [SESSION_STILL] = &still_ops,
	    [SESSION_CONVERTER] = &converter_ops,
	    [SESSION_ENCODER] = &encoder_ops,
	};

	enum session_kind kind;
	if (!kind_of(params, &kind))
	{
		*error = VCERR_BADPARAM;
		return NULL;
	}
	if (kind == SESSION_STILL && !image_call)
	{
		*error = VCERR_UNSUPPORTED;
		return NULL;
	}
	struct vc_session *session = AllocMem(sizeof(*session), MEMF_PUBLIC | MEMF_CLEAR);
	if (session == NULL)
	{
		*error = VCERR_NOMEM;
		return NULL;
	}

	session->base = base;
	session->kind = kind;
	session->ops = ops_of[kind];
	session->image_call = image_call;
	memcpy(&session->params, params, umin(params->Size, sizeof(session->params)));
	session->params.Size = sizeof(session->params);
	session->magic = SESSION_MAGIC;
	AddTail((struct List *)&base->sessions, (struct Node *)&session->node); /* listed first: session_close() takes a half-made one */
	base->session_count++;

	*error = session->ops->open(session);
	session->params.Extradata = NULL; /* the caller's, valid during the open only */
	session->params.ExtradataSize = 0;
	if (*error != 0)
	{
		session_close(session, FALSE);
		return NULL;
	}
	return session;
}

/**
 * session_close() - Close a session: its components, its objects, the session (VCB_CloseSession(), the image calls).
 * @session: the session, open or half-made by session_open()
 * @keep:    an image call's that did its work: its components stay for the next call
 *
 * The components go first, so the VPU writes none of the memory freed after
 * them.  The magic is cleared before the free, so a stale handle is refused.
 */
void session_close(struct vc_session *session, BOOL keep)
{
	struct VideoCoreBase *base = session->base;
	struct ExecBase *SysBase = base->SysBase;
	BOOL report = !session->image_call; /* an image call reports its sessions' timings itself */

	components_close(session, keep);
	set_empty(session, &session->input_set);
	set_empty(session, &session->output_set);
	if (session->ops->close != NULL)
		session->ops->close(session);

	Remove((struct Node *)&session->node);
	base->session_count--;
	session->magic = 0;
	FreeMem(session, sizeof(*session));
	dma_pool_shrink(base->mmal.pool); /* a picture's frames: the next one may be another size */
	if (report)
		perf_report(&base->perf); /* a client's run ends with its own lines (profile tier) */
}

/* ---- imports --------------------------------------------------------------------------- */

/**
 * object_import() - An object over the client's memory, added to a side.
 * @session:  the session
 * @side:     VCS_Input or VCS_Output
 * @is_frame: a frame, else a packet
 * @data:     the client's memory
 * @capacity: its bytes
 *
 * The first import on a side replaces the backend's own objects, unless the
 * client was given one of them.  A side is fixed while it is attached.
 *
 * Return: the object, NULL if refused.
 */
static struct vc_object *object_import(struct vc_session *session, ULONG side, BOOL is_frame, APTR data, u32 capacity)
{
	if ((side != VCS_Input && side != VCS_Output) || data == NULL || capacity == 0 || session->failed)
		return NULL;

	struct vc_set *set = side == VCS_Input ? &session->input_set : &session->output_set;
	if (set->port != NULL)
		return NULL;

	if (!set->imported)
	{
		for (u32 i = 0; i < set->count; i++)
			if (set->objects[i]->handed || set->objects[i]->at_client)
				return NULL; /* no mixed set: the client holds or held one of the backend's */

		set_empty(session, set);
		set->imported = TRUE;
	}
	struct vc_object *object = set_new_object(session, set, is_frame, data, capacity);
#ifdef PROFILE
	if (object != NULL)
		object->stamp = get_time(); /* in the client's hands from now: its release measures from here */
#endif
	return object;
}

/**
 * session_import_packet() - A packet over the client's memory (VCB_ImportPacket()): a decoder's input, an encoder's output.
 * @session:  the session
 * @side:     VCS_Input or VCS_Output
 * @data:     the client's memory
 * @capacity: its bytes
 *
 * Return: the packet, NULL if refused.
 */
struct VCPacket *session_import_packet(struct vc_session *session, ULONG side, APTR data, ULONG capacity)
{
	BOOL takes_packets = (session->ops->send_packet != NULL && side == VCS_Input) ||
	                     (session->kind == SESSION_ENCODER && side == VCS_Output);

	struct vc_object *object = takes_packets ? object_import(session, side, FALSE, data, capacity) : NULL;
	return object != NULL ? &object->pub.packet : NULL;
}

/**
 * session_import_frame() - A frame over the client's memory (VCB_ImportFrame()): decoder output, encoder input, either converter side.
 * @session: the session
 * @side:    VCS_Input or VCS_Output
 * @data:    the client's memory, as large as the side's frames
 *
 * The side's frame shape must be known.  An input picture's is resolved at
 * open from VCA_Width/Height.  An output frame's comes with the stream's
 * shape, or a geometry change, or at open from VCA_OutputWidth/Height (the
 * ISP's frame) or VCA_Width/Height (a decoder's native frame).
 *
 * Return: the frame, described, or NULL if refused.
 */
struct VCFrame *session_import_frame(struct vc_session *session, ULONG side, APTR data)
{
	BOOL takes_frames = side == VCS_Input ? session->ops->send_frame != NULL : session->kind != SESSION_ENCODER;
	if (!takes_frames)
		return NULL;

	if (side == VCS_Input)
	{
		struct vc_object *object = object_import(session, side, TRUE, data, session->input_geometry.size);
		if (object == NULL)
			return NULL;
		input_frame_shape(session, object);
		return &object->pub.frame;
	}

	const struct vc_shape *shape = &session->shape;
	if (!shape->resolved)
		return NULL;

	struct vc_object *object = object_import(session, side, TRUE, data, shape->geometry.size);
	if (object == NULL)
		return NULL;

	frame_set_geometry(object, &shape->geometry, shape->format, &shape->dest);
	return &object->pub.frame;
}

/* ---- sends and releases ----------------------------------------------------------------- */

/**
 * session_start() - Start the session at its first send: the sets are final, the kind's components run.
 * @session: the session
 *
 * Return: 0 if it runs, now or already; else the error, and the session has
 * failed.
 */
static LONG session_start(struct vc_session *session)
{
	if (session->started)
		return 0;
	LONG error = session->ops->start(session);
	if (error != 0)
	{
		session_fail(session, error, "start");
		return error;
	}
	session->started = TRUE;
	return 0;
}

/**
 * session_send_packet() - Send a packet to a decoder (VCB_SendPacket()).
 * @session: the session
 * @packet:  one of its input packets, in the client's hands
 *
 * Return: 0, VCERR_UNSUPPORTED for a kind that takes no packets, or the error.
 */
LONG session_send_packet(struct vc_session *session, struct VCPacket *packet)
{
	if (session->ops->send_packet == NULL)
		return VCERR_UNSUPPORTED;

	struct vc_object *object = object_lookup(session, packet, VCS_Input, FALSE);
	if (object == NULL || !object->at_client || session->failed)
		return VCERR_BADSTATE;

	if (packet->Length > object->capacity)
		return VCERR_BADPARAM; /* checked before the start: a bad packet starts nothing */

	LONG error = session_start(session);
	if (error != 0)
		return error;

	if (packet->Length > session->input_set.size)
		return VCERR_BADPARAM; /* the port runs with the smallest imported packet, known from the start */

	return session->ops->send_packet(session, object, packet);
}

/**
 * session_send_frame() - Send a picture to an encoder or converter (VCB_SendFrame()).
 * @session: the session
 * @frame:   one of its input frames, in the client's hands
 *
 * Return: 0, VCERR_UNSUPPORTED for a kind that takes no pictures, or the error.
 */
LONG session_send_frame(struct vc_session *session, struct VCFrame *frame)
{
	if (session->ops->send_frame == NULL)
		return VCERR_UNSUPPORTED;

	struct vc_object *object = object_lookup(session, frame, VCS_Input, TRUE);
	if (object == NULL || !object->at_client || session->failed)
		return VCERR_BADSTATE;

	LONG error = session_start(session);
	if (error != 0)
		return error;
	return session->ops->send_frame(session, object, frame);
}

/**
 * output_object_release() - An output object back from the client goes to the VPU.
 * @session: the session
 * @object:  the output object, in the client's hands
 *
 * While the output does not run (not started yet, or being remade), the
 * object waits: set_send_output() sends it with the rest.
 */
static void output_object_release(struct vc_session *session, struct vc_object *object)
{
	PERF_ADD(&session->base->perf, VCPERF_OUT_CLIENT, object->stamp);
	object->at_client = FALSE;
	if (output_running(session))
		output_object_send(session, object, object->is_frame ? "frame release" : "packet release");
}

/**
 * session_release_frame() - The client is done with a frame (VCB_ReleaseFrame()).
 * @session: the session
 * @frame:   one of its output frames, in the client's hands
 *
 * A kind with a shape that waits for the client's frames applies it once
 * the last is home (frame_released).
 */
void session_release_frame(struct vc_session *session, struct VCFrame *frame)
{
	struct vc_object *object = object_lookup(session, frame, VCS_Output, TRUE);
	if (object == NULL || !object->at_client || session->failed)
		return;
	output_object_release(session, object);
	if (session->ops->frame_released != NULL)
		session->ops->frame_released(session);
}

/**
 * session_release_packet() - The client is done with a packet (VCB_ReleasePacket()).
 * @session: the session
 * @packet:  one of its output packets, in the client's hands
 */
void session_release_packet(struct vc_session *session, struct VCPacket *packet)
{
	struct vc_object *object = object_lookup(session, packet, VCS_Output, FALSE);
	if (object == NULL || !object->at_client || session->failed)
		return;
	output_object_release(session, object);
}

/**
 * ports_flush() - Flush ports in order, until one fails.
 * @session: the session
 * @ports:   the ports; NULL entries are skipped
 * @count:   how many
 *
 * Return: 0 or the first error.
 */
LONG ports_flush(struct vc_session *session, struct mmal_port *const *ports, u32 count)
{
	LONG error = 0;
	for (u32 i = 0; error == 0 && i < count; i++)
		if (ports[i] != NULL)
			error = mmal_port_flush(session->base, ports[i]);
	return error;
}

/**
 * session_flush() - Return everything the VPU holds (VCB_Flush()).
 * @session: the session
 *
 * The kind's ports in the pipeline's order; without a flush of its own, the
 * input and the output while it runs (between the stream's shape and the
 * output's start it is down).  Pictures the VPU had already handed back (in
 * transfer, or queued here) are dropped as they come
 * (poll_output_returned()).
 *
 * Return: 0, VCERR_BADSTATE for a failed session, or the error.
 */
LONG session_flush(struct vc_session *session)
{
	if (session->failed)
		return VCERR_BADSTATE;
	if (session->kind == SESSION_ENCODER && session->tunnelled)
		return VCERR_UNSUPPORTED; /* a flush reaches the tunnel's pictures, and has been known to end the VPU's service */
	if (!session->started)
		return 0;
	session->flushes++; /* what comes back filled before this is stale */
	LONG error;
	if (session->ops->flush != NULL)
		error = session->ops->flush(session);
	else
	{
		struct mmal_port *const ports[] = {session->input_port, session->output_port->enabled ? session->output_port : NULL};
		error = ports_flush(session, ports, ARRAY_SIZE(ports));
	}
	session->end_sent = FALSE; /* a flushed end mark ends nothing */
	return error;
}

/**
 * session_set_attrs() - Change a running session (VCB_SetAttrs()).
 * @session: the session
 * @changes: the parameters, Changed naming which
 *
 * Return: 0, VCERR_UNSUPPORTED for a change the kind cannot make, or the error.
 */
LONG session_set_attrs(struct vc_session *session, const struct VCSessionParams *changes)
{
	if (session->failed)
		return VCERR_BADSTATE;
	if (changes->Changed == 0)
		return 0;
	return session->ops->set_attrs != NULL ? session->ops->set_attrs(session, changes) : VCERR_UNSUPPORTED;
}
