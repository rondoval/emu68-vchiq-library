/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * mmal_client.c - the MMAL client's core: the service, opened with the
 * first component or image call and kept open until mmal_idle(), the
 * components (created, kept between uses, destroyed), and the client's
 * state at library init, idle and expunge.
 *
 * Follows userland/interface/mmal/vc/mmal_vc_client.c and mmal_vc_api.c
 * (Copyright (c) 2012, Broadcom Europe Ltd, BSD-3-Clause).
 */
#include <byteorder.h>
#include <memory.h>
#include <minlist.h>
#include <strutil.h>

#include <videocore_private.h>
#include <mmal_internal.h>

/* ---- the service --------------------------------------------------------- */

/**
 * dma_pool_trim() - Keep the DMA pool's regions to the Pi RAM the VPU reaches.
 * @base: the library base, the reach known (link_reach())
 *
 * The backend's own buffers can then all be imported (mmal_zero_copy.c).
 * On a Pi 4 the VPU reaches the lowest GiB, Pi RAM's first block, which
 * Exec lists first and fills first anyway.  If no region lies within, all
 * stay, and the backend's own buffers go by bulk.
 */
static void dma_pool_trim(struct VideoCoreBase *base)
{
	struct dma_mem_ctx *dma = &base->dma;
	struct vc_reach *reach = &base->reach;
	u32 kept = 0;
	for (u32 i = 0; i < dma->count; i++)
		if (dma->regions[i].start >= reach->start && dma->regions[i].end <= reach->end)
			dma->regions[kept++] = dma->regions[i];
	if (kept == 0)
	{
		Kprintf("[videocore] no Pi RAM within the VPU's reach: the backend's own buffers go by bulk\n");
		return;
	}
	dma->count = kept;
	reach->own_in_reach = TRUE;
}

/**
 * dma_pool_make() - The DMA pool and the scratch, made at the first open and kept until the expunge.
 * @base: the library base, mmal_lock held
 *
 * The pool takes only the Pi RAM the VPU reaches, when vchiq.device says
 * where that is.
 *
 * Return: TRUE once both exist.
 */
static BOOL dma_pool_make(struct VideoCoreBase *base)
{
	struct mmal_client *client = &base->mmal;
	if (client->pool == NULL)
	{
		if (base->reach.known)
			dma_pool_trim(base);
		client->pool = dma_pool_create(&base->dma);
	}
	if (client->pool != NULL && client->scratch == NULL)
		client->scratch = dma_zalloc(client->pool, DMA_ALIGN_MIN, CLIENT_SCRATCH_SIZE);
	return client->scratch != NULL;
}

/**
 * version_read() - Read the VPU's MMAL version.
 * @base: the library base, mmal_lock held, the link open
 *
 * The version decides how flushes and end-of-stream markers travel
 * (vpu_has_flush_sync(), vpu_has_zerolen()).  Without an answer the
 * version read before stays: 0.0 at first, the oldest ways.
 */
static void version_read(struct VideoCoreBase *base)
{
	struct mmal_client *client = &base->mmal;
	struct mmal_msg_version request, reply;
	u32 reply_size = sizeof(reply);
	memset(&request, 0, sizeof(request));
	s32 status = call_vpu(base, &request, sizeof(request), MMAL_MSG_GET_VERSION, &reply, &reply_size, 0, NULL, 0);
	if (status == MMAL_SUCCESS && reply_size >= sizeof(reply))
	{
		client->version_major = le32(reply.major);
		client->version_minor = le32(reply.minor);
		client->version_minimum = le32(reply.minimum);
	}
}

/**
 * vpu_has_zerolen() - Whether a bare end-of-stream marker travels as BUFFER_FROM_HOST_ZEROLEN.
 * @base: the library base
 *
 * Return: TRUE from MMAL 12.2 on.
 */
BOOL vpu_has_zerolen(const struct VideoCoreBase *base)
{
	const struct mmal_client *client = &base->mmal;
	return client->version_major > 12 || (client->version_major == 12 && client->version_minor >= 2);
}

/**
 * vpu_has_flush_sync() - Whether a flush can be serialised with the bulks before it.
 * @base: the library base
 *
 * Return: TRUE from firmware 15 on (userland's flush_sync).
 */
BOOL vpu_has_flush_sync(const struct VideoCoreBase *base)
{
	return base->mmal.version_major >= 15;
}

/**
 * service_close() - Close the service: no component is left (mmal_idle()), or the first one failed.
 * @base: the library base, mmal_lock held
 *
 * Only sinks are still parked (every buffer was let go with its component),
 * and their receives will never run.
 */
static void service_close(struct VideoCoreBase *base)
{
	PERF_T0(t0);
	if (base->reset_guarded && reset_guard_remove(&base->reset_guard)) /* before vchiq.device's can go: this one sits on it */
		base->reset_guarded = FALSE;
	link_close(base);
	rx_parked_drop(base);
	PERF_ADD(&base->perf, VCPERF_MMAL_CLOSE, t0);
}

/**
 * service_open() - Open the "mmal" service, with the DMA pool, unless it is open and alive.
 * @base: the library base, mmal_lock held
 *
 * A service kept open past its last component (mmal_idle()) and lost since
 * is closed and opened afresh; one lost with components still in use stays
 * as it is until their closes bring it down.
 *
 * Return: MMAL_SUCCESS, or LINK_ERR_*.
 */
static s32 service_open(struct VideoCoreBase *base)
{
	struct link *link = &base->link;
	if (link->state != LINK_CLOSED)
	{
		if (link_ready(link))
			return MMAL_SUCCESS;
		if (base->mmal.component_count != 0)
			return LINK_ERR_STATE;
		service_close(base);
	}
	if (!base->reach.known)
		link_reach(base);
	if (!dma_pool_make(base))
	{
		Kprintf("[videocore] no DMA-reachable memory\n");
		return LINK_ERR_STATE;
	}
	PERF_T0(t0);
	s32 status = link_open(base);
	if (status != 0)
		return status;
	if (!base->reset_guarded) /* vchiq.device's is in by now, so this one runs first */
		base->reset_guarded = reset_guard_install(&base->reset_guard, base->SysBase, sessions_reset, base, (CONST_STRPTR)LIBRARY_NAME);
	version_read(base);
	PERF_ADD(&base->perf, VCPERF_MMAL_OPEN, t0);
	Kprintf("[videocore] service open, peer version %ld, VPU MMAL %ld.%ld (minimum %ld)\n", (LONG)link->mmal.iovq_PeerVersion,
	        base->mmal.version_major, base->mmal.version_minor, base->mmal.version_minimum);
	return MMAL_SUCCESS;
}

/* ---- components ---------------------------------------------------------- */

/**
 * component_free() - Free a component and everything of it, without a message to the VPU.
 * @base:      the library base, mmal_lock held (released while waiting)
 * @component: the component, off the list or never on it; its ports may be missing
 *
 * The VPU side is gone or being abandoned.  An event buffer whose payload
 * is still owed is let go like a port's buffers.  The caller's buffers
 * outlive the port; their memory is the caller's again once the VPU has
 * let go of their imports, which is waited for.
 */
static void component_free(struct VideoCoreBase *base, struct mmal_component *component)
{
	struct ExecBase *SysBase = base->SysBase;

	for (u32 i = 0; i < CLIENT_EVENT_BUFFERS; i++)
		rx_forget(base, &component->events[i]);
	if (component->ports != NULL)
	{
		for (u32 i = 0; i < component->port_count; i++)
		{
			struct mmal_port *port = &component->ports[i];
			if (port->tunnel != NULL)
				port->tunnel->tunnel = NULL; /* the far end must not point at freed memory */
			port_buffers_forget(base, port);
		}
		FreeMem(component->ports, component->port_count * sizeof(struct mmal_port));
	}
	if (component->event_payload != NULL)
		dma_free(base->mmal.pool, component->event_payload);
	FreeMem(component, sizeof(*component));
	smem_released_wait(base);
}

/**
 * ports_init() - Set up a run of a new component's ports, all of one type.
 * @component: the component, its ports allocated
 * @first:     the run's first place in ports[]
 * @count:     its ports
 * @type:      MMAL_PORT_TYPE_*
 */
static void ports_init(struct mmal_component *component, u32 first, u32 count, u32 type)
{
	for (u32 index = 0; index < count; index++)
	{
		struct mmal_port *port = &component->ports[first + index];
		port->component = component;
		port->type = type;
		port->index = (u16)index;
		port->index_all = (u16)(first + index);
		_NewMinList(&port->queue);
	}
}

/**
 * events_init() - Fill a new component's event pool: each buffer a slice of its event payload.
 * @base:      the library base, mmal_lock held
 * @component: the component, its event payload allocated
 */
static void events_init(struct VideoCoreBase *base, struct mmal_component *component)
{
	for (u32 i = 0; i < CLIENT_EVENT_BUFFERS; i++)
	{
		struct mmal_buffer *event = &component->events[i];
		event->magic = BUFFER_MAGIC; /* a bulk completion may name it: marked once, for good */
		event->kind = BUFFER_EVENT;
		event->component = component;
		event->data = component->event_payload + i * CLIENT_EVENT_BUFFER_SIZE;
		event->alloc_size = CLIENT_EVENT_BUFFER_SIZE;
		event_recycle(base, event);
	}
}

/**
 * component_layout() - Allocate and lay out a new component's ports and event buffers.
 * @base:      the library base, mmal_lock held
 * @component: the component, created in the VPU
 * @inputs:    its input ports, as the VPU counts them
 * @outputs:   its output ports
 * @clocks:    its clock ports
 *
 * ports[] holds the control port, then the inputs, the outputs, the clocks.
 *
 * Return: 0, VCERR_TRANSPORT for more ports than the protocol allows, or
 * VCERR_NOMEM.
 */
static LONG component_layout(struct VideoCoreBase *base, struct mmal_component *component, u32 inputs, u32 outputs, u32 clocks)
{
	struct ExecBase *SysBase = base->SysBase;

	if (inputs > MMAL_WIRE_MAX_PORTS || outputs > MMAL_WIRE_MAX_PORTS || clocks > MMAL_WIRE_MAX_PORTS)
		return VCERR_TRANSPORT; /* the counts are the VPU's: checked before they size anything */

	component->port_count = 1u + inputs + outputs + clocks;
	component->ports = AllocMem(component->port_count * sizeof(struct mmal_port), MEMF_PUBLIC | MEMF_CLEAR);
	component->event_payload = dma_alloc(base->mmal.pool, DMA_ALIGN_MIN, CLIENT_EVENT_BUFFERS * CLIENT_EVENT_BUFFER_SIZE);
	if (component->ports == NULL || component->event_payload == NULL)
		return VCERR_NOMEM;

	ports_init(component, 0, 1, MMAL_PORT_TYPE_CONTROL);
	ports_init(component, 1, inputs, MMAL_PORT_TYPE_INPUT);
	ports_init(component, 1 + inputs, outputs, MMAL_PORT_TYPE_OUTPUT);
	ports_init(component, 1 + inputs + outputs, clocks, MMAL_PORT_TYPE_CLOCK);
	component->control = &component->ports[0];
	component->input = inputs != 0 ? &component->ports[1] : NULL;
	component->output = outputs != 0 ? &component->ports[1 + inputs] : NULL;
	events_init(base, component);
	return 0;
}

/**
 * component_ports_read() - Read every port of a new component from the VPU: its handle, state and format.
 * @base:      the library base, mmal_lock held
 * @component: the component, its ports laid out
 *
 * Each port's first state is what a kept component's ports start from again.
 *
 * Return: 0, or the first port's error as a VCERR_*.
 */
static LONG component_ports_read(struct VideoCoreBase *base, struct mmal_component *component)
{
	for (u32 i = 0; i < component->port_count; i++)
	{
		struct mmal_port *port = &component->ports[i];
		s32 status = port_info_get(base, port);
		if (status != MMAL_SUCCESS)
		{
			Kprintf("[videocore] read %s port %ld: %ld\n", component->name, i, status);
			return vcerr_from_status(status);
		}
		port->initial_format = port->format;
		port->initial_buffer_num = port->buffer_num;
		port->initial_buffer_size = port->buffer_size;
	}
	return 0;
}

/**
 * component_build() - Create a component in the VPU, lay out its ports and read them.
 * @base:      the library base, mmal_lock held, the service open
 * @component: the component, allocated and named
 * @name:      the VPU's name for it, whole (component->name may be cut short)
 *
 * Once the VPU has created it, a failure destroys it there again.
 *
 * Return: 0, or the error as a VCERR_*.
 */
static LONG component_build(struct VideoCoreBase *base, struct mmal_component *component, const char *name)
{
	PERF_T0(t0);
	struct mmal_msg_component_create request;
	struct mmal_msg_component_create_reply reply;
	u32 reply_size = sizeof(reply);
	memset(&request, 0, sizeof(request));
	request.client_component = le32((u32)component); /* the cookie its events come back with */
	strlcpy((char *)request.name, name, sizeof(request.name));
	s32 status = call_vpu(base, &request, sizeof(request), MMAL_MSG_COMPONENT_CREATE, &reply, &reply_size, sizeof(reply), NULL, 0);
	if (status != MMAL_SUCCESS)
	{
		Kprintf("[videocore] create %s: %ld\n", name, status);
		return vcerr_from_status(status); /* refused: nothing to undo in the VPU */
	}

	component->handle = le32(reply.component_handle);
	u32 inputs = le32(reply.input_num), outputs = le32(reply.output_num), clocks = le32(reply.clock_num);
	LONG error = component_layout(base, component, inputs, outputs, clocks);
	if (error == 0)
		error = component_ports_read(base, component);
	if (error != 0)
	{
		(void)component_call(base, component, MMAL_MSG_COMPONENT_DESTROY); /* the VPU has it: undo that */
		return error;
	}
	Kprintf("[videocore] %s created: handle %ld, %ld in, %ld out, %ld clock\n", name, component->handle, inputs, outputs, clocks);
	PERF_ADD(&base->perf, VCPERF_COMPONENT, t0);
	return 0;
}

/* ---- kept components ------------------------------------------------------ */

/**
 * kept_find() - A kept component of a name.
 * @base: the library base, mmal_lock held
 * @name: the VPU's name for it; NULL for any
 *
 * Return: the component, NULL if none is kept.
 */
static struct mmal_component *kept_find(struct VideoCoreBase *base, const char *name)
{
	for (struct MinNode *node = base->mmal.components.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
	{
		struct mmal_component *component = (struct mmal_component *)node;
		if (component->kept && (name == NULL || strcmp(component->name, name) == 0))
			return component;
	}
	return NULL;
}

/**
 * kept_queue_drain() - Give back what waits on a kept component's port queues: events of its last use, or since.
 * @base:      the library base, mmal_lock held
 * @component: the kept component, its buffers let go
 */
static void kept_queue_drain(struct VideoCoreBase *base, struct mmal_component *component)
{
	struct ExecBase *SysBase = base->SysBase;
	for (u32 i = 0; i < component->port_count; i++)
	{
		struct mmal_buffer *left;
		while ((left = (struct mmal_buffer *)RemHead((struct List *)&component->ports[i].queue)) != NULL)
			if (left->kind == BUFFER_EVENT)
				event_recycle(base, left);
	}
}

/**
 * kept_eligible() - Whether a component may be kept for a later use.
 * @base:      the library base, mmal_lock held
 * @component: the component
 *
 * Return: TRUE if it holds no error, its name was not cut short, none of its
 * name is kept already, and every port but the control port is stopped and
 * in no tunnel.
 */
static BOOL kept_eligible(struct VideoCoreBase *base, const struct mmal_component *component)
{
	if (component->error != 0 || strlen(component->name) >= sizeof(component->name) - 1u || kept_find(base, component->name) != NULL)
		return FALSE;
	for (u32 i = 0; i < component->port_count; i++)
	{
		const struct mmal_port *port = &component->ports[i];
		if (port->tunnel != NULL || (port->type != MMAL_PORT_TYPE_CONTROL && port->enabled))
			return FALSE;
	}
	return TRUE;
}

/**
 * kept_park() - Keep a component: its buffers let go, the component disabled, no task to signal.
 * @base:      the library base, mmal_lock held (released while waiting)
 * @component: the component, kept_eligible()
 *
 * Its buffers are let go (the caller frees them once the VPU has let go of
 * their imports), the events left on its queues go back to its pool, and
 * the component itself is disabled, which ends its stream (port disables
 * alone leave the image decoder at its end of stream: the next picture
 * ends at once); the next use enables it as the first did.
 *
 * Return: TRUE if kept; FALSE if the disable failed (the caller destroys it).
 */
static BOOL kept_park(struct VideoCoreBase *base, struct mmal_component *component)
{
	for (u32 i = 0; i < component->port_count; i++)
	{
		port_buffers_forget(base, &component->ports[i]);
		component->ports[i].sent_data = FALSE;
	}
	smem_released_wait(base);
	if (component->enabled)
	{
		s32 status = component_call(base, component, MMAL_MSG_COMPONENT_DISABLE);
		if (status != MMAL_SUCCESS && status != MMAL_ENOSYS)
		{
			Kprintf("[videocore] disable %s to keep it: %ld\n", component->name, status);
			return FALSE;
		}
		component->enabled = FALSE;
	}
	kept_queue_drain(base, component);
	component->kept = TRUE;
	component->task = NULL;
	component->sigmask = 0;
	KprintfT("[videocore] %s kept\n", component->name);
	return TRUE;
}

/**
 * kept_take() - A kept component back in use: its ports as the VPU gave them at the first create.
 * @base:      the library base, mmal_lock held
 * @component: the kept component
 * @task:      the task to signal from now
 * @sigmask:   its signal
 *
 * The VPU keeps every port's last committed format: the caller commits the
 * restored one, as after a fresh create.  Events that came while it was
 * kept are dropped.  Parameters set during the last use stay with the VPU;
 * component->reused tells the caller.
 */
static void kept_take(struct VideoCoreBase *base, struct mmal_component *component, struct Task *task, ULONG sigmask)
{
	kept_queue_drain(base, component);
	for (u32 i = 0; i < component->port_count; i++)
	{
		struct mmal_port *port = &component->ports[i];
		port->format = port->initial_format;
		port->buffer_num = port->initial_buffer_num;
		port->buffer_size = port->initial_buffer_size;
	}
	component->kept = FALSE;
	component->reused = TRUE;
	component->task = task;
	component->sigmask = sigmask;
}

/**
 * kept_drop_if_lost() - Let the kept components go without a word to the VPU if the service went.
 * @base: the library base, mmal_lock held
 *
 * They went with it; the next create opens the service afresh.
 */
static void kept_drop_if_lost(struct VideoCoreBase *base)
{
	struct ExecBase *SysBase = base->SysBase;
	if (base->link.state == LINK_CLOSED || link_ready(&base->link))
		return;
	struct mmal_component *kept;
	while ((kept = kept_find(base, NULL)) != NULL)
	{
		Remove((struct Node *)&kept->node);
		component_free(base, kept);
		base->mmal.component_count--;
	}
}

/**
 * mmal_component_create() - Create a VPU component and read its ports.
 * @base:      the library base
 * @name:      the VPU's name for it, e.g. "ril.isp"
 * @task:      the task Signal()led whenever a port of it has something back
 * @sigmask:   the signal
 * @component: where the component goes, NULL on failure
 *
 * The first component opens the service, which then stays open past the
 * last (mmal_idle() closes it); a failed first one closes it again.
 *
 * Return: 0, VCERR_NOMEM, or the VPU's refusal as a VCERR_* (VCERR_DEVMEM:
 * no room in the VPU's memory).
 */
LONG mmal_component_create(struct VideoCoreBase *base, const char *name, struct Task *task, ULONG sigmask,
                           struct mmal_component **component)
{
	struct ExecBase *SysBase = base->SysBase;
	struct mmal_client *client = &base->mmal;

	*component = NULL;
	ObtainSemaphore(&base->mmal_lock);
	kept_drop_if_lost(base);
	s32 status = service_open(base);
	if (status != MMAL_SUCCESS)
	{
		ReleaseSemaphore(&base->mmal_lock);
		return vcerr_from_status(status);
	}

	LONG error = VCERR_NOMEM;
	struct mmal_component *created = AllocMem(sizeof(*created), MEMF_PUBLIC | MEMF_CLEAR);
	if (created != NULL)
	{
		_NewMinList(&created->free_events);
		strlcpy(created->name, name, sizeof(created->name)); /* for the logs */
		created->task = task;
		created->sigmask = sigmask;
		error = component_build(base, created, name);
	}
	if (error == 0)
	{
		AddTail((struct List *)&client->components, (struct Node *)&created->node);
		client->component_count++;
		*component = created;
	}
	else
	{
		if (created != NULL)
			component_free(base, created);
		if (client->component_count == 0)
			service_close(base); /* none left after a failed create: the link may be why */
	}
	ReleaseSemaphore(&base->mmal_lock);
	return error;
}

/**
 * mmal_component_take() - Take a component kept by mmal_component_keep(), its ports as at its first create.
 * @base:    the library base
 * @name:    the VPU's name for it
 * @task:    the task Signal()led from now on
 * @sigmask: the signal
 *
 * Return: the component (component->reused set), NULL if none of @name is
 * kept or the service went meanwhile (the kept ones went with it).
 */
struct mmal_component *mmal_component_take(struct VideoCoreBase *base, const char *name, struct Task *task, ULONG sigmask)
{
	struct ExecBase *SysBase = base->SysBase;
	ObtainSemaphore(&base->mmal_lock);
	kept_drop_if_lost(base);
	struct mmal_component *kept = kept_find(base, name);
	if (kept != NULL)
		kept_take(base, kept, task, sigmask);
	ReleaseSemaphore(&base->mmal_lock);
	return kept;
}

/**
 * mmal_service_open() - Open the service as the first component would, ahead of it.
 * @base: the library base, from a task
 *
 * An image call asks before it decides what the VPU's memory has to hold:
 * whether buffers can be imported is known only once the service is open.
 * It stays open like one a component opened (mmal_idle() closes it).
 *
 * Return: 0 or the error.
 */
LONG mmal_service_open(struct VideoCoreBase *base)
{
	struct ExecBase *SysBase = base->SysBase;

	ObtainSemaphore(&base->mmal_lock);
	kept_drop_if_lost(base);
	s32 status = service_open(base);
	ReleaseSemaphore(&base->mmal_lock);
	return vcerr_from_status(status);
}

/**
 * mmal_component_destroy() - Stop a component, break its tunnels, destroy it, free it.
 * @base:      the library base
 * @component: the component
 *
 * Its enabled ports are disabled first, in port order, so its buffers come
 * home; then its tunnels are undone and the VPU destroys it.  Each step is
 * best effort: a failure is logged and the component is freed either way;
 * the caller's buffers are its own again on return.  The service stays
 * open after the last component: mmal_idle() closes it.
 */
void mmal_component_destroy(struct VideoCoreBase *base, struct mmal_component *component)
{
	struct ExecBase *SysBase = base->SysBase;

	ObtainSemaphore(&base->mmal_lock);
	PERF_T0(t0);
	for (u32 i = 0; i < component->port_count; i++)
		if (component->ports[i].enabled)
			(void)port_disable(base, &component->ports[i]); /* a tunnel's far end follows, so it is skipped when its turn comes */

	for (u32 i = 0; i < component->port_count; i++)
		if (component->ports[i].tunnel != NULL)
			(void)tunnel_break(base, &component->ports[i]);

	s32 status = component_call(base, component, MMAL_MSG_COMPONENT_DESTROY);
	if (status != MMAL_SUCCESS)
		Kprintf("[videocore] destroy %s: %ld\n", component->name, status);
	Remove((struct Node *)&component->node);
	component_free(base, component);
	PERF_ADD(&base->perf, VCPERF_DESTROY, t0);
	base->mmal.component_count--; /* the service stays open for the next one: mmal_idle() closes it */
	ReleaseSemaphore(&base->mmal_lock);
}

/**
 * mmal_component_keep() - Keep a component for the next create of its name instead of destroying it.
 * @base:      the library base
 * @component: the component; its ports but the control port stopped, none in a tunnel
 *
 * It signals no task while kept.  One component of a name is kept at a time.
 *
 * Return: TRUE if kept (kept_park()); FALSE if it may not be
 * (kept_eligible()) or its disable failed: the caller destroys it.
 */
BOOL mmal_component_keep(struct VideoCoreBase *base, struct mmal_component *component)
{
	struct ExecBase *SysBase = base->SysBase;

	ObtainSemaphore(&base->mmal_lock);
	BOOL kept = kept_eligible(base, component) && kept_park(base, component);
	ReleaseSemaphore(&base->mmal_lock);
	return kept;
}

/**
 * mmal_component_enable() - Enable a component's control port, then the component.
 * @base:      the library base
 * @component: the component
 *
 * The control port carries the component's events, so it runs first.  A
 * component without the notion of enabling (ENOSYS) counts as enabled, as
 * userland takes it.
 *
 * Return: 0 or the error.
 */
LONG mmal_component_enable(struct VideoCoreBase *base, struct mmal_component *component)
{
	struct ExecBase *SysBase = base->SysBase;

	ObtainSemaphore(&base->mmal_lock);
	s32 status = port_enable(base, component->control);
	if (status == MMAL_SUCCESS && !component->enabled) /* a kept one runs still */
	{
		status = component_call(base, component, MMAL_MSG_COMPONENT_ENABLE);
		if (status == MMAL_ENOSYS)
			status = MMAL_SUCCESS;
		if (status != MMAL_SUCCESS)
			Kprintf("[videocore] enable %s: %ld\n", component->name, status);
		component->enabled = status == MMAL_SUCCESS;
	}
	ReleaseSemaphore(&base->mmal_lock);
	return vcerr_from_status(status);
}

/**
 * mmal_component_error() - The component's error: the first ERROR event of its control or output ports.
 * @base:      the library base
 * @component: the component
 *
 * Return: 0 while none came, else the VCERR_*.
 */
LONG mmal_component_error(struct VideoCoreBase *base, struct mmal_component *component)
{
	struct ExecBase *SysBase = base->SysBase;

	ObtainSemaphore(&base->mmal_lock); /* the receive task sets it */
	LONG error = component->error;
	ReleaseSemaphore(&base->mmal_lock);
	return error;
}

/* ---- lifetime ------------------------------------------------------------ */

/**
 * mmal_init() - The client's state at library init: no component, no service.
 * @base: the library base
 *
 * The DMA pool is made at the first service open, not here: an idle library
 * holds no DMA memory until a session or image call needs the VPU.
 */
void mmal_init(struct VideoCoreBase *base)
{
	struct ExecBase *SysBase = base->SysBase;
	struct mmal_client *client = &base->mmal;

	_NewMinList(&client->components);
	_NewMinList(&client->pending_rx);
	dma_mem_init(&base->dma, SysBase);
}

/**
 * mmal_info() - The client's counters and the VPU's version.
 * @base:     the library base
 * @counters: where they go
 */
void mmal_info(struct VideoCoreBase *base, struct mmal_counters *counters)
{
	struct ExecBase *SysBase = base->SysBase;
	const struct mmal_client *client = &base->mmal;

	ObtainSemaphore(&base->mmal_lock);
	BOOL open = base->link.state != LINK_CLOSED;
	counters->version_major = open ? client->version_major : 0;
	counters->version_minor = open ? client->version_minor : 0;
	counters->events = client->events;
	counters->events_dropped = client->events_dropped;
	counters->buffers_back = client->buffers_back;
	counters->stray_messages = client->stray_messages;
	ReleaseSemaphore(&base->mmal_lock);
}

/**
 * mmal_kept_release() - Destroy the kept components, and with them the VPU memory they hold.
 * @base: the library base, from a task that may wait
 */
void mmal_kept_release(struct VideoCoreBase *base)
{
	struct ExecBase *SysBase = base->SysBase;
	for (;;)
	{
		ObtainSemaphore(&base->mmal_lock);
		struct mmal_component *kept = kept_find(base, NULL);
		ReleaseSemaphore(&base->mmal_lock);
		if (kept == NULL)
			break;
		mmal_component_destroy(base, kept); /* it takes mmal_lock itself */
	}
}

/**
 * mmal_idle() - Destroy the kept components, then close the service kept open past the last one.
 * @base: the library base, from a task that may wait (VCB_Idle())
 *
 * Opening the service costs each component's caller a VCHIQ open, the
 * receive task's start and the version read, closing it as much again: so
 * it stays open from the first component until here.  With a component in
 * use left it stays.
 */
void mmal_idle(struct VideoCoreBase *base)
{
	struct ExecBase *SysBase = base->SysBase;
	mmal_kept_release(base);
	ObtainSemaphore(&base->mmal_lock);
	if (base->mmal.component_count == 0)
		service_close(base);
	ReleaseSemaphore(&base->mmal_lock);
}

/**
 * mmal_expunge() - Whether the client is down for an expunge.
 * @base: the library base
 *
 * A close whose requests did not all come home left the receive task
 * running for them; once they are home it is joined here (link_settle()).
 *
 * Return: TRUE with no component, the service closed and no receive task left.
 */
BOOL mmal_expunge(struct VideoCoreBase *base)
{
	struct ExecBase *SysBase = base->SysBase;

	ObtainSemaphore(&base->mmal_lock);
	BOOL down = base->mmal.component_count == 0 && link_settle(base);
	ReleaseSemaphore(&base->mmal_lock);
	return down;
}

/**
 * mmal_shutdown() - Free the DMA pool and the scratch, at expunge.
 * @base: the library base, the client down (mmal_expunge())
 */
void mmal_shutdown(struct VideoCoreBase *base)
{
	struct mmal_client *client = &base->mmal;
	if (client->pool == NULL)
		return; /* no service was ever opened */
	if (client->scratch != NULL)
		dma_free(client->pool, client->scratch);
	client->scratch = NULL;
	dma_pool_delete(client->pool);
	client->pool = NULL;
}
