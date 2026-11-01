/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * videocore_link.c - the link to vchiq.device: the "mmal" service and
 * "SMEM" beside it, a pool of requests, and the receive task
 * (videocore_link.h has the model).
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#endif

#include <exec/memory.h>
#include <dos/dos.h>

#include <driver_task.h>
#include <memory.h>
#include <minlist.h>

#include <videocore_private.h>
#include <mmal_proto.h>
#include <smem_proto.h>

/* ---- waiting --------------------------------------------------------------------- */

/**
 * link_wait() - Wait under mmal_lock until a condition holds.
 * @base:       the library base, mmal_lock held (released while waiting)
 * @pred:       the condition, called with @base and @arg
 * @arg:        its argument
 * @timeout_ms: how long at most
 *
 * The receive task wakes every waiter after each batch of completions, so a
 * condition on anything a completion changes is seen at once.
 *
 * Return: 0, LINK_ERR_TIMEOUT, or LINK_ERR_STATE when the wait could not be
 * set up (no signal, no timer).
 */
s32 link_wait(struct VideoCoreBase *base, drv_wait_pred_t pred, APTR arg, ULONG timeout_ms)
{
	struct ExecBase *SysBase = base->SysBase;
	LONG result = drv_wait_event(&base->waitq, SysBase, &base->mmal_lock, pred, base, arg, timeout_ms);
	return result == DRV_WAIT_OK ? 0 : result == DRV_WAIT_TIMEOUT ? LINK_ERR_TIMEOUT : LINK_ERR_STATE;
}

/**
 * request_free() - link_wait() condition: a request is free, or none will be.
 * @base: the library base
 * @arg:  unused
 *
 * Return: TRUE if the free list has one, or the link no longer takes requests.
 */
static BOOL request_free(APTR base, APTR arg)
{
	(void)arg;
	const struct link *link = &((struct VideoCoreBase *)base)->link;
	return link->free.mlh_Head->mln_Succ != NULL || !link_ready(link);
}

/**
 * requests_home() - link_wait() condition: no request is at the device.
 * @base: the library base
 * @arg:  unused
 *
 * Return: TRUE when every request has come back.
 */
static BOOL requests_home(APTR base, APTR arg)
{
	(void)arg;
	return ((struct VideoCoreBase *)base)->link.outstanding == 0;
}

/* ---- the pool -------------------------------------------------------------------- */

/**
 * request_get() - Take a free request, ready to send.
 * @base: the library base
 * @wait: wait for one if none is free (callers only, never the receive task)
 *
 * Return: the request (a copy of the opened service's request since
 * link_open()); NULL if none is free (after LINK_WAIT_MS with @wait) or the
 * link no longer takes requests.
 */
static struct link_request *request_get(struct VideoCoreBase *base, BOOL wait)
{
	struct ExecBase *SysBase = base->SysBase;
	struct link *link = &base->link;

	if (wait && link_wait(base, request_free, NULL, LINK_WAIT_MS) != 0)
		return NULL;
	if (!link_ready(link))
		return NULL;
	struct MinNode *node = (struct MinNode *)RemHead((struct List *)&link->free);
	return node != NULL ? CONTAINER_OF(node, struct link_request, node) : NULL;
}

/**
 * request_put() - Return a request to the free list.
 * @base:    the library base
 * @request: a request not at the device
 */
static void request_put(struct VideoCoreBase *base, struct link_request *request)
{
	struct ExecBase *SysBase = base->SysBase;
	request->at_device = FALSE;
	AddTail((struct List *)&base->link.free, (struct Node *)&request->node);
}

/**
 * request_send() - Send a request to the device; it comes back to the receive task.
 * @base:    the library base
 * @request: from request_get()
 * @kind:    what the request is
 * @command: CMD_READ, CMD_WRITE, NSCMD_VQ_BULKWRITE or NSCMD_VQ_BULKREAD
 * @data:    the buffer
 * @size:    its length
 * @context: what the completion hands on (enum link_kind says what)
 *
 * The request goes to the unit of its kind's service: SMEM's, or mmal's.
 */
static void request_send(struct VideoCoreBase *base, struct link_request *request, enum link_kind kind, UWORD command,
                         APTR data, u32 size, APTR context)
{
	struct ExecBase *SysBase = base->SysBase;
	struct link *link = &base->link;
	BOOL smem = kind == LINK_SMEM_RX || kind == LINK_SMEM_TX;
	request->io.iovq_Req.io_Unit = (smem ? &link->smem : &link->mmal)->iovq_Req.io_Unit;
	request->kind = kind;
	request->context = context;
	request->at_device = TRUE;
	request->io.iovq_Req.io_Command = command;
	request->io.iovq_Req.io_Data = data;
	request->io.iovq_Req.io_Length = size;
	link->outstanding++;
	SendIO((struct IORequest *)&request->io);
}

/**
 * post_read() - Send a request as a read for the VPU's next message on a service.
 * @base:    the library base
 * @request: a request not at the device
 * @kind:    LINK_MSG_RX (mmal) or LINK_SMEM_RX
 */
static void post_read(struct VideoCoreBase *base, struct link_request *request, enum link_kind kind)
{
	request_send(base, request, kind, CMD_READ, request->msg, LINK_MESSAGE_BYTES, NULL);
}

/* ---- sending (callers) ------------------------------------------------------------- */

/**
 * send_message() - Send a message to the VPU, with the payload that follows it.
 * @base:      the library base, mmal_lock held
 * @kind:      LINK_MSG_TX_CALL, LINK_MSG_TX_BUFFER or LINK_SMEM_TX: the
 *             service, and who hears of a failed write
 * @msg:       the message, copied: the caller's buffer is free on return
 * @size:      its length, at most LINK_MESSAGE_BYTES
 * @context:   the call's cookie, the buffer or the import's cookie (struct link_request.context)
 * @bulk:      the payload, sent by bulk transfer right after the message; NULL for none
 * @bulk_size: its length; 0 for none
 * @wait:      wait for a free request (callers only, never the receive task)
 *
 * Both requests are taken before either is sent: mmal_lock may be released
 * while waiting for one, never between the two sends, so the payload follows
 * its message.
 *
 * Return: 0, LINK_ERR_PARAM for an oversized message, LINK_ERR_STATE if the
 * link (or SMEM) is closed or dead, LINK_ERR_NOSPACE if no request came free
 * (in time).
 */
static s32 send_message(struct VideoCoreBase *base, enum link_kind kind, const void *msg, u32 size, APTR context, APTR bulk,
                        u32 bulk_size, BOOL wait)
{
	struct ExecBase *SysBase = base->SysBase;
	if (size > LINK_MESSAGE_BYTES)
		return LINK_ERR_PARAM;
	if (kind == LINK_SMEM_TX && !base->link.smem_open)
		return LINK_ERR_STATE;
	struct link_request *write = request_get(base, wait);
	struct link_request *payload = write != NULL && bulk_size != 0 ? request_get(base, wait) : NULL;
	if (write == NULL || (bulk_size != 0 && payload == NULL))
	{
		if (write != NULL)
			request_put(base, write);
		return link_ready(&base->link) ? LINK_ERR_NOSPACE : LINK_ERR_STATE;
	}
	CopyMem((APTR)msg, write->msg, size);
	request_send(base, write, kind, CMD_WRITE, write->msg, size, context);
	if (payload != NULL)
		request_send(base, payload, LINK_BULK_TX, NSCMD_VQ_BULKWRITE, bulk, bulk_size, NULL);
	return 0;
}

/**
 * link_send_call() - Send a control message whose reply a call waits for.
 * @base:      the library base, mmal_lock held
 * @msg:       the message, copied
 * @size:      its length, at most LINK_MESSAGE_BYTES
 * @cookie:    the call's cookie: handed to mmal_call_failed() if the write fails
 * @bulk:      a payload to follow the message (the synchronising flush's dummy), or NULL
 * @bulk_size: its length, or 0
 *
 * Return: as send_message().
 */
s32 link_send_call(struct VideoCoreBase *base, const void *msg, u32 size, u32 cookie, APTR bulk, u32 bulk_size)
{
	return send_message(base, LINK_MSG_TX_CALL, msg, size, (APTR)cookie, bulk, bulk_size, TRUE);
}

/**
 * link_send_buffer() - Send the message that hands the VPU a buffer, with its payload.
 * @base:      the library base, mmal_lock held
 * @msg:       the message, copied
 * @size:      its length, at most LINK_MESSAGE_BYTES
 * @buffer:    the buffer: returned failed through mmal_buffer_failed() if the write fails
 * @bulk:      its payload, or NULL
 * @bulk_size: the payload's length, or 0
 *
 * Return: as send_message().
 */
s32 link_send_buffer(struct VideoCoreBase *base, const void *msg, u32 size, struct mmal_buffer *buffer, APTR bulk,
                     u32 bulk_size)
{
	return send_message(base, LINK_MSG_TX_BUFFER, msg, size, buffer, bulk, bulk_size, TRUE);
}

/**
 * link_send_smem() - Send a message on the SMEM service.
 * @base:   the library base, mmal_lock held
 * @msg:    the message, copied
 * @size:   its length, at most LINK_MESSAGE_BYTES
 * @cookie: the import's cookie: handed to smem_send_failed() if the write fails; 0 for none
 * @wait:   wait for a free request; FALSE from the receive task, or from a
 *          walk that must keep mmal_lock (smem_release())
 *
 * Return: as send_message().
 */
s32 link_send_smem(struct VideoCoreBase *base, const void *msg, u32 size, u32 cookie, BOOL wait)
{
	return send_message(base, LINK_SMEM_TX, msg, size, (APTR)cookie, NULL, 0, wait);
}

/**
 * link_bulk_receive() - Receive a payload the VPU announced.
 * @base:   the library base, mmal_lock held (the receive task, or a caller)
 * @data:   where it goes
 * @size:   its length
 * @buffer: handed to mmal_rx_done() when it is in
 *
 * Never waits: the receive task calls it.
 *
 * Return: 0, LINK_ERR_NOSPACE if no request is free (park it and try again
 * once one comes back), LINK_ERR_STATE if the link is closed or dead.
 */
s32 link_bulk_receive(struct VideoCoreBase *base, APTR data, u32 size, struct mmal_buffer *buffer)
{
	struct link_request *request = request_get(base, FALSE);
	if (request == NULL)
		return link_ready(&base->link) ? LINK_ERR_NOSPACE : LINK_ERR_STATE;
	request_send(base, request, LINK_BULK_RX, NSCMD_VQ_BULKREAD, data, size, buffer);
	return 0;
}

/**
 * link_disown() - Make the requests at the device that carry a buffer name none.
 * @base:   the library base, mmal_lock held
 * @buffer: the struct mmal_buffer being let go
 *
 * A buffer's message or bulk receive may still be at the device when its
 * memory goes; the completion then reaches mmal_buffer_failed() or
 * mmal_rx_done() with NULL instead.  The transfer itself stays in its place.
 */
void link_disown(struct VideoCoreBase *base, struct mmal_buffer *buffer)
{
	struct link *link = &base->link;
	for (u32 i = 0; link->requests != NULL && i < LINK_REQUESTS; i++)
	{
		struct link_request *request = &link->requests[i];
		if (request->at_device && (request->kind == LINK_MSG_TX_BUFFER || request->kind == LINK_BULK_RX) && request->context == buffer)
			request->context = NULL;
	}
}

/* ---- the receive task -------------------------------------------------------------- */

/**
 * link_dead() - Take the service as gone: nothing new is sent, both clients hear of it.
 * @base: the library base, mmal_lock held
 *
 * The VPU closed the service, the connection went, or the VPU stopped
 * answering (a call's reply never came, mmal_call.c).  Every waiting call
 * fails now and each later one at once rather than at its own timeout; the
 * imports are given up (smem_lost()).  A link already closing goes on
 * closing.  The next open after the close tries the service anew.
 */
void link_dead(struct VideoCoreBase *base)
{
	if (base->link.state != LINK_READY)
		return;
	base->link.state = LINK_DEAD;
	mmal_lost(base);
	smem_lost(base);
}

/**
 * service_lost() - The VPU closed the mmal service, or the connection went.
 * @base:  the library base, mmal_lock held
 * @error: the request's error, for the log
 */
static void service_lost(struct VideoCoreBase *base, BYTE error)
{
	(void)error; /* only the debug log reads it */
	if (base->link.state == LINK_READY)
		Kprintf("[videocore] the mmal service is gone (%ld)\n", (LONG)error);
	link_dead(base);
}

/**
 * message_received() - A read came back: its message goes to the MMAL client.
 * @base:    the library base, mmal_lock held
 * @request: the LINK_MSG_RX request
 * @error:   its error
 *
 * A message longer than LINK_MESSAGE_BYTES, which MMAL never sends, is dropped.
 */
static void message_received(struct VideoCoreBase *base, struct link_request *request, BYTE error)
{
	if (error == 0)
		mmal_message(base, request->msg, request->io.iovq_Req.io_Actual);
	else if (error == VQERR_TRUNCATED)
		Kprintf("[videocore] a message of %ld bytes dropped (at most %ld)\n", request->io.iovq_Req.io_Actual, (ULONG)LINK_MESSAGE_BYTES);
}

/**
 * transfer_done() - A write or bulk transfer came back: tell its owner how it went.
 * @base:    the library base, mmal_lock held
 * @request: any request but a read
 * @error:   its error
 */
static void transfer_done(struct VideoCoreBase *base, struct link_request *request, BYTE error)
{
	switch (request->kind)
	{
	case LINK_MSG_TX_CALL:
		if (error != 0)
			mmal_call_failed(base, (u32)request->context, base->link.state == LINK_DEAD ? LINK_ERR_STATE : LINK_ERR_REMOTE);
		break;
	case LINK_MSG_TX_BUFFER:
		if (error != 0)
			mmal_buffer_failed(base, request->context);
		break;
	case LINK_BULK_TX:
		if (error != 0)
			Kprintf("[videocore] a bulk transmit failed: %ld\n", (LONG)error);
		break;
	case LINK_BULK_RX:
		mmal_rx_done(base, request->context, error == 0 ? (s32)request->io.iovq_Req.io_Actual : -1);
		break;
	case LINK_SMEM_TX:
		if (error != 0)
			smem_send_failed(base, (u32)request->context);
		break;
	default:
		break;
	}
}

/**
 * request_done() - Handle a request that came back from the device.
 * @base:    the library base, mmal_lock held
 * @request: the request
 *
 * A read is posted again while the link takes requests (an SMEM read while
 * SMEM is up); every other request goes back to the pool.  A service the VPU
 * closed is lost on its own: SMEM gone leaves mmal running, unimported.
 */
static void request_done(struct VideoCoreBase *base, struct link_request *request)
{
	BYTE error = request->io.iovq_Req.io_Error;
	BOOL smem = request->kind == LINK_SMEM_RX || request->kind == LINK_SMEM_TX;

	base->link.outstanding--;
	if (error == VQERR_CLOSED || error == VQERR_NOCONNECTION)
	{
		if (smem)
			smem_lost(base);
		else
			service_lost(base, error);
	}
	if (request->kind == LINK_MSG_RX)
	{
		message_received(base, request, error);
		if (link_ready(&base->link))
		{
			post_read(base, request, LINK_MSG_RX);
			return;
		}
	}
	else if (request->kind == LINK_SMEM_RX)
	{
		if (error == 0)
			smem_message(base, request->msg, request->io.iovq_Req.io_Actual);
		if (link_ready(&base->link) && base->smem.up)
		{
			post_read(base, request, LINK_SMEM_RX);
			return;
		}
	}
	else
		transfer_done(base, request, error);
	request_put(base, request);
}

/**
 * receive_task() - The task every request replies to.
 * @base:   the library base
 * @parent: the spawning task, told when the port is ready
 *
 * Handles each batch of returned requests under mmal_lock, sends bulk
 * receives that were parked for lack of a request, and wakes every caller
 * waiting in link_wait().  Stops on CTRL_C, which drv_task_join() sends once
 * link_settle() finds every request home.
 */
static void receive_task(struct VideoCoreBase *base, struct Task *parent)
{
	struct ExecBase *SysBase = base->SysBase;
	struct link *link = &base->link;

	link->port = CreateMsgPort();
	if (link->port == NULL)
	{
		drv_task_exit(SysBase, &link->task, parent, FALSE);
		return;
	}
	link->task = FindTask(NULL);
	Signal(parent, SIGBREAKF_CTRL_F);

	ULONG port_mask = 1UL << link->port->mp_SigBit;
	for (;;)
	{
		ULONG signals = Wait(port_mask | SIGBREAKF_CTRL_C);
		if (signals & SIGBREAKF_CTRL_C)
			break;
		ObtainSemaphore(&base->mmal_lock);
		struct Message *message;
		while ((message = GetMsg(link->port)) != NULL)
			request_done(base, (struct link_request *)message);
		mmal_pump(base);
		drv_wake_up_all(&base->waitq, SysBase);
		ReleaseSemaphore(&base->mmal_lock);
	}
	DeleteMsgPort(link->port);
	link->port = NULL;
	/* the spawning task may be long gone: report to ourselves, the join polls the slot */
	drv_task_exit(SysBase, &link->task, FindTask(NULL), TRUE);
}

/* ---- open and close (callers) ----------------------------------------------------- */

/**
 * stop_task() - Join the receive task and free the pool.
 * @base: the library base, mmal_lock held on entry and on return
 *
 * Every request must be home.  mmal_lock is released for the join: the task
 * may be on its way to mmal_lock with a wake-up left over, and has to get
 * through to see the CTRL_C.
 */
static void stop_task(struct VideoCoreBase *base)
{
	struct ExecBase *SysBase = base->SysBase;
	struct link *link = &base->link;
	ReleaseSemaphore(&base->mmal_lock);
	drv_task_join(SysBase, &link->task);
	ObtainSemaphore(&base->mmal_lock);
	if (link->requests != NULL)
		FreeMem(link->requests, LINK_REQUESTS * sizeof(struct link_request));
	link->requests = NULL;
	_NewMinList(&link->free);
}

/**
 * link_settle() - Whether the link is fully down, joining a receive task left behind.
 * @base: the library base, mmal_lock held
 *
 * A close whose requests did not all come home leaves the receive task
 * running for them; once they are home, it is joined here.
 *
 * Return: TRUE if the link is closed and no receive task is left.
 */
BOOL link_settle(struct VideoCoreBase *base)
{
	struct link *link = &base->link;
	if (link->state != LINK_CLOSED)
		return FALSE;
	if (link->task != NULL && link->outstanding == 0)
		stop_task(base);
	return link->task == NULL;
}

/**
 * service_request_init() - Set up the request that opens a service of vchiq.device.
 * @io:      the request
 * @port:    the reply port
 * @fourcc:  the service, 0 for an information-only unit
 * @version: the version offered
 * @minimum: the oldest version taken
 */
static void service_request_init(struct IOVchiqReq *io, struct MsgPort *port, u32 fourcc, UWORD version, UWORD minimum)
{
	memset(io, 0, sizeof(*io));
	io->iovq_Req.io_Message.mn_Node.ln_Type = NT_REPLYMSG;
	io->iovq_Req.io_Message.mn_ReplyPort = port;
	io->iovq_Req.io_Message.mn_Length = sizeof(*io);
	io->iovq_FourCC = fourcc;
	io->iovq_Version = version;
	io->iovq_VersionMin = minimum;
}

/**
 * smem_unit_open() - Open the SMEM service beside mmal, if the VPU has it.
 * @base: the library base, mmal_lock held, the link open
 *
 * Its requests are the pool's, sent to its unit (request_send()).  Without
 * it nothing is imported, which only costs the VPU its copies.
 */
static void smem_unit_open(struct VideoCoreBase *base)
{
	struct ExecBase *SysBase = base->SysBase;
	struct link *link = &base->link;

	service_request_init(&link->smem, link->port, SMEM_SERVICE_FOURCC, SMEM_SERVICE_VERSION, SMEM_SERVICE_VERSION_MIN);
	if (OpenDevice((CONST_STRPTR)VCHIQ_NAME, 0, (struct IORequest *)&link->smem, 0) != 0)
	{
		Kprintf("[videocore] %s: the SMEM service did not open (%ld): nothing is imported\n", VCHIQ_NAME,
		        (LONG)link->smem.iovq_Req.io_Error);
		return;
	}
	link->smem_open = TRUE;
	smem_up(base);
	for (u32 i = 0; i < LINK_SMEM_READS; i++)
	{
		struct link_request *read = request_get(base, FALSE);
		if (read != NULL)
			post_read(base, read, LINK_SMEM_RX);
	}
}

/**
 * pool_make() - Allocate the request pool and start the receive task that owns its reply port.
 * @base: the library base, mmal_lock held, the link closed and settled
 *
 * Return: TRUE with both up; FALSE with neither.
 */
static BOOL pool_make(struct VideoCoreBase *base)
{
	struct ExecBase *SysBase = base->SysBase;
	struct link *link = &base->link;

	link->outstanding = 0;
	_NewMinList(&link->free);
	link->requests = AllocMem(LINK_REQUESTS * sizeof(struct link_request), MEMF_PUBLIC | MEMF_CLEAR);
	if (link->requests == NULL)
		return FALSE;
	if (drv_task_spawn(SysBase, base, (APTR)receive_task, LIBRARY_NAME, LINK_TASK_STACK, LINK_TASK_PRIORITY) != 0)
	{
		FreeMem(link->requests, LINK_REQUESTS * sizeof(struct link_request));
		link->requests = NULL;
		return FALSE;
	}
	return TRUE;
}

/**
 * pool_fill() - Every request a copy of the opened service's, on the free list; the reads posted.
 * @base: the library base, mmal_lock held, the mmal service open
 *
 * A request's device, unit and reply port never change; the sends fill in the rest.
 */
static void pool_fill(struct VideoCoreBase *base)
{
	struct ExecBase *SysBase = base->SysBase;
	struct link *link = &base->link;
	for (u32 i = 0; i < LINK_REQUESTS; i++)
	{
		CopyMem(&link->mmal, &link->requests[i].io, sizeof(link->mmal));
		request_put(base, &link->requests[i]);
	}
	for (u32 i = 0; i < LINK_READS; i++)
		post_read(base, request_get(base, FALSE), LINK_MSG_RX);
}

/**
 * link_open() - Open the "mmal" service and start the receive task.
 * @base: the library base, mmal_lock held
 *
 * SMEM opens after it (smem_unit_open()), if the VPU has it.
 *
 * Return: 0, or LINK_ERR_STATE if the link is open already but dead or
 * closing, a previous close's requests are still out, or the pool, the task
 * or the service did not come up (logged).
 */
s32 link_open(struct VideoCoreBase *base)
{
	struct ExecBase *SysBase = base->SysBase;
	struct link *link = &base->link;

	if (link->state != LINK_CLOSED)
		return link_ready(link) ? 0 : LINK_ERR_STATE;
	if (!link_settle(base) || !pool_make(base))
		return LINK_ERR_STATE;

	service_request_init(&link->mmal, link->port, MMAL_SERVICE_FOURCC, MMAL_SERVICE_VERSION, MMAL_SERVICE_VERSION_MIN);
	if (OpenDevice((CONST_STRPTR)VCHIQ_NAME, 0, (struct IORequest *)&link->mmal, 0) != 0)
	{
		Kprintf("[videocore] %s: the mmal service did not open (%ld)\n", VCHIQ_NAME, (LONG)link->mmal.iovq_Req.io_Error);
		stop_task(base);
		return LINK_ERR_STATE;
	}
	link->state = LINK_READY;
	pool_fill(base);
	smem_unit_open(base);
	return 0;
}

/**
 * link_close() - Close the services and stop the receive task.
 * @base: the library base, mmal_lock held (released while waiting)
 *
 * Aborts the posted reads and waits for every request; the device's close
 * fails what is still queued and the service's close aborts the bulk
 * transfers the VPU holds, so a second wait collects those.  If some still
 * do not come back, the receive task stays for them and link_settle() joins
 * it later.  SMEM closes first: every port, and so every import, is gone
 * by now.  Closing a closed link does nothing.
 */
void link_close(struct VideoCoreBase *base)
{
	struct ExecBase *SysBase = base->SysBase;
	struct link *link = &base->link;

	if (link->state == LINK_CLOSED)
		return;
	link->state = LINK_CLOSING;
	if (link->smem_open)
		smem_closing(base);
	for (u32 i = 0; i < LINK_REQUESTS; i++)
		if (link->requests[i].at_device && (link->requests[i].kind == LINK_MSG_RX || link->requests[i].kind == LINK_SMEM_RX))
			AbortIO((struct IORequest *)&link->requests[i].io);
	if (link_wait(base, requests_home, NULL, LINK_WAIT_MS) != 0)
		Kprintf("[videocore] %ld requests still at %s at the close\n", link->outstanding, VCHIQ_NAME);
	if (link->smem_open)
		CloseDevice((struct IORequest *)&link->smem);
	link->smem_open = FALSE;
	CloseDevice((struct IORequest *)&link->mmal);
	link->state = LINK_CLOSED;
	if (link->outstanding != 0 && link_wait(base, requests_home, NULL, LINK_WAIT_MS) != 0)
		Kprintf("[videocore] %ld requests never came back: the receive task stays for them\n", link->outstanding);
	(void)link_settle(base);
}

/**
 * link_reach() - Learn from vchiq.device where the VPU reaches host memory.
 * @base: the library base, from a task
 *
 * An information-only unit (no service) answers NSCMD_VQ_GETINFO; the
 * answer goes to the MMAL client (reach_set()).
 */
void link_reach(struct VideoCoreBase *base)
{
	struct ExecBase *SysBase = base->SysBase;
	struct MsgPort *port = CreateMsgPort();
	if (port == NULL)
		return;
	struct IOVchiqReq io;
	service_request_init(&io, port, 0, 0, 0);
	if (OpenDevice((CONST_STRPTR)VCHIQ_NAME, 0, (struct IORequest *)&io, 0) == 0)
	{
		struct VchiqInfo info;
		memset(&info, 0, sizeof(info));
		io.iovq_Req.io_Command = NSCMD_VQ_GETINFO;
		io.iovq_Req.io_Data = &info;
		io.iovq_Req.io_Length = sizeof(info);
		if (DoIO((struct IORequest *)&io) == 0)
			reach_set(base, &info);
		CloseDevice((struct IORequest *)&io);
	}
	DeleteMsgPort(port);
}
