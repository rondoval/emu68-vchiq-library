/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * vchiq_service.c - services: the table and its port records, open and close (asynchronous
 * and blocking), and the messages a service queues, takes and releases.
 */
#include <vchiq/vchiq_core_priv.h>

/* ---- the table ----------------------------------------------------------- */

/* The service on our port @port; NULL if none or FREE. */
struct vchiq_service *vq_find_service_by_port(struct vchiq_ctx *ctx, u32 port)
{
	if (port >= VCHIQ_MAX_LOCAL_SERVICES)
		return NULL;
	struct vchiq_service *svc = &ctx->services[port];
	return svc->state == VCHIQ_SVC_FREE ? NULL : svc;
}

/* The service a message from the VPU's port @src to our port @dst is for; NULL if none. */
struct vchiq_service *vq_find_peer_service(struct vchiq_ctx *ctx, u32 dst, u32 src)
{
	struct vchiq_service *svc = vq_find_service_by_port(ctx, dst);
	return svc != NULL && svc->remoteport == src ? svc : NULL;
}

/* A FREE entry of the table; NULL if every port is taken. */
static struct vchiq_service *find_free_service(struct vchiq_ctx *ctx)
{
	for (u32 i = 0; i < VCHIQ_MAX_LOCAL_SERVICES; i++)
		if (ctx->services[i].state == VCHIQ_SVC_FREE)
			return &ctx->services[i];
	return NULL;
}

/* An empty FREE service on @port. */
static void reset_service(struct vchiq_service *svc, u32 port)
{
	memset(svc, 0, sizeof(*svc));
	svc->localport = (u16)port;
}

/* Every service FREE and empty (a new session). */
void vq_reset_services(struct vchiq_ctx *ctx)
{
	for (u32 i = 0; i < VCHIQ_MAX_LOCAL_SERVICES; i++)
		reset_service(&ctx->services[i], i);
}

/* Record @svc's port in the window as open (its fourcc, the VPU's port) or clear it: what a repair closes. */
void vq_record_port(struct vchiq_ctx *ctx, const struct vchiq_service *svc, BOOL open)
{
	ctx->host->port_fourcc[svc->localport] = open ? svc->fourcc : 0u;
	ctx->host->port_remote[svc->localport] = open ? svc->remoteport : 0u;
}

/* The service's callback, if it has one and the session is not quiet. */
void vq_make_service_callback(struct vchiq_ctx *ctx, struct vchiq_service *svc, u32 reason, const struct vchiq_bulk *bulk,
                              s32 bulk_actual)
{
	if (svc->callback != NULL && !ctx->quiet)
		svc->callback(ctx, svc, reason, bulk, bulk_actual);
}

/* ---- open and close ------------------------------------------------------ */

/* Every held message released unread. */
static void release_service_messages(struct vchiq_ctx *ctx, struct vchiq_service *svc)
{
	volatile struct vchiq_header *hdr;
	while ((hdr = vchiq_take_message(svc)) != NULL)
	{
		vchiq_release_message(ctx, hdr);
		ctx->stats.dropped_msgs++;
	}
}

/*
 * The VPU accepted our OPEN (an OPENACK from port @src), on its sync slot if
 * @sync.  A service abandoned mid-open only learns the VPU's port: the CLOSE
 * it already sent closes that, and the VPU answers it.
 */
void vq_set_service_open(struct vchiq_ctx *ctx, struct vchiq_service *svc, u32 src, volatile struct vchiq_header *hdr,
                         u32 size, BOOL sync)
{
	svc->remoteport = (u16)src;
	vq_record_port(ctx, svc, TRUE);
	if (svc->state == VCHIQ_SVC_CLOSING)
		return;
	const volatile struct vchiq_openack_payload *ack = (const volatile struct vchiq_openack_payload *)hdr->data;
	svc->peer_version = size >= sizeof(*ack) ? (s16)le16(ack->version) : 0; /* 0: the OPENACK carried none */
	svc->sync = (u8)sync;
	svc->state = VCHIQ_SVC_OPEN;
}

/* The service is gone on both sides: its messages dropped, its bulks reported aborted. */
void vq_close_service_complete(struct vchiq_ctx *ctx, struct vchiq_service *svc)
{
	release_service_messages(ctx, svc);
	for (u32 dir = 0; dir < 2; dir++)
	{
		struct vchiq_bulk *bulk;
		while ((bulk = vq_pop_bulk(svc, dir)) != NULL)
			vq_finish_bulk(ctx, svc, dir, bulk, VCHIQ_BULK_ACTUAL_ABORTED);
	}
	vq_record_port(ctx, svc, FALSE);
	svc->state = VCHIQ_SVC_CLOSED;
}

/* Take a FREE entry and queue OPEN for service @fourcc: OPENING until a poll brings the VPU's answer. */
s32 vchiq_queue_service_open(struct vchiq_ctx *ctx, u32 fourcc, s16 version, s16 version_min,
                             vchiq_callback_t cb, APTR userdata, struct vchiq_service **out)
{
	if (ctx->conn_state != VCHIQ_CONN_CONNECTED)
		return VQERR_NOCONNECTION;
	struct vchiq_service *svc = find_free_service(ctx);
	if (svc == NULL)
		return IOERR_UNITBUSY;

	reset_service(svc, svc->localport);
	svc->fourcc = fourcc;
	svc->remoteport = VCHIQ_PORT_FREE; /* until the OPENACK */
	svc->callback = cb;
	svc->userdata = userdata;

	struct vchiq_open_payload op = {
	    .fourcc = le32(fourcc),
	    .client_id = le32(1),
	    .version = le16((u16)version),
	    .version_min = le16((u16)version_min),
	};
	s32 rc = vq_queue_message(ctx, VCHIQ_MAKE_MSG(VCHIQ_MSG_OPEN, svc->localport, 0), &op, sizeof(op), 0, FALSE);
	if (rc != 0)
		return rc; /* the entry stays FREE */
	svc->state = VCHIQ_SVC_OPENING;
	vq_record_port(ctx, svc, TRUE); /* the VPU may answer after this session: the repair closes the port */
	*out = svc;
	return 0;
}

/* Queue CLOSE for an OPEN or OPENING service: CLOSING until a poll brings the VPU's answer. */
s32 vchiq_queue_service_close(struct vchiq_ctx *ctx, struct vchiq_service *svc)
{
	switch (svc->state)
	{
	case VCHIQ_SVC_OPENING:
		/*
		 * Abandoned mid-open: the port stays taken until the VPU answers, or a
		 * late OPENACK would open whatever service takes it next.  No remote
		 * port yet: the CLOSE goes to port 0, where the VPU looks the service
		 * up by ours.
		 */
	case VCHIQ_SVC_OPEN:
	{
		u32 dst = svc->remoteport == VCHIQ_PORT_FREE ? 0u : svc->remoteport;
		s32 rc = vq_queue_message(ctx, VCHIQ_MAKE_MSG(VCHIQ_MSG_CLOSE, svc->localport, dst), NULL, 0, 0, FALSE);
		if (rc != 0)
			return rc; /* nothing changed */
		release_service_messages(ctx, svc);
		svc->state = VCHIQ_SVC_CLOSING;
		return 0;
	}
	case VCHIQ_SVC_CLOSING:
	case VCHIQ_SVC_CLOSED:
		return 0; /* already on its way, or there */
	default:
		return VQERR_CLOSED; /* FREE */
	}
}

/* A CLOSED service back to the table. */
void vchiq_free_service(struct vchiq_service *svc)
{
	if (svc->state == VCHIQ_SVC_CLOSED)
		svc->state = VCHIQ_SVC_FREE;
}

/* vq_poll_until() predicate: the VPU answered the OPEN (accepted or refused). */
static BOOL is_open_answered(const struct vchiq_ctx *ctx, const void *arg)
{
	(void)ctx;
	return ((const struct vchiq_service *)arg)->state != VCHIQ_SVC_OPENING;
}

/* Open service @fourcc and poll for the answer (the probe's way); closed again on failure. */
s32 vchiq_open_service(struct vchiq_ctx *ctx, u32 fourcc, s16 version, s16 version_min,
                       struct vchiq_service **out, u32 timeout_us)
{
	struct vchiq_service *svc;
	s32 rc = vchiq_queue_service_open(ctx, fourcc, version, version_min, NULL, NULL, &svc);
	if (rc != 0)
		return rc;
	rc = vq_poll_until(ctx, is_open_answered, svc, timeout_us);
	if (rc == 0 && svc->state != VCHIQ_SVC_OPEN)
		rc = VQERR_REJECTED;
	if (rc != 0)
	{
		(void)vchiq_close_service(ctx, svc, timeout_us); /* refused: freed; unanswered: abandoned */
		return rc;
	}
	*out = svc;
	return 0;
}

/* vq_poll_until() predicate: the VPU answered the CLOSE. */
static BOOL is_close_answered(const struct vchiq_ctx *ctx, const void *arg)
{
	(void)ctx;
	return ((const struct vchiq_service *)arg)->state != VCHIQ_SVC_CLOSING;
}

/* Close a service, poll for the VPU's answer, free it. */
s32 vchiq_close_service(struct vchiq_ctx *ctx, struct vchiq_service *svc, u32 timeout_us)
{
	s32 rc = vchiq_queue_service_close(ctx, svc);
	if (rc == 0)
		rc = vq_poll_until(ctx, is_close_answered, svc, timeout_us); /* a timeout leaves it CLOSING: not clean */
	if (rc == 0)
		vchiq_free_service(svc);
	return rc;
}

/* ---- messages ------------------------------------------------------------ */

/* Queue a DATA message on an OPEN service, through the sync slots if the VPU made it synchronous. */
s32 vchiq_queue_message(struct vchiq_ctx *ctx, struct vchiq_service *svc, const void *data, u32 size, u32 timeout_us,
                        BOOL poll)
{
	if (svc->state != VCHIQ_SVC_OPEN)
		return VQERR_CLOSED;
	u32 msgid = VCHIQ_MAKE_MSG(VCHIQ_MSG_DATA, svc->localport, svc->remoteport); /* our port to the VPU's */
	if (svc->sync)
		return vq_queue_message_sync(ctx, msgid, data, size, timeout_us, poll); /* the VPU chose the sync slots at OPENACK */
	return vq_queue_message(ctx, msgid, data, size, timeout_us, poll);
}

/* The service's next held message, oldest first; NULL if none. */
volatile struct vchiq_header *vchiq_take_message(struct vchiq_service *svc)
{
	if (svc->rxq_head == svc->rxq_tail)
		return NULL;
	volatile struct vchiq_header *hdr = svc->rxq[svc->rxq_head % VCHIQ_RXQ_SIZE];
	svc->rxq_head++;
	return hdr;
}

/* vq_poll_until() predicate: a message to take, or no more will come. */
static BOOL has_message_or_closed(const struct vchiq_ctx *ctx, const void *arg)
{
	(void)ctx;
	const struct vchiq_service *svc = arg;
	return svc->rxq_head != svc->rxq_tail || svc->state != VCHIQ_SVC_OPEN;
}

/* vchiq_take_message(), polling up to @timeout_us for a message. */
volatile struct vchiq_header *vchiq_dequeue_message(struct vchiq_ctx *ctx, struct vchiq_service *svc, u32 timeout_us)
{
	return vq_poll_until(ctx, has_message_or_closed, svc, timeout_us) == 0 ? vchiq_take_message(svc) : NULL;
}

/* A taken message released: a VPU data slot goes back once all its claims are, the VPU's sync slot at once. */
void vchiq_release_message(struct vchiq_ctx *ctx, volatile struct vchiq_header *hdr)
{
	u32 slot = slot_index_of(ctx, hdr);
	if (slot >= le32(ctx->shared_vpu->slot_first) && slot <= le32(ctx->shared_vpu->slot_last))
		vq_release_slot(ctx, slot, hdr);
	else if (slot == le32(ctx->shared_vpu->slot_sync))
		vq_release_message_sync(ctx);
}
