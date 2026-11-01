/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * mmal_client.h - the MMAL client (the mmal_*.c files, which share
 * mmal_internal.h): the VPU's components (ril.video_decode, ril.isp,
 * ril.video_encode, ril.image_encode, the image decoder's pipeline) driven
 * through MMAL over vchiq.device's "mmal" service.
 *
 * A component has ports; a port has a format and buffer needs; buffers go to
 * a port and come back through it.  The buffers are the caller's, like
 * IORequests: a struct mmal_buffer over the caller's memory.  The session
 * layer (videocore_session.c and the session_*.c files), the client's caller:
 *
 *   1. mmal_component_create()s a component, which Signal()s the given task
 *      whenever a port of it has something back, and uses its input and
 *      output ports;
 *   2. fills a port's format and mmal_port_commit()s it, sets parameters,
 *      mmal_port_attach()es its buffers to the port, mmal_component_enable()s
 *      the component and mmal_port_enable()s the port;
 *   3. mmal_buffer_send()s the buffers; what the VPU returns waits on the
 *      port's queue, in order, for mmal_port_take(), like messages on a reply
 *      port.  A returned buffer stays the caller's until it is sent again.
 *      mmal_port_flush() has the VPU return what it holds without stopping;
 *   4. mmal_port_disable()s the port, which brings every buffer home, and
 *      mmal_port_detach()es them; mmal_component_destroy() ends it all, or
 *      mmal_component_keep() keeps the component, which mmal_component_take()
 *      hands out again with each port's format as the VPU gave it at the
 *      first create (component->reused set: parameters it holds from its
 *      last use must be set again).  mmal_idle() destroys the kept ones.
 *
 * Events: an ERROR on a control or output port becomes the component's
 * error (mmal_component_error(), the first one kept) and the task is
 * signalled.  A FORMAT_CHANGED on an output port waits on the queue in order
 * with the buffers, cmd set and its data decoded to a struct
 * mmal_format_change, and goes back with mmal_event_done(); one whose data
 * did not arrive becomes the component's error instead.  Every other event
 * is dropped when it arrives.
 *
 * Two components can be joined inside the VPU (mmal_port_connect(): an output
 * port feeds an input port, a tunnel): neither end has buffers attached, theirs
 * never cross to the host.  Enabling either end enables both (enable the
 * input, as libmmal does), disabling either disables both; the far end's
 * enabled flag follows, and an enable or disable of it is then a no-op.
 * mmal_component_destroy() breaks the tunnel.
 *
 * Zero copy (mmal_zero_copy.c): when every buffer attached to a port lies
 * where the VPU can work in it as it is, the attach imports them (SMEM) and
 * the VPU reads and writes the caller's memory itself; otherwise the
 * payloads cross by bulk transfer.  The caller never knows which.
 *
 * The service opens with the first component (or mmal_service_open()) and
 * stays open until mmal_idle(): opening and closing it cost an image call
 * about 5 ms each.  The calls that can fail return 0 or a VCERR_*; the VPU's
 * own status goes to the log.  The caller is trusted: its structures and
 * arguments are checked only where that is cheap.  The calls take mmal_lock
 * themselves, and those that talk to the VPU sleep for its answer with the
 * lock released (CLIENT_WAIT_MS at most), so they come from a task.  The
 * receive task's entries are called with mmal_lock held and never wait.
 */
#ifndef MMAL_CLIENT_H
#define MMAL_CLIENT_H

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/tasks.h>

#include <types.h>
#include <mmal.h>

struct VideoCoreBase;
struct VchiqInfo;
struct dma_pool;

/* The client's own sizes (the protocol's constants are MMAL_*, mmal.h and mmal_proto.h) */
#define CLIENT_WAIT_MS 3000            /* the longest wait for the VPU: a reply, or a disabled port's buffers */
#define CLIENT_WAITERS_MAX 8           /* control messages in flight (the sessions, under session_lock, make one at a time) */
#define CLIENT_EVENT_BUFFERS 4         /* event buffers per component */
#define CLIENT_EVENT_BUFFER_SIZE 512   /* bytes of each: a longer event is dropped */
#define CLIENT_SCRATCH_SIZE 4096       /* memory nobody reads: a payload nobody keeps lands there, cut to this */
#define CLIENT_PORT_BUFFERS_MAX 17     /* buffers attached to one port: a session's set and one of its own */
#define CLIENT_PORT_ENCODINGS_MAX 48   /* encodings a port's list keeps; a longer one is cut */
#define CLIENT_COMPONENT_NAME_MAX 64   /* the name kept (the logs, mmal_component_keep()); a longer one is cut and never kept */

/*
 * Where the VPU reaches host memory, from vchiq.device (reach_set(), at the
 * first service open): the components address CPU memory [start, end) as
 * bus addresses from @bus; the RTG card's VRAM inside it needs no cache
 * maintenance (Emu68 maps it write-through).  Chip RAM is no Pi RAM: an
 * address inside the reach still has to be Pi RAM (dma_addr_reachable()).
 */
struct vc_reach
{
	BOOL known;          /* the fields below came from vchiq.device */
	u32 start, end;      /* the memory the VPU addresses, as the host does */
	u32 bus;             /* @start as the VPU addresses it */
	u32 uncached_start;  /* the VRAM, [start, end) */
	u32 uncached_end;
	BOOL own_in_reach;   /* the DMA pool was cut to Pi RAM in reach: the backend's own buffers can all be imported */
};

/* A call waiting for the reply to its control message */
enum waiter_state
{
	WAITER_FREE = 0, /* no call holds the slot */
	WAITER_WAITING,  /* a call sleeps for its reply */
	WAITER_DONE,     /* the reply came, or the call failed */
};

/*
 * This and struct mmal_client are here because the library base embeds the
 * client's state; outside the mmal_*.c files only the DMA pool (the
 * sessions' objects) is read, through base->mmal.pool.
 */
struct mmal_waiter
{
	enum waiter_state state;
	u16 generation; /* with the index, the cookie the VPU echoes */
	s32 status;     /* MMAL_SUCCESS with the reply in, or why not (LINK_ERR_*) */
	APTR reply;     /* where the reply is copied */
	u32 reply_size; /* in: room; out: bytes copied */
};

/* Where a buffer is.  The order counts: BUF_VPU and the states after it are out, not home. */
enum buffer_state
{
	BUF_IDLE = 0,   /* the caller's (a buffer is allocated cleared) */
	BUF_FREE,       /* an event buffer in its component's pool */
	BUF_QUEUED,     /* returned by the VPU, on the port's queue: mmal_port_take() hands it out */
	BUF_VPU,        /* sent to the VPU */
	BUF_PENDING_RX, /* the VPU filled it; the bulk receive waits for a free request */
	BUF_RX,         /* the bulk receive is in flight */
};

/* What a buffer is */
enum buffer_kind
{
	BUFFER_DATA = 0, /* the caller's, over the caller's memory */
	BUFFER_EVENT,    /* one of a component's event pool */
	BUFFER_SINK,     /* a stand-in whose bulk receive takes a payload nobody keeps (mmal_receive.c) */
};

struct mmal_component;
struct mmal_port;

/*
 * A buffer: the caller's, over the caller's memory, or an event of a
 * component's own pool.  The caller sets data (and room) before
 * mmal_port_attach() and reads the header fields after mmal_port_take();
 * mmal_buffer_send() sets length, flags and pts from its arguments.
 */
struct mmal_buffer
{
	struct MinNode node;    /* first: on the port's queue or the event pool, or among the parked receives */
	struct mmal_port *port; /* attached to; for an event, the port it came through */
	u32 cmd;                /* 0: data; else the event's fourcc (MMAL_EVENT_*) */
	u8 *data;               /* the payload: 4-byte aligned, DMA-reachable */
	u32 room;               /* the bytes from data its owner holds, whole cache lines for the backend's own; 0: the attach's size */
	u32 alloc_size;         /* its bytes, as the attach gave them */
	u32 length;             /* valid bytes from the start */
	u32 flags;              /* MMAL_BUFFER_HEADER_FLAG_* */
	s64 pts;                /* microseconds, or MMAL_TIME_UNKNOWN */
	struct
	{
		u32 planes; /* 0 when the component did not describe the planes */
		u32 offset[4];
		u32 pitch[4];
		u32 flags; /* MMAL_BUFFER_HEADER_VIDEO_FLAG_* */
	} video;                /* the picture's planes, as the component laid them out */
	/* the client's */
	u32 magic;                        /* BUFFER_MAGIC while attached, and in an event buffer or a sink */
	enum buffer_kind kind;
	enum buffer_state state;
	u32 rx_len;                       /* bytes the bulk receive moves: the payload rounded up to 4 (8 for a bare end of stream) */
	u32 rx_at;                        /* when its header arrived with a payload to receive (profile tier) */
	u32 import;                       /* its import into the VPU while attached to a zero-copy port (smem_client.c), else 0 */
	struct mmal_component *component; /* an event's: its pool */
};

/* Whether a port's list of encodings was asked for (mmal_port_supports()) */
enum encodings_state
{
	ENCODINGS_UNASKED = 0,
	ENCODINGS_LISTED, /* the port gave a list: encodings[] */
	ENCODINGS_NONE,   /* it gave none (the image pipeline's output): the format commit decides */
};

/* A port of a component */
struct mmal_port
{
	/* What the port is */
	struct mmal_component *component; /* the component it belongs to */
	u32 type;                         /* MMAL_PORT_TYPE_*: control, input, output or clock */
	u16 index;                        /* its number among the component's ports of that type */
	u16 index_all;                    /* its place in component->ports[] */

	/* The VPU's view of it, read when the component is created and after every commit */
	BOOL enabled;                 /* running: buffers may flow; the enable and disable set it, a tunnel's far end is read back */
	u32 capabilities;             /* MMAL_PORT_CAPABILITY_* */
	u32 buffer_num_min;           /* the fewest buffers the port accepts */
	u32 buffer_size_min;          /* the smallest buffer it accepts, in bytes */
	u32 buffer_alignment_min;     /* the alignment it wants of a buffer's address, 0 for none */
	u32 buffer_num_recommended;   /* the count it suggests, 0 for no suggestion */
	u32 buffer_size_recommended;  /* the size it suggests, 0 for no suggestion */

	/*
	 * The buffer count and size the port runs with, sent to the VPU with the
	 * commit and the enable.  mmal_port_attach() sets them from the buffers
	 * it is given; without buffers (a tunnel's end) the caller sets them
	 * before the commit, and mmal_port_connect() gives both ends of a tunnel
	 * the larger of theirs.  The VPU's values replace them only while no
	 * buffers are attached.
	 */
	u32 buffer_num;
	u32 buffer_size;

	/*
	 * The format: the caller fills it and mmal_port_commit()s it, which
	 * leaves here the format the VPU settled on.  An input's commit
	 * refreshes the outputs' formats too, as the input may change them.
	 */
	struct mmal_format format;

	/* The VPU's view at the component's first create: a kept component's ports start from it again */
	struct mmal_format initial_format;
	u32 initial_buffer_num, initial_buffer_size;

	/* The encodings it takes (MMAL_PARAMETER_SUPPORTED_ENCODINGS), asked once for the component's life */
	u32 encodings[CLIENT_PORT_ENCODINGS_MAX];
	u32 encoding_count;
	enum encodings_state encodings_state;

	/* The client's bookkeeping */
	u32 handle;               /* the VPU's name for the port, in actions, parameters and buffer messages */
	struct MinList queue;     /* returned buffers and format changes waiting for mmal_port_take(), in arrival order */
	struct mmal_port *tunnel; /* the other end of a tunnel (mmal_port_connect()), NULL when none */
	struct mmal_buffer *buffers[CLIENT_PORT_BUFFERS_MAX]; /* the buffers mmal_port_attach() gave it */
	u32 attached_count;       /* how many; 0 when none */
	BOOL zero_copy;           /* the VPU holds ZERO_COPY set: the attached buffers are imports, named by handle (it outlives a keep) */
	BOOL sent_data;           /* a payload went out by bulk: a flush must be the kind that cannot overtake it */
};

/* A component of the VPU */
struct mmal_component
{
	struct MinNode node; /* first: on the client's list */
	u32 handle;          /* the VPU's */
	char name[CLIENT_COMPONENT_NAME_MAX];
	struct Task *task;   /* Signal()led when a port of it has something back, or on an error */
	ULONG sigmask;
	u32 port_count;
	struct mmal_port *ports;  /* control, inputs, outputs, clocks, in that order */
	struct mmal_port *control;
	struct mmal_port *input;  /* the first input in ports[], or NULL */
	struct mmal_port *output; /* the first output, or NULL */
	LONG error;               /* the first ERROR event's, as a VCERR_*; 0 while none came */
	BOOL enabled;             /* the component itself (COMPONENT_ENABLE): a reused one is not enabled again */
	BOOL kept;                /* kept for a mmal_component_take(): no task is signalled for it */
	BOOL reused;              /* taken from the kept ones at least once */
	struct mmal_buffer events[CLIENT_EVENT_BUFFERS];
	u8 *event_payload;        /* the event buffers' DMA memory, CLIENT_EVENT_BUFFER_SIZE each */
	struct MinList free_events; /* the pool: BUF_FREE event buffers */
};

/* The client's state, in the library base under mmal_lock */
struct mmal_client
{
	u32 component_count; /* the service may close at 0 (mmal_idle()) */
	u32 version_major;   /* the VPU's MMAL version, read when the service opens */
	u32 version_minor;
	u32 version_minimum;
	struct mmal_waiter waiters[CLIENT_WAITERS_MAX];
	u16 generation;            /* the last cookie generation handed out */
	struct MinList components; /* the open ones */
	struct MinList pending_rx; /* buffers and sinks waiting for a request for their bulk receive, in arrival order */
	struct dma_pool *pool;     /* DMA-reachable Fast RAM in the VPU's reach: event payloads, the scratch, the sessions' own objects */
	u8 *scratch;               /* CLIENT_SCRATCH_SIZE bytes nobody reads: the sinks' payloads, the synchronising flush's dummy bulk */
	u32 events;         /* EVENT_TO_HOST messages seen */
	u32 events_dropped; /* of which not delivered */
	u32 buffers_back;   /* BUFFER_TO_HOST messages seen */
	u32 stray_messages; /* messages and completions that matched nothing */
};

/* What the client reports of itself (mmal_info(), for VCB_GetInfo()) */
struct mmal_counters
{
	u32 version_major, version_minor; /* the VPU's MMAL version, 0 while the service is closed */
	u32 events, events_dropped, buffers_back, stray_messages;
};

/*
 * The receive task's entries: videocore_link.c calls them as vchiq.device
 * returns its requests, with mmal_lock held.  None of them waits.
 */
/* A message from the VPU: a reply to a call, a returned buffer or an event */
void mmal_message(struct VideoCoreBase *base, const u8 *data, u32 size);
/* A control message did not reach the VPU: its call fails, if it still waits */
void mmal_call_failed(struct VideoCoreBase *base, u32 cookie, s32 status);
/* A buffer's message did not reach the VPU: the buffer comes back to its port, failed */
void mmal_buffer_failed(struct VideoCoreBase *base, struct mmal_buffer *buffer);
/* A payload's bulk receive finished or failed: the buffer goes to its port's queue */
void mmal_rx_done(struct VideoCoreBase *base, struct mmal_buffer *buffer, s32 actual);
/* A request came free: start the parked bulk receives, in order */
void mmal_pump(struct VideoCoreBase *base);
/* The service went: every waiting call fails now rather than at its timeout */
void mmal_lost(struct VideoCoreBase *base);
/* The link learnt where the VPU reaches host memory (link_reach()): take it */
void reach_set(struct VideoCoreBase *base, const struct VchiqInfo *info);

/* The library's (videocore_lib.c, videocore_backend.c) */
/* At library init: the client's state, no component and no service */
void mmal_init(struct VideoCoreBase *base);
/* Whether the client is down for an expunge: no component, the link closed and its task gone (joined here once its requests are home) */
BOOL mmal_expunge(struct VideoCoreBase *base);
/* At expunge, the client down: free the DMA pool and the scratch */
void mmal_shutdown(struct VideoCoreBase *base);
/* Destroy the kept components and close the service kept open past the last one (VCB_Idle()); it may wait for the VPU */
void mmal_kept_release(struct VideoCoreBase *base);
void mmal_idle(struct VideoCoreBase *base);
/* The client's counters and the VPU's version (VCB_GetInfo()) */
void mmal_info(struct VideoCoreBase *base, struct mmal_counters *counters);

/*
 * The session layer's calls.  Each takes mmal_lock itself.  Those that send
 * the VPU a control message sleep for its answer, CLIENT_WAIT_MS at most, so
 * they come from a task.  Those that can fail return 0 or a VCERR_*.
 */

/* The service */
/* Open the service ahead of the first component: what can be imported is known then */
LONG mmal_service_open(struct VideoCoreBase *base);
/* Whether a buffer over some memory (NULL: the backend's own) would be imported rather than sent by bulk */
BOOL mmal_importable(struct VideoCoreBase *base, APTR data, u32 size, BOOL written);

/* Components */
/* Create a VPU component by name and read its ports; @task is signalled when a port has something back */
LONG mmal_component_create(struct VideoCoreBase *base, const char *name, struct Task *task, ULONG sigmask,
                           struct mmal_component **component);
/* Disable its ports, break its tunnels, destroy it and free it; never fails */
void mmal_component_destroy(struct VideoCoreBase *base, struct mmal_component *component);
/* Keep it, its ports stopped and their buffers detached, for a mmal_component_take(); FALSE: destroy it */
BOOL mmal_component_keep(struct VideoCoreBase *base, struct mmal_component *component);
/* A kept component of a name, its ports as at its first create; NULL if none */
struct mmal_component *mmal_component_take(struct VideoCoreBase *base, const char *name, struct Task *task, ULONG sigmask);
/* Enable its control port, then the component itself */
LONG mmal_component_enable(struct VideoCoreBase *base, struct mmal_component *component);
/* Its first ERROR event, as a VCERR_*; 0 while none came */
LONG mmal_component_error(struct VideoCoreBase *base, struct mmal_component *component);

/* Ports */
/* Send port->format and the buffer count and size; the VPU's answer lands in the port */
LONG mmal_port_commit(struct VideoCoreBase *base, struct mmal_port *port);
/* Start the port: it needs buffers attached, or a tunnel (either end starts both) */
LONG mmal_port_enable(struct VideoCoreBase *base, struct mmal_port *port);
/* Stop the port; every buffer comes home to its queue (either end of a tunnel stops both) */
LONG mmal_port_disable(struct VideoCoreBase *base, struct mmal_port *port);
/* Join an output port to an input port inside the VPU: a tunnel */
LONG mmal_port_connect(struct VideoCoreBase *base, struct mmal_port *output, struct mmal_port *input);
/* Have the VPU return every buffer it holds on the port, which keeps running */
LONG mmal_port_flush(struct VideoCoreBase *base, struct mmal_port *port);
/* Whether the port takes an encoding, by the list the firmware gives once; TRUE if it gives none */
BOOL mmal_port_supports(struct VideoCoreBase *base, struct mmal_port *port, u32 encoding);

/* Parameters: their bodies are 32-bit words in native order */
/* Set a parameter */
LONG mmal_port_param_set(struct VideoCoreBase *base, struct mmal_port *port, u32 id, const u32 *words, u32 count);
/* Set a one-word parameter */
LONG mmal_port_param_set_u32(struct VideoCoreBase *base, struct mmal_port *port, u32 id, u32 value);
/* Read a parameter; *count comes back as the whole answer's words, which may be more than fit */
LONG mmal_port_param_get(struct VideoCoreBase *base, struct mmal_port *port, u32 id, u32 *words, u32 *count);

/* Buffers: the caller's, lent to a port */
/* Lend a port the caller's buffers; their count and size become the port's; imported into the VPU if they all can be */
LONG mmal_port_attach(struct VideoCoreBase *base, struct mmal_port *port, struct mmal_buffer *const *buffers, u32 count, u32 size);
/* Take them back: the port must be stopped, with none out; imports are let go and their release waited for */
LONG mmal_port_detach(struct VideoCoreBase *base, struct mmal_port *port);
/* Send one to the VPU: input data to consume, or an output buffer to fill */
LONG mmal_buffer_send(struct VideoCoreBase *base, struct mmal_buffer *buffer, u32 length, u32 flags, s64 pts);
/* Take the next returned buffer or format change off the port's queue; NULL if there is none */
struct mmal_buffer *mmal_port_take(struct VideoCoreBase *base, struct mmal_port *port);
/* Give a format change mmal_port_take() handed out back to its component's pool */
void mmal_event_done(struct VideoCoreBase *base, struct mmal_buffer *event);

#endif /* MMAL_CLIENT_H */
