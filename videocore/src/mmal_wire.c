/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * mmal_wire.c - the MMAL client's wire format: message headers, ports,
 * formats and buffers to and from the VPU's layout, and the VPU's statuses
 * as the client's callers see them.
 *
 * Follows userland/interface/mmal/vc/mmal_vc_client.c and mmal_vc_api.c
 * (Copyright (c) 2012, Broadcom Europe Ltd, BSD-3-Clause).
 */
#include <byteorder.h>
#include <memory.h>

#include <videocore_private.h>
#include <mmal_internal.h>

/* Every reply that carries a status has it right after the header */
_Static_assert(__builtin_offsetof(struct mmal_msg_port_info, status) == __builtin_offsetof(struct mmal_msg_reply, status), "status");
_Static_assert(__builtin_offsetof(struct mmal_msg_component_create_reply, status) == __builtin_offsetof(struct mmal_msg_reply, status),
               "status");
_Static_assert(__builtin_offsetof(struct mmal_msg_port_param_get_reply, status) == __builtin_offsetof(struct mmal_msg_reply, status),
               "status");

/* The format's type-specific words are copied one for one to and from the wire */
_Static_assert(sizeof(((struct mmal_format *)0)->es.words) == sizeof(struct mmal_wire_es_specific), "es words");

/**
 * vcerr_from_status() - An MMAL status or a link failure, as the client's callers see it.
 * @status: MMAL_* (the VPU's) or LINK_ERR_* (before or around the VPU)
 *
 * Return: 0 or a VCERR_*.
 */
LONG vcerr_from_status(s32 status)
{
	switch (status)
	{
	case MMAL_SUCCESS:
		return 0;
	case MMAL_ENOMEM:
	case MMAL_ENOSPC:
		return VCERR_DEVMEM;
	case MMAL_EINVAL:
	case LINK_ERR_PARAM:
		return VCERR_BADPARAM;
	case MMAL_ENOSYS:
	case MMAL_ENOENT:
		return VCERR_UNSUPPORTED;
	case MMAL_ECORRUPT:
		return VCERR_STREAM;
	case MMAL_EAGAIN:
	case MMAL_ENOTREADY:
	case MMAL_EISCONN:
	case MMAL_ENOTCONN:
		return VCERR_BADSTATE;
	default:
		return VCERR_TRANSPORT;
	}
}

/**
 * header_to_wire() - Fill a message's header.
 * @header:  the header
 * @msgid:   MMAL_MSG_*
 * @context: the cookie the VPU echoes in the reply
 */
void header_to_wire(struct mmal_msg_header *header, u32 msgid, u32 context)
{
	header->magic = le32(MMAL_WIRE_MAGIC);
	header->msgid = le32(msgid);
	header->control_service = 0;
	header->context = le32(context);
	header->status = 0;
	header->padding = 0;
}

/**
 * port_to_wire() - A port's state as the VPU lays it out.
 * @port: the port
 * @wire: the wire structure
 */
void port_to_wire(const struct mmal_port *port, struct mmal_wire_port *wire)
{
	memset(wire, 0, sizeof(*wire));
	wire->type = le32(port->type);
	wire->index = le16(port->index);
	wire->index_all = le16(port->index_all);
	wire->is_enabled = le32(port->enabled ? 1u : 0u);
	wire->buffer_num_min = le32(port->buffer_num_min);
	wire->buffer_size_min = le32(port->buffer_size_min);
	wire->buffer_alignment_min = le32(port->buffer_alignment_min);
	wire->buffer_num_recommended = le32(port->buffer_num_recommended);
	wire->buffer_size_recommended = le32(port->buffer_size_recommended);
	wire->buffer_num = le32(port->buffer_num);
	wire->buffer_size = le32(port->buffer_size);
	wire->capabilities = le32(port->capabilities);
}

/**
 * port_from_wire() - Take the VPU's view of a port.
 * @port: the port
 * @wire: the VPU's
 *
 * While buffers are attached they define the buffer count and size: the
 * VPU's are not taken back then.
 */
void port_from_wire(struct mmal_port *port, const struct mmal_wire_port *wire)
{
	port->enabled = le32(wire->is_enabled) != 0;
	port->capabilities = le32(wire->capabilities);
	port->buffer_num_min = le32(wire->buffer_num_min);
	port->buffer_size_min = le32(wire->buffer_size_min);
	port->buffer_alignment_min = le32(wire->buffer_alignment_min);
	port->buffer_num_recommended = le32(wire->buffer_num_recommended);
	port->buffer_size_recommended = le32(wire->buffer_size_recommended);
	if (port->attached_count == 0)
	{
		port->buffer_num = le32(wire->buffer_num);
		port->buffer_size = le32(wire->buffer_size);
	}
}

/**
 * format_to_wire() - A format as the VPU lays it out.
 * @format:    the format
 * @wire:      the VPU's MMAL_ES_FORMAT_T
 * @specific:  its type-specific part
 * @extradata: MMAL_WIRE_EXTRADATA_MAX bytes for the codec configuration
 */
void format_to_wire(const struct mmal_format *format, struct mmal_wire_es_format *wire, struct mmal_wire_es_specific *specific,
                    u8 *extradata)
{
	u32 extradata_size = umin(format->extradata_size, MMAL_WIRE_EXTRADATA_MAX);
	wire->type = le32(format->type);
	wire->encoding = le32(format->encoding);
	wire->encoding_variant = le32(format->encoding_variant);
	wire->es = 0;
	wire->bitrate = le32(format->bitrate);
	wire->flags = le32(format->flags);
	wire->extradata_size = le32(extradata_size);
	wire->extradata = 0;
	for (u32 i = 0; i < MMAL_WIRE_ES_WORDS; i++)
		specific->words[i] = le32(format->es.words[i]);
	memset(extradata, 0, MMAL_WIRE_EXTRADATA_MAX);
	memcpy(extradata, format->extradata, extradata_size);
}

/**
 * format_from_wire() - Take a format the VPU sent.
 * @format:         the format
 * @wire:           the VPU's MMAL_ES_FORMAT_T
 * @specific:       its type-specific part
 * @extradata:      the codec configuration bytes that follow
 * @extradata_room: how many of them the message holds
 */
void format_from_wire(struct mmal_format *format, const struct mmal_wire_es_format *wire,
                      const struct mmal_wire_es_specific *specific, const u8 *extradata, u32 extradata_room)
{
	format->type = le32(wire->type);
	format->encoding = le32(wire->encoding);
	format->encoding_variant = le32(wire->encoding_variant);
	format->bitrate = le32(wire->bitrate);
	format->flags = le32(wire->flags);
	for (u32 i = 0; i < MMAL_WIRE_ES_WORDS; i++)
		format->es.words[i] = le32(specific->words[i]);
	u32 extradata_size = umin(le32(wire->extradata_size), umin(extradata_room, MMAL_EXTRADATA_MAX));
	format->extradata_size = extradata_size;
	memcpy(format->extradata, extradata, extradata_size);
}

/**
 * buffer_to_wire() - A buffer as BUFFER_FROM_HOST carries it.
 * @buffer:  the buffer, attached
 * @message: the message, cleared and filled but for its header (header_to_wire())
 *
 * The port's handles and the buffer's address as the cookie, the header
 * (the payload from the buffer's start: the offset is 0; no decode time),
 * the planes.
 */
void buffer_to_wire(const struct mmal_buffer *buffer, struct mmal_msg_buffer *message)
{
	const struct mmal_port *port = buffer->port;
	memset(message, 0, sizeof(*message));
	message->drvbuf.magic = le32(MMAL_WIRE_MAGIC);
	message->drvbuf.component_handle = le32(port->component->handle);
	message->drvbuf.port_handle = le32(port->handle);
	message->drvbuf.client_context = le32((u32)buffer);
	message->buffer_header.cmd = le32(buffer->cmd);
	message->buffer_header.data = le32((u32)buffer->data);
	message->buffer_header.alloc_size = le32(buffer->alloc_size);
	message->buffer_header.length = le32(buffer->length);
	message->buffer_header.flags = le32(buffer->flags);
	time_to_wire(buffer->pts, &message->buffer_header.pts_lo, &message->buffer_header.pts_hi);
	time_to_wire((s64)MMAL_TIME_UNKNOWN, &message->buffer_header.dts_lo, &message->buffer_header.dts_hi);
	message->video.planes = le32(buffer->video.planes);
	for (u32 i = 0; i < 4; i++)
	{
		message->video.offset[i] = le32(buffer->video.offset[i]);
		message->video.pitch[i] = le32(buffer->video.pitch[i]);
	}
	message->video.flags = le32(buffer->video.flags);
}

/**
 * buffer_from_wire() - Take a returned buffer's header: command, length, flags, timestamp, planes.
 * @buffer:  the buffer
 * @message: the BUFFER_TO_HOST that returns it
 *
 * The payload lands at the start of the buffer: the VPU's offset is not
 * taken (a zero-copy buffer's is checked by zero_copy_landed()).
 */
void buffer_from_wire(struct mmal_buffer *buffer, const struct mmal_msg_buffer *message)
{
	buffer->cmd = le32(message->buffer_header.cmd);
	buffer->length = le32(message->buffer_header.length);
	buffer->flags = le32(message->buffer_header.flags);
	buffer->pts = time_from_wire(message->buffer_header.pts_lo, message->buffer_header.pts_hi);
	buffer->video.planes = le32(message->video.planes);
	for (u32 i = 0; i < 4; i++)
	{
		buffer->video.offset[i] = le32(message->video.offset[i]);
		buffer->video.pitch[i] = le32(message->video.pitch[i]);
	}
	buffer->video.flags = le32(message->video.flags);
}

/**
 * time_from_wire() - A 64-bit timestamp from its two words, low first.
 * @low:  the low word
 * @high: the high word
 *
 * Return: the timestamp.
 */
s64 time_from_wire(__le32 low, __le32 high)
{
	return (s64)(((u64)le32(high) << 32) | (u64)le32(low));
}

/**
 * time_to_wire() - A 64-bit timestamp as its two words.
 * @time: the timestamp
 * @low:  where the low word goes
 * @high: where the high word goes
 */
void time_to_wire(s64 time, __le32 *low, __le32 *high)
{
	*low = le32(u64_lo32((u64)time));
	*high = le32(u64_hi32((u64)time));
}

/**
 * status_from_wire() - The VPU's status in a reply.
 * @reply:      the reply, a status word right after its header
 * @reply_size: its bytes, as they came
 * @needed:     the bytes it must have
 *
 * Return: the status, or LINK_ERR_REMOTE for a reply too short.
 */
s32 status_from_wire(const void *reply, u32 reply_size, u32 needed)
{
	if (reply_size < needed)
		return LINK_ERR_REMOTE;
	return (s32)le32(((const struct mmal_msg_reply *)reply)->status);
}
