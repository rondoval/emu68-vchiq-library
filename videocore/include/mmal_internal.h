/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * mmal_internal.h - what the MMAL client's sources share: the functions one
 * file calls in another.
 *
 *   mmal_client.c     the service, the components (created, kept, destroyed), init, idle and expunge
 *   mmal_call.c       control messages: the call, its waiter and reply; the messages (component, port info, actions, flush)
 *   mmal_receive.c    the receive side: messages from the VPU, bulk receives, delivery
 *   mmal_port.c       ports: commit, enable, disable, tunnels, flush; parameters; the encodings a port takes
 *   mmal_buffer.c     buffers: attach, detach, send, take; events back to their pool
 *   mmal_zero_copy.c  zero copy: where the VPU reaches, which buffers it may work in, the imports, the cache work
 *   mmal_wire.c       the wire format: headers, ports, formats, buffers; the VPU's statuses
 *
 * Follows userland/interface/mmal/vc/mmal_vc_client.c and mmal_vc_api.c
 * (Copyright (c) 2012, Broadcom Europe Ltd, BSD-3-Clause), reduced to one
 * client with plain structures: no pools, callbacks or reference counts.
 *
 *   Control messages (component create, port info, actions, parameters) are
 *   sent by the calling task and answered through a waiter the reply names
 *   by cookie; the task sleeps in link_wait() meanwhile (mmal_call.c).
 *
 *   Buffers go to the VPU as BUFFER_FROM_HOST with the payload's bulk
 *   transmit right behind it (link_send_buffer()), and come back as
 *   BUFFER_TO_HOST to the receive task.  A payload comes inline when short,
 *   else the receive task queues its bulk receive, in order; once the data
 *   is in, the buffer goes on its port's queue and the component's task is
 *   signalled (mmal_buffer.c out, mmal_receive.c back).
 *
 *   Events (EVENT_TO_HOST) take a buffer of the component's event pool and
 *   travel the same way, decoded to native byte order on delivery: an ERROR
 *   becomes the component's error, a FORMAT_CHANGED on an output port is
 *   queued like a buffer, anything else goes back to the pool at once.
 *
 *   A port whose buffers can all be imported into the VPU runs zero copy
 *   (mmal_zero_copy.c): the attach imports them and sets the port's
 *   ZERO_COPY, each buffer travels by its import's handle with no bulk, and
 *   the client does the cache maintenance the device would do for a bulk
 *   (zero_copy_arm() before, zero_copy_landed() after).
 *
 * The buffers are the caller's: attached to a port, they are named to the
 * VPU by their address, and the port knows them by that.  Everything in
 * these files runs under mmal_lock: the public calls take it, everything
 * below runs with it held, and the receive task's entries never wait.  The
 * caller is trusted with the structures.  What the VPU sends is not: its
 * handles and cookies are only compared, never followed.
 */
#ifndef MMAL_INTERNAL_H
#define MMAL_INTERNAL_H

#include <exec/types.h>

#include <types.h>
#include <mmal_client.h>
#include <mmal_proto.h>

struct VideoCoreBase;

/* 'MMBU': in a buffer while attached, and in an event buffer or a sink for good; a link completion naming a buffer without it is a stray */
#define BUFFER_MAGIC 0x4d4d4255u

/*
 * The functions one file calls in another, all with mmal_lock held.  Those
 * returning s32 give the VPU's status (MMAL_*) or a LINK_ERR_*; the public
 * calls turn it into a VCERR_* with vcerr_from_status().
 */

/* mmal_wire.c: the wire format and the VPU's statuses */
/* An MMAL status or a link failure as a VCERR_*, 0 for success */
LONG vcerr_from_status(s32 status);
/* Fill a message's header: the magic, the id, the cookie the reply echoes */
void header_to_wire(struct mmal_msg_header *header, u32 msgid, u32 context);
/* A port's state as the VPU lays it out */
void port_to_wire(const struct mmal_port *port, struct mmal_wire_port *wire);
/* Take the VPU's view of a port; its buffer count and size only while no buffers are attached */
void port_from_wire(struct mmal_port *port, const struct mmal_wire_port *wire);
/* A format as the VPU lays it out: the format, its type-specific words, its extradata */
void format_to_wire(const struct mmal_format *format, struct mmal_wire_es_format *wire, struct mmal_wire_es_specific *specific,
                    u8 *extradata);
/* Take a format the VPU sent, its extradata cut to what the message holds */
void format_from_wire(struct mmal_format *format, const struct mmal_wire_es_format *wire, const struct mmal_wire_es_specific *specific,
                      const u8 *extradata, u32 extradata_room);
/* A buffer as BUFFER_FROM_HOST carries it: the port's handles, the buffer's header and planes (not the message header) */
void buffer_to_wire(const struct mmal_buffer *buffer, struct mmal_msg_buffer *message);
/* Take a returned buffer's header from its BUFFER_TO_HOST: command, length, flags, timestamp, planes */
void buffer_from_wire(struct mmal_buffer *buffer, const struct mmal_msg_buffer *message);
/* A 64-bit timestamp from its two words, low first */
s64 time_from_wire(__le32 low, __le32 high);
/* A 64-bit timestamp as its two words */
void time_to_wire(s64 time, __le32 *low, __le32 *high);
/* The VPU's status in a reply; LINK_ERR_REMOTE for a reply too short to hold it */
s32 status_from_wire(const void *reply, u32 reply_size, u32 needed);

/* mmal_client.c: the VPU's MMAL version */
/* Whether a bare end-of-stream marker travels as BUFFER_FROM_HOST_ZEROLEN (MMAL 12.2 and later) */
BOOL vpu_has_zerolen(const struct VideoCoreBase *base);
/* Whether a flush can be serialised with the bulks before it (userland's flush_sync, firmware 15 and later) */
BOOL vpu_has_flush_sync(const struct VideoCoreBase *base);

/* mmal_call.c: control messages */
/* Send a control message and sleep until its reply comes (its status checked when @needed is set), the send fails or CLIENT_WAIT_MS passes */
s32 call_vpu(struct VideoCoreBase *base, void *message, u32 size, u32 msgid, void *reply, u32 *reply_size, u32 needed, APTR bulk,
             u32 bulk_size);
/* Send a control message whose reply is only a status, and give that status */
s32 call_vpu_status(struct VideoCoreBase *base, void *message, u32 size, u32 msgid);
/* A reply from the VPU, copied to the call that waits for it; FALSE if none does */
BOOL call_reply(struct VideoCoreBase *base, const u8 *data, u32 size, u32 context);
/* Send a message that names only a component: destroy, enable or disable */
s32 component_call(struct VideoCoreBase *base, struct mmal_component *component, u32 msgid);
/* Read a port's state and format from the VPU into the port */
s32 port_info_get(struct VideoCoreBase *base, struct mmal_port *port);
/* Send a port's buffer count and size and its format; the VPU's answer lands in the port */
s32 port_info_set(struct VideoCoreBase *base, struct mmal_port *port);
/* Send a port action (MMAL_ACTION_*); @peer is the input port of a connect */
s32 port_action(struct VideoCoreBase *base, struct mmal_port *port, u32 action, const struct mmal_port *peer);
/* Flush a port with a dummy bulk of its own, so the flush cannot overtake a payload sent by bulk */
s32 port_flush_sync(struct VideoCoreBase *base, struct mmal_port *port);

/* mmal_port.c: starting and stopping ports */
/* Undo a tunnel from either end; the VPU is told through the output end */
s32 tunnel_break(struct VideoCoreBase *base, struct mmal_port *port);
/* Enable a port; a tunnel's far end is given its buffer shape first */
s32 port_enable(struct VideoCoreBase *base, struct mmal_port *port);
/* Disable a port and sleep until its buffers come home */
s32 port_disable(struct VideoCoreBase *base, struct mmal_port *port);
/* Set a port's parameter, the lock held once: mmal_port_param_set()'s body */
s32 port_param_set(struct VideoCoreBase *base, struct mmal_port *port, u32 id, const u32 *words, u32 count);

/* mmal_buffer.c: attached buffers */
/* Whether every buffer attached to a port is home: a link_wait() condition */
BOOL port_buffers_home(APTR base, APTR port);
/* Let go of a port's buffers: off its queue, their imports let go (not waited for), their port and magic cleared */
void port_buffers_forget(struct VideoCoreBase *base, struct mmal_port *port);
/* Let go of a port's buffers and wait for the VPU to report their imports released: the memory is the caller's again */
void port_buffers_release(struct VideoCoreBase *base, struct mmal_port *port);
/* Put an event buffer in its component's pool */
void event_recycle(struct VideoCoreBase *base, struct mmal_buffer *event);

/* mmal_zero_copy.c: the reach, the imports, the cache work */
/* Whether a range needs no cache maintenance (the VRAM) */
BOOL reach_uncached(const struct VideoCoreBase *base, APTR data, u32 size);
/* An address as the VPU addresses it */
u32 reach_bus(const struct VideoCoreBase *base, APTR data);
/* Run a port zero copy if its buffers can all be imported, else by bulk; a status when ZERO_COPY could not be cleared */
s32 zero_copy_choose(struct VideoCoreBase *base, struct mmal_port *port);
/* Let go of the imports of a port's attached buffers (nothing waits) */
void imports_release(struct VideoCoreBase *base, struct mmal_port *port);
/* The cache work before the VPU works in an imported buffer, about to be sent */
void zero_copy_arm(struct VideoCoreBase *base, const struct mmal_buffer *buffer);
/* An imported buffer is back: the cache work after; FALSE if the VPU wrote past the buffer */
BOOL zero_copy_landed(struct VideoCoreBase *base, struct mmal_buffer *buffer, u32 offset);

/* mmal_receive.c: the receive side's bookkeeping */
/* A buffer let go while out: a receive still parked goes to a sink in its place, a request at the device names nothing */
void rx_forget(struct VideoCoreBase *base, struct mmal_buffer *buffer);
/* The service closed: the parked receives never run, their sinks are freed */
void rx_parked_drop(struct VideoCoreBase *base);

#endif /* MMAL_INTERNAL_H */
