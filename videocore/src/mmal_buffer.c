/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * mmal_buffer.c - buffers: the caller's, attached to a port, sent to the
 * VPU and taken back off the port's queue; events back to their pool.
 * Whether a port's buffers are imported into the VPU, and the cache work on
 * an imported one, is mmal_zero_copy.c's.
 *
 * Follows userland/interface/mmal/vc/mmal_vc_client.c and mmal_vc_api.c
 * (Copyright (c) 2012, Broadcom Europe Ltd, BSD-3-Clause).
 */
#include <byteorder.h>
#include <memory.h>

#include <videocore_private.h>
#include <mmal_internal.h>

/**
 * port_buffers_home() - Whether every buffer attached to a port is home (a link_wait() condition).
 * @base: the library base
 * @port: the port
 *
 * Return: TRUE if each is queued or the caller's: none at the VPU, none in a bulk.
 */
BOOL port_buffers_home(APTR base, APTR port)
{
	(void)base;
	const struct mmal_port *attached_to = port;
	for (u32 i = 0; i < attached_to->attached_count; i++)
		if (attached_to->buffers[i]->state >= BUF_VPU)
			return FALSE;
	return TRUE;
}

/**
 * port_buffers_forget() - Let go of a port's attached buffers.
 * @base: the library base, mmal_lock held
 * @port: the port
 *
 * Each comes off the port's queue if it is there, or out of the receive
 * side's bookkeeping if it is still out (rx_forget(): a disable that timed
 * out), its import is let go (imports_release(), which does not wait: the
 * callers that hand the memory back wait with smem_released_wait()), and it
 * loses its port and magic: the VPU returning it later is a stray.
 * buffer_num and buffer_size stay: a format commit sends them, and the VPU
 * rejects zeros; so does the port's ZERO_COPY, which the next attach sets
 * again only if it changes.
 */
void port_buffers_forget(struct VideoCoreBase *base, struct mmal_port *port)
{
	struct ExecBase *SysBase = base->SysBase;

	for (u32 i = 0; i < port->attached_count; i++)
	{
		struct mmal_buffer *buffer = port->buffers[i];
		if (buffer->state == BUF_QUEUED)
		{
			Remove((struct Node *)&buffer->node);
			buffer->state = BUF_IDLE;
		}
		rx_forget(base, buffer);
	}
	imports_release(base, port);
	for (u32 i = 0; i < port->attached_count; i++)
	{
		port->buffers[i]->magic = 0;
		port->buffers[i]->port = NULL;
	}
	port->attached_count = 0;
}

/**
 * port_buffers_release() - Let go of a port's buffers and wait for the VPU to let go of their imports.
 * @base: the library base, mmal_lock held (released while waiting)
 * @port: the port
 *
 * The memory is the caller's again on return.
 */
void port_buffers_release(struct VideoCoreBase *base, struct mmal_port *port)
{
	port_buffers_forget(base, port);
	smem_released_wait(base);
}

/**
 * mmal_port_attach() - Give a port the caller's buffers to run with.
 * @base:    the library base
 * @port:    the port, none attached
 * @buffers: the buffers, each idle and attached nowhere, data set: 4-byte
 *           aligned, DMA-reachable (the backend's own or a screen's bitmap)
 * @count:   how many, 1..CLIENT_PORT_BUFFERS_MAX
 * @size:    the bytes of each; on an output port rounded down to a multiple
 *           of 4, so that a bulk receive, which rounds up, stays inside
 *
 * The buffers' shape becomes the port's buffer_num and buffer_size, which
 * the enable sends.  They stay the caller's memory; the port only knows
 * them until mmal_port_detach().  When every one can be imported into the
 * VPU, they are, and the port runs zero copy (zero_copy_choose()): the
 * attach then waits for the VPU.
 *
 * Return: 0, VCERR_BADPARAM, VCERR_BADSTATE if the port has buffers, or the
 * error of a ZERO_COPY that could not be cleared (nothing attached then).
 */
LONG mmal_port_attach(struct VideoCoreBase *base, struct mmal_port *port, struct mmal_buffer *const *buffers, u32 count, u32 size)
{
	struct ExecBase *SysBase = base->SysBase;

	if (port->type == MMAL_PORT_TYPE_OUTPUT)
		size &= ~3u;
	if (count == 0 || count > CLIENT_PORT_BUFFERS_MAX || size == 0)
		return VCERR_BADPARAM;

	for (u32 i = 0; i < count; i++)
		if (buffers[i]->data == NULL || ((u32)buffers[i]->data & 3u) != 0 || buffers[i]->port != NULL)
			return VCERR_BADPARAM;

	ObtainSemaphore(&base->mmal_lock);
	LONG error = VCERR_BADSTATE;
	if (port->attached_count == 0)
	{
		for (u32 i = 0; i < count; i++)
		{
			struct mmal_buffer *buffer = buffers[i];
			buffer->port = port;
			buffer->magic = BUFFER_MAGIC;
			buffer->kind = BUFFER_DATA;
			buffer->state = BUF_IDLE;
			buffer->alloc_size = size;
			buffer->import = 0;
			port->buffers[i] = buffer;
		}
		port->attached_count = count;
		port->buffer_num = count;
		port->buffer_size = size;
		error = vcerr_from_status(zero_copy_choose(base, port));
		if (error != 0)
			port_buffers_forget(base, port);
	}
	ReleaseSemaphore(&base->mmal_lock);
	return error;
}

/**
 * mmal_port_detach() - Take a port's buffers back.
 * @base: the library base
 * @port: the port, disabled
 *
 * The ones the VPU returned come off the port's queue, events stay on it.
 * Imported ones are let go, and the VPU's word that it let go of them is
 * waited for: the memory is the caller's again on return.
 *
 * Return: 0 (also with none attached), or VCERR_BADSTATE while the port is
 * enabled or a buffer is still out.
 */
LONG mmal_port_detach(struct VideoCoreBase *base, struct mmal_port *port)
{
	struct ExecBase *SysBase = base->SysBase;

	ObtainSemaphore(&base->mmal_lock);
	LONG error = 0;
	if (port->enabled || !port_buffers_home(base, port))
		error = VCERR_BADSTATE; /* disable the port first; its buffers come home then */
	else
		port_buffers_release(base, port);
	ReleaseSemaphore(&base->mmal_lock);
	return error;
}

/**
 * mmal_port_take() - Take the next buffer or format change a port's queue holds.
 * @base: the library base
 * @port: the port
 *
 * A format change has cmd set; it goes back with mmal_event_done().
 *
 * Return: the buffer or event, now the caller's, or NULL if the queue is empty.
 */
struct mmal_buffer *mmal_port_take(struct VideoCoreBase *base, struct mmal_port *port)
{
	struct ExecBase *SysBase = base->SysBase;

	ObtainSemaphore(&base->mmal_lock);
	struct mmal_buffer *buffer = (struct mmal_buffer *)RemHead((struct List *)&port->queue);
	if (buffer != NULL)
		buffer->state = BUF_IDLE;
	ReleaseSemaphore(&base->mmal_lock);
	return buffer;
}

/**
 * event_recycle() - Put an event buffer in its component's pool.
 * @base:  the library base, mmal_lock held
 * @event: the event buffer, on no list
 */
void event_recycle(struct VideoCoreBase *base, struct mmal_buffer *event)
{
	struct ExecBase *SysBase = base->SysBase;
	event->state = BUF_FREE;
	AddTail((struct List *)&event->component->free_events, (struct Node *)&event->node);
}

/**
 * mmal_event_done() - Give an event mmal_port_take() handed out back to its component's pool.
 * @base:  the library base
 * @event: the event
 */
void mmal_event_done(struct VideoCoreBase *base, struct mmal_buffer *event)
{
	struct ExecBase *SysBase = base->SysBase;

	ObtainSemaphore(&base->mmal_lock);
	if (event->kind == BUFFER_EVENT && event->state == BUF_IDLE)
		event_recycle(base, event);
	ReleaseSemaphore(&base->mmal_lock);
}

/**
 * buffer_message() - Send a buffer to the VPU: BUFFER_FROM_HOST and the payload's bulk transmit.
 * @base:   the library base, mmal_lock held
 * @buffer: the buffer, filled, off every list
 *
 * An imported buffer is named by its handle and needs no bulk (the VPU
 * reads and writes it in place), only the cache work (zero_copy_arm()).
 * Otherwise the payload follows by bulk, landing at the start of the VPU's
 * buffer; a zero-length end-of-stream marker travels as
 * BUFFER_FROM_HOST_ZEROLEN with a dummy bulk where the VPU knows the form,
 * so it cannot overtake the data before it.
 *
 * Return: MMAL_SUCCESS, or the link's LINK_ERR_*.
 */
static s32 buffer_message(struct VideoCoreBase *base, struct mmal_buffer *buffer)
{
	struct mmal_port *port = buffer->port;
	struct mmal_msg_buffer request;
	buffer_to_wire(buffer, &request);

	if (buffer->import != 0)
	{
		request.buffer_header.data = le32(smem_handle(base, buffer->import));
		zero_copy_arm(base, buffer);
		header_to_wire(&request.h, MMAL_MSG_BUFFER_FROM_HOST, 0);
		return link_send_buffer(base, &request, sizeof(request), buffer, NULL, 0);
	}

	u32 msgid = MMAL_MSG_BUFFER_FROM_HOST;
	u32 length = buffer->length;
	if (length == 0 && (buffer->flags & MMAL_BUFFER_HEADER_FLAG_EOS) && vpu_has_zerolen(base))
	{
		length = 8;
		msgid = MMAL_MSG_BUFFER_FROM_HOST_ZEROLEN;
	}
	if (length != 0)
		port->sent_data = TRUE;
	header_to_wire(&request.h, msgid, 0);
	/* the message and its payload's bulk go out together; a message that fails later brings the buffer back failed */
	return link_send_buffer(base, &request, sizeof(request), buffer, length != 0 ? buffer->data : NULL, ALIGN_UP(length, 4u));
}

/**
 * mmal_buffer_send() - Send a buffer to its port: input data to consume, or an output buffer to fill.
 * @base:   the library base
 * @buffer: an attached buffer, the caller's (or still on the queue), its port enabled
 * @length: bytes of data from the start of the buffer; 0 for an output buffer
 * @flags:  MMAL_BUFFER_HEADER_FLAG_*
 * @pts:    its presentation time, or MMAL_TIME_UNKNOWN
 *
 * The command and the plane count are reset.
 *
 * Return: 0, VCERR_BADPARAM for a buffer that cannot go (the caller keeps
 * it), or the link's error.
 */
LONG mmal_buffer_send(struct VideoCoreBase *base, struct mmal_buffer *buffer, u32 length, u32 flags, s64 pts)
{
	struct ExecBase *SysBase = base->SysBase;

	ObtainSemaphore(&base->mmal_lock);
	s32 status = MMAL_EINVAL;
	if (buffer->kind == BUFFER_DATA && buffer->port != NULL && buffer->port->enabled && buffer->state <= BUF_QUEUED &&
	    length <= buffer->alloc_size)
	{
		buffer->cmd = 0;
		buffer->length = length;
		buffer->flags = flags;
		buffer->pts = pts;
		buffer->video.planes = 0;
		if (buffer->state == BUF_QUEUED)
			Remove((struct Node *)&buffer->node); /* straight off the queue */
		buffer->state = BUF_VPU;
		status = buffer_message(base, buffer);
		if (status != MMAL_SUCCESS)
			buffer->state = BUF_IDLE; /* the caller keeps it */
	}
	ReleaseSemaphore(&base->mmal_lock);
	return vcerr_from_status(status);
}
