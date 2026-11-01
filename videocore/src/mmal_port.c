/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * mmal_port.c - ports: format commits, enable and disable, tunnels,
 * flushes, parameters, and the encodings a port takes.
 *
 * Follows userland/interface/mmal/vc/mmal_vc_client.c and mmal_vc_api.c
 * (Copyright (c) 2012, Broadcom Europe Ltd, BSD-3-Clause).
 */
#include <byteorder.h>
#include <memory.h>

#include <videocore_private.h>
#include <mmal_internal.h>

/* ---- ports --------------------------------------------------------------- */

/**
 * tunnel_break() - Undo a tunnel from either end; the VPU is told through its output end.
 * @base: the library base, mmal_lock held
 * @port: an end of the tunnel
 *
 * Return: the VPU's status, or LINK_ERR_*.
 */
s32 tunnel_break(struct VideoCoreBase *base, struct mmal_port *port)
{
	struct mmal_port *output = port->type == MMAL_PORT_TYPE_OUTPUT ? port : port->tunnel;
	s32 status = port_action(base, output, MMAL_ACTION_DISCONNECT, NULL);
	port->tunnel->tunnel = NULL;
	port->tunnel = NULL;
	return status;
}

/**
 * port_enable() - Enable a port; a tunnel's far end gets its shape first.
 * @base: the library base, mmal_lock held
 * @port: the port: with buffers attached, in a tunnel, or a control port
 *
 * The VPU enables both ends of a tunnel on the first enable and refuses the
 * second (EINVAL): the far end gets its shape first, since no enable of its
 * own will carry it, and its state is read back afterwards so that an enable
 * of it is a no-op.  This is libmmal's sequence.
 *
 * Return: the VPU's status, MMAL_EINVAL without buffers or a tunnel, or LINK_ERR_*.
 */
s32 port_enable(struct VideoCoreBase *base, struct mmal_port *port)
{
	if (port->enabled)
		return MMAL_SUCCESS;
	if (port->attached_count == 0 && port->tunnel == NULL && port->type != MMAL_PORT_TYPE_CONTROL)
		return MMAL_EINVAL; /* mmal_port_attach() first: the enable carries the buffers' shape */

	s32 status = port->tunnel != NULL ? port_action(base, port->tunnel, MMAL_ACTION_SET_REQUIREMENTS, NULL) : MMAL_SUCCESS;
	if (status == MMAL_SUCCESS)
		status = port_action(base, port, MMAL_ACTION_ENABLE, NULL);
	if (status != MMAL_SUCCESS)
		return status;

	port->enabled = TRUE;
	if (port->tunnel != NULL)
		(void)port_info_get(base, port->tunnel);
	return MMAL_SUCCESS;
}

/**
 * port_disable() - Disable a port and wait for its buffers to come home.
 * @base: the library base, mmal_lock held (released while waiting)
 * @port: an enabled port
 *
 * The VPU returns every buffer before it answers; the bulks of the last ones
 * may still be landing.  Disabling one end of a tunnel disables both inside
 * the VPU: the far end's state is read back, as libmmal does, so its own
 * disable is then a no-op.
 *
 * Return: the VPU's status, or LINK_ERR_* (LINK_ERR_TIMEOUT if the buffers
 * did not come home).
 */
s32 port_disable(struct VideoCoreBase *base, struct mmal_port *port)
{
	s32 status = port_action(base, port, MMAL_ACTION_DISABLE, NULL);
	if (status != MMAL_SUCCESS)
		return status;

	port->enabled = FALSE;
	if (port->tunnel != NULL)
		(void)port_info_get(base, port->tunnel);
	if (port->attached_count != 0)
		status = link_wait(base, port_buffers_home, port, CLIENT_WAIT_MS);
	return status;
}

/**
 * mmal_port_commit() - Send a port's format and buffer shape; the VPU's answer lands in the port.
 * @base: the library base
 * @port: the port, disabled
 *
 * An input format may change what the outputs offer: after an input port's
 * commit, every output port is read back too.
 *
 * Return: 0 or the error.
 */
LONG mmal_port_commit(struct VideoCoreBase *base, struct mmal_port *port)
{
	struct ExecBase *SysBase = base->SysBase;

	ObtainSemaphore(&base->mmal_lock);
	s32 status = port_info_set(base, port);
	if (status == MMAL_SUCCESS)
		status = port_info_get(base, port);

	struct mmal_component *component = port->component;
	for (u32 i = 0; status == MMAL_SUCCESS && port->type == MMAL_PORT_TYPE_INPUT && i < component->port_count; i++)
		if (component->ports[i].type == MMAL_PORT_TYPE_OUTPUT)
			status = port_info_get(base, &component->ports[i]);
	if (status != MMAL_SUCCESS)
		Kprintf("[videocore] commit %s port %ld format: %ld\n", component->name, (ULONG)port->index_all, status);
	ReleaseSemaphore(&base->mmal_lock);
	return vcerr_from_status(status);
}

/**
 * mmal_port_enable() - Enable a port.
 * @base: the library base
 * @port: the port, with buffers attached or in a tunnel
 *
 * Enabling either end of a tunnel enables both.
 *
 * Return: 0, VCERR_BADPARAM without buffers or a tunnel, or the error.
 */
LONG mmal_port_enable(struct VideoCoreBase *base, struct mmal_port *port)
{
	struct ExecBase *SysBase = base->SysBase;

	ObtainSemaphore(&base->mmal_lock);
	s32 status = port_enable(base, port);
	ReleaseSemaphore(&base->mmal_lock);
	return vcerr_from_status(status);
}

/**
 * mmal_port_disable() - Disable a port; its buffers come home, to its queue.
 * @base: the library base
 * @port: the port
 *
 * Disabling either end of a tunnel disables both.  A disabled port succeeds
 * at once.
 *
 * Return: 0 or the error.
 */
LONG mmal_port_disable(struct VideoCoreBase *base, struct mmal_port *port)
{
	struct ExecBase *SysBase = base->SysBase;

	ObtainSemaphore(&base->mmal_lock);
	s32 status = port->enabled ? port_disable(base, port) : MMAL_SUCCESS;
	ReleaseSemaphore(&base->mmal_lock);
	return vcerr_from_status(status);
}

/**
 * mmal_port_connect() - Tunnel an output port into an input port inside the VPU.
 * @base:   the library base
 * @output: the output port, disabled, no tunnel
 * @input:  the input port, disabled, no tunnel, its format committed to match
 *
 * The buffers go between the two inside the VPU; the host sees neither.  The
 * VPU's pool takes the larger of the two shapes (libmmal's rule); it is
 * created when an end is enabled (the input, as libmmal does), which enables
 * both.  mmal_component_destroy() breaks the tunnel.
 *
 * Return: 0, VCERR_BADPARAM for ports of the wrong type or enabled,
 * VCERR_BADSTATE for an end already in a tunnel, or the error.
 */
LONG mmal_port_connect(struct VideoCoreBase *base, struct mmal_port *output, struct mmal_port *input)
{
	struct ExecBase *SysBase = base->SysBase;

	if (output->type != MMAL_PORT_TYPE_OUTPUT || input->type != MMAL_PORT_TYPE_INPUT || output->enabled || input->enabled)
		return VCERR_BADPARAM;
	if (output->tunnel != NULL || input->tunnel != NULL)
		return VCERR_BADSTATE;

	ObtainSemaphore(&base->mmal_lock);
	s32 status = port_action(base, output, MMAL_ACTION_CONNECT, input);
	if (status == MMAL_SUCCESS)
	{
		output->tunnel = input;
		input->tunnel = output;
		output->buffer_num = input->buffer_num = umax(output->buffer_num, input->buffer_num);
		output->buffer_size = input->buffer_size = umax(output->buffer_size, input->buffer_size);
	}
	ReleaseSemaphore(&base->mmal_lock);
	return vcerr_from_status(status);
}

/**
 * mmal_port_flush() - Have the VPU return every buffer it holds on a port.
 * @base: the library base
 * @port: the port
 *
 * A flush after data went out by bulk must not overtake that data: on a
 * VPU that can, it then goes with a dummy bulk of its own
 * (port_flush_sync()).
 *
 * Return: 0 or the error.
 */
LONG mmal_port_flush(struct VideoCoreBase *base, struct mmal_port *port)
{
	struct ExecBase *SysBase = base->SysBase;

	ObtainSemaphore(&base->mmal_lock);
	s32 status = port->sent_data && vpu_has_flush_sync(base) ? port_flush_sync(base, port) : port_action(base, port, MMAL_ACTION_FLUSH, NULL);
	ReleaseSemaphore(&base->mmal_lock);
	return vcerr_from_status(status);
}

/**
 * mmal_port_supports() - Whether a port takes an encoding, by the firmware's list.
 * @base:     the library base
 * @port:     the port
 * @encoding: MMAL_ENCODING_*
 *
 * The list is asked for once for the component's life (a kept one's too),
 * and so is a refusal to give one (the image pipeline's output has none).
 * The port's list is the caller's to read, like its format: the callers
 * serialise themselves.
 *
 * Return: TRUE if @encoding is listed, or if the firmware gives no list
 * (the format commit decides then).
 */
BOOL mmal_port_supports(struct VideoCoreBase *base, struct mmal_port *port, u32 encoding)
{
	if (port->encodings_state == ENCODINGS_UNASKED)
	{
		u32 count = ARRAY_SIZE(port->encodings);
		memset(port->encodings, 0, sizeof(port->encodings)); /* the request's body; words a short reply leaves out read as no encoding */
		BOOL listed = mmal_port_param_get(base, port, MMAL_PARAMETER_SUPPORTED_ENCODINGS, port->encodings, &count) == 0;
		port->encoding_count = listed ? umin(count, ARRAY_SIZE(port->encodings)) : 0; /* a longer list is cut to the room */
		port->encodings_state = listed ? ENCODINGS_LISTED : ENCODINGS_NONE;
	}
	if (port->encodings_state == ENCODINGS_NONE)
		return TRUE;
	for (u32 i = 0; i < port->encoding_count; i++)
		if (port->encodings[i] == encoding)
			return TRUE;
	return FALSE;
}

/* ---- parameters ---------------------------------------------------------- */

/**
 * param_bytes() - A parameter's size on the wire for a body of so many words.
 * @words: the body's words
 *
 * Return: the bytes, the id and size words included, as the size word counts them.
 */
static u32 param_bytes(u32 words)
{
	return 8u + words * 4u;
}

/**
 * param_words() - The body's words of a parameter of a given size.
 * @bytes: the parameter's bytes, as its size word counts them
 *
 * Return: the words after the id and size, 0 if there are none.
 */
static u32 param_words(u32 bytes)
{
	return bytes >= 8u ? (bytes - 8u) / 4u : 0;
}

/**
 * param_request() - A parameter message: the port, the id, the size, the body's words.
 * @request: the message
 * @port:    the port
 * @id:      MMAL_PARAMETER_*
 * @words:   the body, native order
 * @count:   its words, at most MMAL_WIRE_PARAM_WORDS
 *
 * Return: the bytes to send.
 */
static u32 param_request(struct mmal_msg_port_param *request, const struct mmal_port *port, u32 id, const u32 *words, u32 count)
{
	u32 size = param_bytes(count);
	memset(request, 0, MMAL_MSG_PORT_PARAM_FIXED);
	request->component_handle = le32(port->component->handle);
	request->port_handle = le32(port->handle);
	request->id = le32(id);
	request->size = le32(size);
	for (u32 i = 0; i < count; i++)
		request->space[i] = le32(words[i]);
	return MMAL_MSG_PORT_PARAM_FIXED + size;
}

/**
 * port_param_set() - Set a parameter of a port, mmal_lock held.
 * @base:  the library base, mmal_lock held once (released while waiting)
 * @port:  the port
 * @id:    MMAL_PARAMETER_*
 * @words: the parameter's body, 32-bit words in native order
 * @count: its words, at most MMAL_WIRE_PARAM_WORDS
 *
 * Return: the VPU's status (in the log if not MMAL_SUCCESS), or LINK_ERR_*.
 */
s32 port_param_set(struct VideoCoreBase *base, struct mmal_port *port, u32 id, const u32 *words, u32 count)
{
	struct mmal_msg_port_param request;
	u32 size = param_request(&request, port, id, words, count);
	s32 status = call_vpu_status(base, &request, size, MMAL_MSG_PORT_PARAMETER_SET);
	if (status != MMAL_SUCCESS)
		Kprintf("[videocore] set parameter %08lx on %s port %ld: %ld\n", id, port->component->name, (ULONG)port->index_all, status);
	return status;
}

/**
 * mmal_port_param_set() - Set a parameter of a port (the control port for a component's own).
 * @base:  the library base
 * @port:  the port
 * @id:    MMAL_PARAMETER_*
 * @words: the parameter's body, 32-bit words in native order
 * @count: its words
 *
 * Return: 0 or the error (the VPU's status in the log).
 */
LONG mmal_port_param_set(struct VideoCoreBase *base, struct mmal_port *port, u32 id, const u32 *words, u32 count)
{
	struct ExecBase *SysBase = base->SysBase;

	if (count > MMAL_WIRE_PARAM_WORDS)
		return VCERR_BADPARAM;

	ObtainSemaphore(&base->mmal_lock);
	s32 status = port_param_set(base, port, id, words, count);
	ReleaseSemaphore(&base->mmal_lock);

	return vcerr_from_status(status);
}

/**
 * mmal_port_param_set_u32() - Set a one-word parameter of a port.
 * @base:  the library base
 * @port:  the port
 * @id:    MMAL_PARAMETER_*
 * @value: the value (a boolean is 0 or 1)
 *
 * Return: 0 or the error.
 */
LONG mmal_port_param_set_u32(struct VideoCoreBase *base, struct mmal_port *port, u32 id, u32 value)
{
	return mmal_port_param_set(base, port, id, &value, 1);
}

/**
 * mmal_port_param_get() - Read a parameter of a port.
 * @base:  the library base
 * @port:  the port
 * @id:    MMAL_PARAMETER_*
 * @words: in: the request's body; out: the answer, native order
 * @count: in: the words @words holds; out: the words of the whole answer,
 *         which may be more than were copied
 *
 * An answer too long for @words is cut short, not refused.
 *
 * Return: 0 or the error (the VPU's status in the log).
 */
LONG mmal_port_param_get(struct VideoCoreBase *base, struct mmal_port *port, u32 id, u32 *words, u32 *count)
{
	struct ExecBase *SysBase = base->SysBase;

	u32 room = *count;
	if (room > MMAL_WIRE_PARAM_WORDS)
		return VCERR_BADPARAM;

	ObtainSemaphore(&base->mmal_lock);
	struct mmal_msg_port_param request;
	struct mmal_msg_port_param_get_reply reply;
	u32 size = param_request(&request, port, id, words, room);
	u32 reply_size = MMAL_MSG_PORT_PARAM_REPLY_FIXED + param_bytes(room);

	s32 status = call_vpu(base, &request, size, MMAL_MSG_PORT_PARAMETER_GET, &reply, &reply_size, MMAL_MSG_PORT_PARAM_REPLY_FIXED, NULL, 0);
	if (status == MMAL_SUCCESS || status == MMAL_ENOSPC)
	{
		/*
		 * The words copied are the fewest of three: the answer's, as its size
		 * word gives them; those that arrived; the caller's room.  ENOSPC (the
		 * answer needs more room than the request gave) still states its size,
		 * so the caller learns how much would do.
		 */
		u32 arrived = reply_size - MMAL_MSG_PORT_PARAM_REPLY_FIXED; /* call_vpu() checked the fixed part */
		u32 answer = arrived >= param_bytes(0) ? param_words(le32(reply.size)) : 0; /* 0 if the size word did not come */
		u32 copy = umin(umin(answer, param_words(arrived)), room);
		for (u32 i = 0; i < copy; i++)
			words[i] = le32(reply.space[i]);
		*count = answer;
		status = MMAL_SUCCESS;
	}
	else
		Kprintf("[videocore] get parameter %08lx on %s port %ld: %ld\n", id, port->component->name, (ULONG)port->index_all, status);
	ReleaseSemaphore(&base->mmal_lock);

	return vcerr_from_status(status);
}
