/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * mmal_call.c - control messages: the calling task sends one and sleeps in
 * link_wait() until the reply its waiter's cookie names comes, the send
 * fails or the service goes; and the messages built on that (component,
 * port info, port actions, the synchronising flush).
 *
 * Follows userland/interface/mmal/vc/mmal_vc_client.c and mmal_vc_api.c
 * (Copyright (c) 2012, Broadcom Europe Ltd, BSD-3-Clause).
 */
#include <byteorder.h>
#include <memory.h>

#include <videocore_private.h>
#include <mmal_internal.h>

/* ---- calls and their replies --------------------------------------------- */

/**
 * waiter_done() - Whether a call's reply came, or the call failed (a link_wait() condition).
 * @base:   the library base
 * @waiter: the call's waiter
 *
 * Return: TRUE once the waiter is done.
 */
static BOOL waiter_done(APTR base, APTR waiter)
{
	(void)base;
	return ((struct mmal_waiter *)waiter)->state == WAITER_DONE;
}

/**
 * waiter_by_cookie() - The waiting call a cookie names.
 * @client: the client
 * @cookie: the waiter's index and generation, as call_vpu() made it
 *
 * Return: the waiter, NULL if the cookie names no call that still waits.
 */
static struct mmal_waiter *waiter_by_cookie(struct mmal_client *client, u32 cookie)
{
	u32 index = cookie & 0xffu;
	struct mmal_waiter *waiter = index < CLIENT_WAITERS_MAX ? &client->waiters[index] : NULL;
	if (waiter == NULL || waiter->state != WAITER_WAITING || waiter->generation != (u16)(cookie >> 8))
		return NULL;
	return waiter;
}

/**
 * waiter_finish() - End a call's wait.
 * @waiter: the waiter
 * @status: MMAL_SUCCESS with the reply in, else why not
 */
static void waiter_finish(struct mmal_waiter *waiter, s32 status)
{
	waiter->status = status;
	waiter->state = WAITER_DONE;
}

/**
 * call_vpu() - Send a control message and wait for its reply.
 * @base:       the library base, mmal_lock held (released while waiting)
 * @message:    the message, its header filled here
 * @size:       its bytes
 * @msgid:      MMAL_MSG_*
 * @reply:      where the reply goes
 * @reply_size: in: room at @reply; out: bytes of the reply
 * @needed:     the bytes the reply must have for its status word to count;
 *              0 for a reply without one (the version)
 * @bulk:       a bulk transmit to follow the message (the synchronising flush), or NULL
 * @bulk_size:  its bytes
 *
 * A reply that never comes means the VPU stopped answering: the link is
 * taken as dead (link_dead()), so the calls after this one fail at once
 * instead of each waiting CLIENT_WAIT_MS, and closing a session does not
 * hang for half a minute.
 *
 * Return: the reply's status (MMAL_SUCCESS with the reply copied), or
 * LINK_ERR_* when the message did not go, no reply came, or the reply is
 * shorter than @needed.
 */
s32 call_vpu(struct VideoCoreBase *base, void *message, u32 size, u32 msgid, void *reply, u32 *reply_size, u32 needed, APTR bulk,
             u32 bulk_size)
{
	struct mmal_client *client = &base->mmal;
	if (!link_ready(&base->link))
		return LINK_ERR_STATE;

	u32 index = 0;
	while (index < CLIENT_WAITERS_MAX && client->waiters[index].state != WAITER_FREE)
		index++;
	if (index == CLIENT_WAITERS_MAX)
		return LINK_ERR_NOSPACE;

	struct mmal_waiter *waiter = &client->waiters[index];
	if (++client->generation == 0)
		client->generation = 1;
	waiter->state = WAITER_WAITING;
	waiter->status = MMAL_SUCCESS;
	waiter->generation = client->generation;
	waiter->reply = reply;
	waiter->reply_size = *reply_size;
	u32 cookie = ((u32)waiter->generation << 8) | index;
	header_to_wire(message, msgid, cookie);

	s32 status = link_send_call(base, message, size, cookie, bulk, bulk_size);
	if (status == 0)
		status = link_wait(base, waiter_done, waiter, CLIENT_WAIT_MS);
	if (status == 0)
		status = waiter->status;
	if (status == MMAL_SUCCESS)
	{
		*reply_size = waiter->reply_size;
		if (needed != 0)
			status = status_from_wire(reply, *reply_size, needed);
	}
	else
		Kprintf("[videocore] message %ld: %s (%ld)\n", msgid, status == LINK_ERR_TIMEOUT ? "no reply" : "not sent", status);
	waiter->state = WAITER_FREE;
	if (status == LINK_ERR_TIMEOUT)
	{
		Kprintf("[videocore] the VPU did not answer in %ld ms: the mmal service is taken as gone\n", (ULONG)CLIENT_WAIT_MS);
		link_dead(base);
	}
	return status;
}

/**
 * call_vpu_status() - Send a control message whose reply is just a status.
 * @base:    the library base, mmal_lock held
 * @message: the message
 * @size:    its bytes
 * @msgid:   MMAL_MSG_*
 *
 * Return: the VPU's status, or LINK_ERR_*.
 */
s32 call_vpu_status(struct VideoCoreBase *base, void *message, u32 size, u32 msgid)
{
	struct mmal_msg_reply reply;
	u32 reply_size = sizeof(reply);
	return call_vpu(base, message, size, msgid, &reply, &reply_size, sizeof(reply), NULL, 0);
}

/**
 * call_reply() - A reply to a control message: copied to its waiter, which is done.
 * @base:    the library base, mmal_lock held (the receive task)
 * @data:    the message
 * @size:    its bytes
 * @context: the cookie of the call, waiter index and generation
 *
 * Return: TRUE, or FALSE if no call waits for it.
 */
BOOL call_reply(struct VideoCoreBase *base, const u8 *data, u32 size, u32 context)
{
	struct mmal_waiter *waiter = waiter_by_cookie(&base->mmal, context);
	if (waiter == NULL)
		return FALSE;
	u32 copied = umin(size, waiter->reply_size);
	memcpy(waiter->reply, (const void *)data, copied);
	waiter->reply_size = copied;
	waiter_finish(waiter, MMAL_SUCCESS);
	return TRUE;
}

/**
 * mmal_call_failed() - A control message did not reach the VPU: its call fails, if it still waits.
 * @base:   the library base, mmal_lock held (the receive task)
 * @cookie: the call's cookie, as the message carried it
 * @status: what the call returns, a LINK_ERR_*
 *
 * A call that already timed out is not waiting any more; its slot may serve
 * another call by now, which the cookie's generation tells apart.
 */
void mmal_call_failed(struct VideoCoreBase *base, u32 cookie, s32 status)
{
	struct mmal_waiter *waiter = waiter_by_cookie(&base->mmal, cookie);
	if (waiter != NULL)
		waiter_finish(waiter, status);
}

/**
 * mmal_lost() - The service went: every waiting call fails now rather than at its timeout.
 * @base: the library base, mmal_lock held (the receive task)
 */
void mmal_lost(struct VideoCoreBase *base)
{
	for (u32 i = 0; i < CLIENT_WAITERS_MAX; i++)
	{
		struct mmal_waiter *waiter = &base->mmal.waiters[i];
		if (waiter->state == WAITER_WAITING)
			waiter_finish(waiter, LINK_ERR_STATE);
	}
}

/* ---- the messages -------------------------------------------------------- */

/**
 * component_call() - Send a message that names only a component (destroy, enable, disable).
 * @base:      the library base, mmal_lock held
 * @component: the component
 * @msgid:     MMAL_MSG_COMPONENT_*
 *
 * Return: the VPU's status, or LINK_ERR_*.
 */
s32 component_call(struct VideoCoreBase *base, struct mmal_component *component, u32 msgid)
{
	struct mmal_msg_component_handle request;
	memset(&request, 0, sizeof(request));
	request.component_handle = le32(component->handle);
	return call_vpu_status(base, &request, sizeof(request), msgid);
}

/**
 * port_info_apply() - Take a port info reply: the handle, the state, the format.
 * @port:  the port
 * @reply: the VPU's reply
 */
static void port_info_apply(struct mmal_port *port, const struct mmal_msg_port_info *reply)
{
	port->handle = le32(reply->port_handle);
	port_from_wire(port, &reply->port);
	format_from_wire(&port->format, &reply->format, &reply->es, reply->extradata, MMAL_WIRE_EXTRADATA_MAX);
}

/**
 * port_info_exchange() - Send a port info message and take the VPU's answer.
 * @base:    the library base, mmal_lock held
 * @port:    the port
 * @request: PORT_INFO_GET or PORT_INFO_SET, filled
 * @size:    its bytes
 * @msgid:   MMAL_MSG_PORT_INFO_GET or MMAL_MSG_PORT_INFO_SET
 *
 * Return: the VPU's status, or LINK_ERR_*.
 */
static s32 port_info_exchange(struct VideoCoreBase *base, struct mmal_port *port, void *request, u32 size, u32 msgid)
{
	struct mmal_msg_port_info reply;
	u32 reply_size = sizeof(reply);
	s32 status = call_vpu(base, request, size, msgid, &reply, &reply_size, sizeof(reply), NULL, 0);
	if (status == MMAL_SUCCESS)
		port_info_apply(port, &reply);
	return status;
}

/**
 * port_info_get() - Read a port's state and format from the VPU.
 * @base: the library base, mmal_lock held
 * @port: the port
 *
 * Return: the VPU's status, or LINK_ERR_*.
 */
s32 port_info_get(struct VideoCoreBase *base, struct mmal_port *port)
{
	struct mmal_msg_port_info_get request;
	memset(&request, 0, sizeof(request));
	request.component_handle = le32(port->component->handle);
	request.port_type = le32(port->type);
	request.index = le32(port->index);
	return port_info_exchange(base, port, &request, sizeof(request), MMAL_MSG_PORT_INFO_GET);
}

/**
 * port_info_set() - Send a port's buffer shape and format to the VPU and take its answer.
 * @base: the library base, mmal_lock held
 * @port: the port
 *
 * Return: the VPU's status, or LINK_ERR_*.
 */
s32 port_info_set(struct VideoCoreBase *base, struct mmal_port *port)
{
	struct mmal_msg_port_info_set request;
	memset(&request, 0, sizeof(request));
	request.component_handle = le32(port->component->handle);
	request.port_type = le32(port->type);
	request.index = le32(port->index);
	port_to_wire(port, &request.port);
	format_to_wire(&port->format, &request.format, &request.es, request.extradata);
	return port_info_exchange(base, port, &request, sizeof(request), MMAL_MSG_PORT_INFO_SET);
}

/**
 * port_action() - Send a port action.
 * @base:   the library base, mmal_lock held
 * @port:   the port
 * @action: MMAL_ACTION_*
 * @peer:   the input port for MMAL_ACTION_CONNECT, else NULL
 *
 * ENABLE and SET_REQUIREMENTS carry the port's shape, CONNECT the peer's
 * handles, the rest nothing.  A failure is logged with the VPU's status.
 *
 * Return: the VPU's status, or LINK_ERR_*.
 */
s32 port_action(struct VideoCoreBase *base, struct mmal_port *port, u32 action, const struct mmal_port *peer)
{
	struct mmal_msg_port_action request;
	memset(&request, 0, sizeof(request));
	request.component_handle = le32(port->component->handle);
	request.port_handle = le32(port->handle);
	request.action = le32(action);
	if (action == MMAL_ACTION_ENABLE || action == MMAL_ACTION_SET_REQUIREMENTS)
		port_to_wire(port, &request.param.port);
	else if (action == MMAL_ACTION_CONNECT)
	{
		request.param.connect.component_handle = le32(peer->component->handle);
		request.param.connect.port_handle = le32(peer->handle);
	}
	s32 status = call_vpu_status(base, &request, sizeof(request), MMAL_MSG_PORT_ACTION);
	if (status != MMAL_SUCCESS)
		Kprintf("[videocore] %s port %ld action %ld (buffers %ld x %ld): %ld\n", port->component->name, (ULONG)port->index_all, action,
		        port->buffer_num, port->buffer_size, status);
	return status;
}

/**
 * port_flush_sync() - Flush a port with a dummy bulk of its own, so the flush cannot overtake a payload.
 * @base: the library base, mmal_lock held
 * @port: the port
 *
 * PORT_FLUSH with an 8-byte bulk out of the scratch, which the VPU
 * serialises with the buffers' bulks (userland's flush_sync).
 *
 * Return: the VPU's status, or LINK_ERR_*.
 */
s32 port_flush_sync(struct VideoCoreBase *base, struct mmal_port *port)
{
	struct mmal_msg_buffer request;
	struct mmal_msg_reply reply;
	u32 reply_size = sizeof(reply);
	memset(&request, 0, sizeof(request));
	request.drvbuf.magic = le32(MMAL_WIRE_MAGIC);
	request.drvbuf.component_handle = le32(port->component->handle);
	request.drvbuf.port_handle = le32(port->handle);
	return call_vpu(base, &request, sizeof(request), MMAL_MSG_PORT_FLUSH, &reply, &reply_size, sizeof(reply), base->mmal.scratch, 8);
}
