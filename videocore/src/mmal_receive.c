/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * mmal_receive.c - the receive side: what the VPU sends, handled by the
 * receive task under mmal_lock without waiting.  A returned buffer or an
 * event brings its payload inline when short, else by a bulk receive queued
 * in order; once the data is in, it goes to its port's queue.  Replies go to
 * their calls (mmal_call.c).
 *
 * The VPU pairs bulk transfers in the order it announced them, so every
 * payload it sends by bulk is received, or the next receive would take it
 * and every buffer after it would be wrong.  A payload nobody keeps (a stray
 * buffer's, a dropped event's, one longer than its buffer, or one owed to a
 * buffer let go while its receive was parked) goes to a sink: a stand-in
 * buffer of the client's whose receive lands in the scratch memory, cut to
 * its size, in the payload's place in the order.
 *
 * A buffer of a zero-copy port (imported, mmal_zero_copy.c) needs none of
 * that: the VPU says so in the message (is_zero_copy), the payload is in
 * the buffer already, and only the cache lines over it are dropped.
 *
 * Follows userland/interface/mmal/vc/mmal_vc_client.c and mmal_vc_api.c
 * (Copyright (c) 2012, Broadcom Europe Ltd, BSD-3-Clause).
 */
#include <byteorder.h>
#include <memory.h>

#include <videocore_private.h>
#include <mmal_internal.h>

/* event_decode() leaves the decoded format change in the event buffer */
_Static_assert(sizeof(struct mmal_format_change) <= CLIENT_EVENT_BUFFER_SIZE, "a decoded format change fits its event buffer");

/**
 * stray_count() - Count and log a message or completion that matched nothing.
 * @base:   the library base, mmal_lock held
 * @why:    what did not match
 * @detail: a number that says which (a size, a cookie)
 */
static void stray_count(struct VideoCoreBase *base, const char *why, u32 detail)
{
	(void)why; /* only the debug log reads it */
	(void)detail;
	base->mmal.stray_messages++;
	Kprintf("[videocore] stray message: %s (%08lx)\n", why, detail);
}

/* ---- what the VPU names -------------------------------------------------- */

/**
 * component_by_handle() - The component the VPU knows by a handle.
 * @base:   the library base
 * @handle: the VPU's component handle
 *
 * Return: the component, NULL if none has @handle.
 */
static struct mmal_component *component_by_handle(struct VideoCoreBase *base, u32 handle)
{
	for (struct MinNode *node = base->mmal.components.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
		if (((struct mmal_component *)node)->handle == handle)
			return (struct mmal_component *)node;
	return NULL;
}

/**
 * component_by_cookie() - The component an event names by the cookie it was created with.
 * @base:   the library base
 * @cookie: the client_component the VPU echoes
 *
 * Return: the component, NULL if @cookie is none of the open components.
 */
static struct mmal_component *component_by_cookie(struct VideoCoreBase *base, u32 cookie)
{
	/* the cookie is the component's address, as COMPONENT_CREATE sent it; compared, never followed */
	for (struct MinNode *node = base->mmal.components.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
		if ((u32)node == cookie)
			return (struct mmal_component *)node;
	return NULL;
}

/**
 * port_by_handle() - The port of a component the VPU knows by a handle.
 * @component: the component
 * @handle:    the VPU's port handle
 *
 * Return: the port, NULL if none has @handle.
 */
static struct mmal_port *port_by_handle(struct mmal_component *component, u32 handle)
{
	for (u32 i = 0; i < component->port_count; i++)
		if (component->ports[i].handle == handle)
			return &component->ports[i];
	return NULL;
}

/**
 * port_by_number() - The port of a component by its type and index within the type.
 * @component: the component
 * @type:      MMAL_PORT_TYPE_*
 * @number:    the index within @type
 *
 * Return: the port, NULL if the component has no such port.
 */
static struct mmal_port *port_by_number(struct mmal_component *component, u32 type, u32 number)
{
	for (u32 i = 0; i < component->port_count; i++)
		if (component->ports[i].type == type && component->ports[i].index == number)
			return &component->ports[i];
	return NULL;
}

/**
 * attached_buffer_by_cookie() - The buffer attached to a port that a cookie names.
 * @port:   the port
 * @cookie: the client_context the VPU echoes
 *
 * Return: the buffer, NULL if @cookie is none of the attached ones.
 */
static struct mmal_buffer *attached_buffer_by_cookie(struct mmal_port *port, u32 cookie)
{
	/* the cookie is the buffer's address, as BUFFER_FROM_HOST sent it; compared, never followed */
	for (u32 i = 0; i < port->attached_count; i++)
		if ((u32)port->buffers[i] == cookie)
			return port->buffers[i];
	return NULL;
}

/**
 * buffer_by_message() - The buffer a BUFFER_TO_HOST hands back, if it is out.
 * @base:    the library base
 * @message: the message
 *
 * Return: the buffer, NULL if the handles and the cookie name no attached
 * buffer, or name one that was not sent.
 */
static struct mmal_buffer *buffer_by_message(struct VideoCoreBase *base, const struct mmal_msg_buffer *message)
{
	struct mmal_component *component = component_by_handle(base, le32(message->drvbuf.component_handle));
	struct mmal_port *port = component != NULL ? port_by_handle(component, le32(message->drvbuf.port_handle)) : NULL;
	struct mmal_buffer *buffer = port != NULL ? attached_buffer_by_cookie(port, le32(message->drvbuf.client_context)) : NULL;
	return buffer != NULL && buffer->state == BUF_VPU ? buffer : NULL;
}

/* ---- delivery ------------------------------------------------------------ */

/**
 * event_decode() - An event's data in native byte order.
 * @buffer: the event buffer, its data in
 *
 * FORMAT_CHANGED becomes a struct mmal_format_change; any other event is
 * words.
 */
static void event_decode(struct mmal_buffer *buffer)
{
	u8 *data = buffer->data;
	u32 length = buffer->length;

	if (buffer->cmd == MMAL_EVENT_FORMAT_CHANGED && length >= sizeof(struct mmal_wire_format_changed))
	{
		/* the wire form: the buffer needs, the format, its type-specific words, then the extradata */
		const struct mmal_wire_format_changed *wire = (const struct mmal_wire_format_changed *)data;
		struct mmal_format_change change;
		memset(&change, 0, sizeof(change));
		change.buffer_size_min = le32(wire->buffer_size_min);
		change.buffer_num_min = le32(wire->buffer_num_min);
		change.buffer_size_recommended = le32(wire->buffer_size_recommended);
		change.buffer_num_recommended = le32(wire->buffer_num_recommended);
		format_from_wire(&change.format, &wire->es_format, &wire->es, data + sizeof(*wire), length - (u32)sizeof(*wire));
		/* the decoded form replaces the wire form in the same buffer; its length marks it as decoded */
		memcpy(data, &change, sizeof(change));
		buffer->length = sizeof(change);
		return;
	}
	for (u32 i = 0; i + 4u <= length; i += 4u)
		*(u32 *)(data + i) = le32(*(u32 *)(data + i));
}

/**
 * component_signal() - Tell a component's task that a port of it has something.
 * @base:      the library base
 * @component: the component
 */
static void component_signal(struct VideoCoreBase *base, struct mmal_component *component)
{
	struct ExecBase *SysBase = base->SysBase;
	if (component->task != NULL)
		Signal(component->task, component->sigmask);
}

/**
 * event_error() - What an event that is not queued means for its component.
 * @event: the event buffer, decoded, not a whole format change
 *
 * A format change on an output port that did not arrive whole loses the
 * stream's shape.  An ERROR on a control or output port carries the VPU's
 * status; one without a status is still an error.  Anything else means
 * nothing.
 *
 * Return: the VCERR_*, 0 for an event that is no error.
 */
static LONG event_error(const struct mmal_buffer *event)
{
	u32 port_type = event->port->type;
	if (event->cmd == MMAL_EVENT_FORMAT_CHANGED)
		return port_type == MMAL_PORT_TYPE_OUTPUT ? VCERR_TRANSPORT : 0;
	if (event->cmd != MMAL_EVENT_ERROR || (port_type != MMAL_PORT_TYPE_CONTROL && port_type != MMAL_PORT_TYPE_OUTPUT))
		return 0;
	s32 status = event->length >= 4 ? *(const s32 *)event->data : MMAL_EIO; /* the status is the event's first word */
	return vcerr_from_status(status != MMAL_SUCCESS ? status : MMAL_EIO);
}

/**
 * event_keep() - An event's data is in: keep a format change, take an error, drop the rest.
 * @base:  the library base, mmal_lock held
 * @event: the event buffer
 *
 * A whole FORMAT_CHANGED on an output port is queued in order with the
 * port's buffers.  Any other event goes back to the component's event pool;
 * one that is an error (event_error()) becomes the component's error, the
 * first one kept, and wakes its task.
 *
 * Return: TRUE to queue the event, FALSE if it went back to the pool.
 */
static BOOL event_keep(struct VideoCoreBase *base, struct mmal_buffer *event)
{
	struct mmal_component *component = event->component;

	event_decode(event);
	/* only a decoded change has this length; a failed delivery has length 0 */
	BOOL change = event->cmd == MMAL_EVENT_FORMAT_CHANGED && event->length == sizeof(struct mmal_format_change);
#ifdef TRACE
	if (change)
	{
		const struct mmal_format_change *to = (const struct mmal_format_change *)event->data;
		KprintfT("[videocore] %s port %ld (type %ld): format change to %ldx%ld, buffers min %ld x %ld\n", component->name,
		         (ULONG)event->port->index_all, (ULONG)event->port->type, (ULONG)to->format.es.video.width,
		         (ULONG)to->format.es.video.height, (ULONG)to->buffer_num_min, (ULONG)to->buffer_size_min);
	}
#endif
	if (change && event->port->type == MMAL_PORT_TYPE_OUTPUT)
		return TRUE;

	LONG error = event_error(event);
	if (error != 0)
	{
		Kprintf("[videocore] %s port %ld: MMAL event %08lx, error %ld\n", component->name, (ULONG)event->port->index_all, event->cmd,
		        error);
		if (component->error == 0)
			component->error = error;
		component_signal(base, component);
	}
	event_recycle(base, event);
	return FALSE;
}

/**
 * deliver() - Put a buffer the VPU returned on its port's queue and signal the component's task.
 * @base:   the library base, mmal_lock held
 * @buffer: the buffer or event, its data in
 */
static void deliver(struct VideoCoreBase *base, struct mmal_buffer *buffer)
{
	struct ExecBase *SysBase = base->SysBase;
	struct mmal_port *port = buffer->port;

	if (buffer->kind == BUFFER_EVENT && !event_keep(base, buffer))
		return; /* back in the event pool */
	buffer->state = BUF_QUEUED;
	AddTail((struct List *)&port->queue, (struct Node *)&buffer->node);
	component_signal(base, port->component);
}

/**
 * deliver_failed() - Deliver a buffer whose return failed: no data, TRANSMISSION_FAILED set.
 * @base:   the library base, mmal_lock held
 * @buffer: the buffer
 */
static void deliver_failed(struct VideoCoreBase *base, struct mmal_buffer *buffer)
{
	buffer->flags |= MMAL_BUFFER_HEADER_FLAG_TRANSMISSION_FAILED;
	buffer->length = 0;
	deliver(base, buffer);
}

/**
 * rx_queue() - Queue the bulk receive of a buffer's payload.
 * @base:   the library base, mmal_lock held
 * @buffer: the buffer or sink, rx_len set
 *
 * The VPU pairs bulk transfers in the order they were announced, so a
 * receive may never overtake one that waits for a request: the buffer joins
 * the parked ones, and mmal_pump() starts them in order while requests are
 * free.
 */
static void rx_queue(struct VideoCoreBase *base, struct mmal_buffer *buffer)
{
	struct ExecBase *SysBase = base->SysBase;
#ifdef PROFILE
	buffer->rx_at = get_time();
#endif
	buffer->state = BUF_PENDING_RX;
	AddTail((struct List *)&base->mmal.pending_rx, (struct Node *)&buffer->node);
	mmal_pump(base);
}

/* ---- sinks: payloads nobody keeps ------------------------------------------ */

/**
 * sink_new() - A sink for a payload nobody keeps: its receive lands in the scratch memory.
 * @base:   the library base, mmal_lock held
 * @length: the bytes the bulk moves (whole words)
 *
 * The receive is cut to CLIENT_SCRATCH_SIZE; it keeps its place in the
 * pairing all the same.  Without memory for a sink the pairing is lost:
 * logged.
 *
 * Return: the sink, not yet queued, or NULL.
 */
static struct mmal_buffer *sink_new(struct VideoCoreBase *base, u32 length)
{
	struct ExecBase *SysBase = base->SysBase;
	struct mmal_buffer *sink = AllocMem(sizeof(*sink), MEMF_PUBLIC | MEMF_CLEAR);
	if (sink == NULL)
	{
		Kprintf("[videocore] no memory to receive a %ld-byte payload nobody keeps: the bulk pairing is lost\n", length);
		return NULL;
	}
	sink->magic = BUFFER_MAGIC;
	sink->kind = BUFFER_SINK;
	sink->data = base->mmal.scratch;
	sink->alloc_size = CLIENT_SCRATCH_SIZE;
	sink->rx_len = umin(length, CLIENT_SCRATCH_SIZE);
	return sink;
}

/**
 * sink_free() - Free a sink: its receive is over, or will never run.
 * @base: the library base, mmal_lock held
 * @sink: the sink, on no list
 */
static void sink_free(struct VideoCoreBase *base, struct mmal_buffer *sink)
{
	struct ExecBase *SysBase = base->SysBase;
	sink->magic = 0;
	FreeMem(sink, sizeof(*sink));
}

/**
 * payload_discard() - Receive a payload that has no buffer to go to, in its place in the order.
 * @base:   the library base, mmal_lock held
 * @length: the bytes the bulk moves (whole words)
 */
static void payload_discard(struct VideoCoreBase *base, u32 length)
{
	struct mmal_buffer *sink = sink_new(base, length);
	if (sink != NULL)
		rx_queue(base, sink);
}

/**
 * rx_forget() - A buffer let go while it is out (a disable that timed out): nothing of it stays in the receive side.
 * @base:   the library base, mmal_lock held
 * @buffer: the buffer or event buffer, about to lose its memory
 *
 * A receive still parked hands its place in the order to a sink.  A message
 * or receive already at the device stays there in its place, but its
 * completion names nothing (link_disown()).  What the VPU sends for the
 * buffer later is a stray.
 */
void rx_forget(struct VideoCoreBase *base, struct mmal_buffer *buffer)
{
	struct ExecBase *SysBase = base->SysBase;
	if (buffer->state == BUF_PENDING_RX)
	{
		struct mmal_buffer *sink = sink_new(base, buffer->rx_len);
		if (sink != NULL)
		{
			sink->state = BUF_PENDING_RX;
			sink->rx_at = buffer->rx_at;
			Insert((struct List *)&base->mmal.pending_rx, (struct Node *)&sink->node, (struct Node *)&buffer->node);
		}
		Remove((struct Node *)&buffer->node);
		buffer->state = BUF_IDLE;
	}
	else if (buffer->state >= BUF_VPU)
	{
		link_disown(base, buffer);
		buffer->state = BUF_IDLE;
	}
}

/**
 * rx_parked_drop() - The service closed: the parked receives will never run.
 * @base: the library base, mmal_lock held
 *
 * Only sinks are parked by then: every buffer was let go with its component.
 */
void rx_parked_drop(struct VideoCoreBase *base)
{
	struct ExecBase *SysBase = base->SysBase;
	struct mmal_buffer *parked;
	while ((parked = (struct mmal_buffer *)RemHead((struct List *)&base->mmal.pending_rx)) != NULL)
		if (parked->kind == BUFFER_SINK)
			sink_free(base, parked);
}

/* ---- messages ------------------------------------------------------------ */

/**
 * payload_bulk_size() - What a returned buffer's payload comes by: the bulk that follows the message, if any.
 * @message: the BUFFER_TO_HOST
 *
 * The payload comes not at all (a failed return, an input buffer the VPU
 * consumed, one returned at a disable, an imported buffer the VPU wrote
 * itself), inline when short, or by bulk (an output buffer it filled).  A
 * bare end of stream comes with an 8-byte dummy bulk, as in userland,
 * unless the buffer is imported.
 *
 * Return: the bulk's bytes in whole words, 0 when none follows.
 */
static u32 payload_bulk_size(const struct mmal_msg_buffer *message)
{
	u32 length = le32(message->buffer_header.length);
	if (le32(message->h.status) != MMAL_SUCCESS || le32(message->is_zero_copy) != 0)
		return 0;
	if (length == 0)
		return (le32(message->buffer_header.flags) & MMAL_BUFFER_HEADER_FLAG_EOS) ? 8u : 0u;
	return le32(message->payload_in_message) != 0 ? 0u : ALIGN_UP(length, 4u);
}

/**
 * buffer_payload() - Bring a returned buffer's payload in, then deliver the buffer.
 * @base:    the library base, mmal_lock held
 * @buffer:  the buffer, its header taken
 * @message: the BUFFER_TO_HOST that returns it
 */
static void buffer_payload(struct VideoCoreBase *base, struct mmal_buffer *buffer, const struct mmal_msg_buffer *message)
{
	u32 status = le32(message->h.status);
	u32 inline_length = le32(message->payload_in_message);
	u32 bulk = payload_bulk_size(message);

	if (status != MMAL_SUCCESS)
	{
		Kprintf("[videocore] buffer returned with status %ld\n", status);
		deliver_failed(base, buffer);
	}
	else if (le32(message->is_zero_copy) != 0)
	{
		if (zero_copy_landed(base, buffer, le32(message->buffer_header.offset)))
			deliver(base, buffer);
		else
			deliver_failed(base, buffer);
	}
	else if (bulk != 0 && buffer->length > buffer->alloc_size)
	{
		/* more than the VPU was told the buffer holds: the payload goes to a sink, the buffer comes back failed */
		Kprintf("[videocore] %ld bytes for a %ld-byte buffer\n", buffer->length, buffer->alloc_size);
		payload_discard(base, bulk);
		deliver_failed(base, buffer);
	}
	else if (bulk != 0)
	{
		buffer->rx_len = bulk; /* an output buffer's alloc_size is whole words too */
		rx_queue(base, buffer);
	}
	else if (buffer->length == 0)
		deliver(base, buffer); /* nothing to bring in */
	else if (inline_length > MMAL_WIRE_SHORT_DATA || inline_length > buffer->alloc_size)
		deliver_failed(base, buffer);
	else
	{
		memcpy(buffer->data, message->short_data, inline_length);
		buffer->length = inline_length;
		deliver(base, buffer);
	}
}

/**
 * buffer_to_host() - The VPU hands a buffer back (BUFFER_TO_HOST).
 * @base: the library base, mmal_lock held
 * @data: the message
 * @size: its bytes
 */
static void buffer_to_host(struct VideoCoreBase *base, const u8 *data, u32 size)
{
	struct mmal_msg_buffer message;
	const u32 needed = (u32)__builtin_offsetof(struct mmal_msg_buffer, pad); /* all of it but the tail padding */

	if (size < needed)
	{
		stray_count(base, "a buffer message cut short", size);
		return;
	}
	memcpy(&message, data, needed); /* out of the device's buffer, aligned for the fields */
	base->mmal.buffers_back++;
	KprintfT("[videocore] buffer %08lx back on port handle %ld: length %ld flags %08lx inline %ld status %ld\n",
	         le32(message.drvbuf.client_context), le32(message.drvbuf.port_handle), le32(message.buffer_header.length),
	         le32(message.buffer_header.flags), le32(message.payload_in_message), le32(message.h.status));

	struct mmal_buffer *buffer = buffer_by_message(base, &message);
	if (buffer == NULL)
	{
		stray_count(base, "a buffer back that was not out", le32(message.drvbuf.client_context));
		u32 bulk = payload_bulk_size(&message);
		if (bulk != 0)
			payload_discard(base, bulk);
		return;
	}
	buffer_from_wire(buffer, &message);
	buffer_payload(base, buffer, &message);
}

/**
 * event_buffer_take() - An event buffer for an event the VPU sends, if the event can be taken.
 * @base:    the library base, mmal_lock held
 * @message: the event message
 * @room:    the payload bytes the message holds
 * @why:     the reason, when the event cannot be taken
 *
 * The event must name a port of an open component, fit an event buffer and,
 * when its payload rides in the message, have come whole; and the
 * component's pool must have a buffer free.
 *
 * Return: a buffer off the component's pool, its port set, or NULL with @why set.
 */
static struct mmal_buffer *event_buffer_take(struct VideoCoreBase *base, const struct mmal_msg_event *message, u32 room,
                                             const char **why)
{
	struct mmal_component *component = component_by_cookie(base, le32(message->client_component));
	struct mmal_port *port = component != NULL ? port_by_number(component, le32(message->port_type), le32(message->port_num)) : NULL;
	u32 length = le32(message->length);

	*why = NULL;
	if (port == NULL)
		*why = "no such port";
	else if (length > CLIENT_EVENT_BUFFER_SIZE)
		*why = "longer than an event buffer";
	else if (length <= MMAL_WIRE_EVENT_SPACE && length > room)
		*why = "cut short";
	if (*why != NULL)
		return NULL;

	struct ExecBase *SysBase = base->SysBase;
	struct mmal_buffer *buffer = (struct mmal_buffer *)RemHead((struct List *)&component->free_events);
	if (buffer == NULL)
	{
		*why = "no event buffer free";
		return NULL;
	}
	buffer->port = port;
	return buffer;
}

/**
 * event_to_host() - The VPU sends an event (EVENT_TO_HOST): it takes a buffer of the component's event pool.
 * @base: the library base, mmal_lock held
 * @data: the message
 * @size: its bytes
 *
 * A short payload rides in the message; a longer one follows by bulk
 * transfer.  An event that cannot be taken (event_buffer_take()) is dropped
 * and counted, its bulk received by a sink.
 */
static void event_to_host(struct VideoCoreBase *base, const u8 *data, u32 size)
{
	struct mmal_client *client = &base->mmal;
	struct mmal_msg_event message;
	const u32 fixed = (u32)__builtin_offsetof(struct mmal_msg_event, data); /* the header and the event's identity */

	client->events++;
	if (size < fixed)
	{
		Kprintf("[videocore] MMAL event message of %ld bytes dropped\n", size);
		client->events_dropped++;
		return;
	}
	/* the message ends after its payload: the rest of the structure reads as 0 */
	memset(&message, 0, sizeof(message));
	memcpy(&message, data, umin(size, (u32)sizeof(message)));
	u32 length = le32(message.length);
	KprintfT("[videocore] MMAL event %08lx for %08lx port %ld/%ld, %ld bytes\n", le32(message.cmd), le32(message.client_component),
	         le32(message.port_type), le32(message.port_num), length);

	const char *why;
	struct mmal_buffer *buffer = event_buffer_take(base, &message, size - fixed, &why);
	if (buffer == NULL)
	{
		Kprintf("[videocore] MMAL event %08lx for %08lx port %ld/%ld (%ld bytes) dropped: %s\n", le32(message.cmd),
		        le32(message.client_component), le32(message.port_type), le32(message.port_num), length, why);
		client->events_dropped++;
		if (length > MMAL_WIRE_EVENT_SPACE)
			payload_discard(base, ALIGN_UP(length, 4u));
		return;
	}
	buffer->cmd = le32(message.cmd);
	buffer->length = length;
	buffer->flags = 0;
	buffer->pts = MMAL_TIME_UNKNOWN; /* events carry no time */
	if (length > MMAL_WIRE_EVENT_SPACE)
	{
		buffer->rx_len = ALIGN_UP(length, 4u);
		rx_queue(base, buffer);
		return;
	}
	memcpy(buffer->data, message.data, length);
	deliver(base, buffer);
}

/**
 * mmal_message() - One message from the VPU, copied out of shared memory by vchiq.device.
 * @base: the library base, mmal_lock held (the receive task)
 * @data: the message
 * @size: its bytes
 */
void mmal_message(struct VideoCoreBase *base, const u8 *data, u32 size)
{
	if (size < sizeof(struct mmal_msg_header))
	{
		stray_count(base, "a message shorter than its header", size);
		return;
	}
	const struct mmal_msg_header *header = (const struct mmal_msg_header *)data;
	u32 msgid = le32(header->msgid);
	if (le32(header->magic) != MMAL_WIRE_MAGIC)
		stray_count(base, "a message without the magic", le32(header->magic));
	else if (msgid == MMAL_MSG_BUFFER_TO_HOST)
		buffer_to_host(base, data, size);
	else if (msgid == MMAL_MSG_EVENT_TO_HOST)
		event_to_host(base, data, size);
	else if (!call_reply(base, data, size, le32(header->context))) /* the VPU sends nothing else unasked */
		stray_count(base, "a reply no call waits for", le32(header->context));
}

/* ---- the link's completions ---------------------------------------------- */

/**
 * mmal_pump() - Start the parked bulk receives, in order, while requests are free.
 * @base: the library base, mmal_lock held (the receive task)
 */
void mmal_pump(struct VideoCoreBase *base)
{
	struct ExecBase *SysBase = base->SysBase;
	struct mmal_client *client = &base->mmal;

	while (client->pending_rx.mlh_Head->mln_Succ != NULL)
	{
		struct mmal_buffer *buffer = (struct mmal_buffer *)client->pending_rx.mlh_Head;
		s32 status = link_bulk_receive(base, buffer->data, buffer->rx_len, buffer);
		if (status == LINK_ERR_NOSPACE)
			break; /* no request free: the next completion calls again */
		Remove((struct Node *)&buffer->node);
		PERF_ADD(&base->perf, VCPERF_RX_PARKED, buffer->rx_at);
		if (status == 0)
			buffer->state = BUF_RX;
		else if (buffer->kind == BUFFER_SINK)
			sink_free(base, buffer); /* the link is closed or dead */
		else
			deliver_failed(base, buffer);
	}
}

/**
 * mmal_rx_done() - A bulk receive finished: the buffer goes to its port's queue, a sink is freed.
 * @base:   the library base, mmal_lock held (the receive task)
 * @buffer: the buffer or sink the payload went into; NULL for a buffer let go meanwhile (rx_forget())
 * @actual: bytes received, or -1 if the transfer failed
 */
void mmal_rx_done(struct VideoCoreBase *base, struct mmal_buffer *buffer, s32 actual)
{
	if (buffer == NULL)
	{
		stray_count(base, "a bulk receive into a buffer let go", (u32)actual); /* its memory's owner may have it back by now */
		return;
	}
	if (buffer->kind == BUFFER_SINK)
	{
		sink_free(base, buffer);
		return;
	}
	if (buffer->magic != BUFFER_MAGIC || buffer->state != BUF_RX)
	{
		stray_count(base, "a bulk receive for no buffer", (u32)buffer);
		return;
	}
	if (actual < 0)
	{
		deliver_failed(base, buffer);
		return;
	}
	PERF_ADD(&base->perf, VCPERF_RX_PAYLOAD, buffer->rx_at);
	if ((u32)actual < buffer->length)
		buffer->length = (u32)actual; /* short: the VPU moved less than it announced */
	deliver(base, buffer);
}

/**
 * mmal_buffer_failed() - A buffer's message did not reach the VPU: the buffer comes back failed.
 * @base:   the library base, mmal_lock held (the receive task)
 * @buffer: the buffer the message carried; NULL for one let go meanwhile (rx_forget())
 */
void mmal_buffer_failed(struct VideoCoreBase *base, struct mmal_buffer *buffer)
{
	if (buffer != NULL && buffer->magic == BUFFER_MAGIC && buffer->state == BUF_VPU)
		deliver_failed(base, buffer);
}
