/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * videocore_link.h - the link to vchiq.device (videocore_link.c).
 *
 * The link is the opened "mmal" service of vchiq.device, beside it the
 * "SMEM" service when the VPU has it (imports, smem_client.c), a fixed pool
 * of requests for both, and the receive task that owns the requests' reply
 * port.
 *
 * - Every request is a copy of the opened service's request, replying to the
 *   receive task's port: whatever comes back (an incoming message, a finished
 *   bulk transfer, a failed write) is handled by that task, under mmal_lock.
 * - Callers only SendIO().  A message and the bulk transfer that belongs to it
 *   are sent back to back without mmal_lock being released, and vchiq.device
 *   keeps one service's writes and bulk transmits in issue order, so the pair
 *   stays together.
 * - Callers may wait for a free request (with mmal_lock released); the
 *   receive task never waits.
 * - The link opens with the first component or image call and closes at
 *   VCB_Idle() (mmal_client.c).  SMEM opens and closes with it; without it
 *   the link works, with nothing imported.  A close whose requests do not
 *   all come home leaves the receive task running until they do;
 *   link_settle() joins it then.
 *
 * Everything here runs under mmal_lock unless said otherwise.
 */
#ifndef VIDEOCORE_LINK_H
#define VIDEOCORE_LINK_H

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/tasks.h>
#include <exec/ports.h>

#include <types.h>
#include <drv_waitq.h>
#include <devices/vchiq.h>

struct VideoCoreBase;
struct mmal_buffer;

#define LINK_TASK_PRIORITY 20   /* below vchiq.device's task, above applications */
#define LINK_TASK_STACK 8192
#define LINK_REQUESTS 64        /* reads, writes and bulk transfers at the device at once */
#define LINK_READS 4            /* reads kept posted for the VPU's messages */
#define LINK_SMEM_READS 2       /* ... on the SMEM service: import answers and releases */
#define LINK_MESSAGE_BYTES 512  /* an MMAL message (userland's MMAL_WORKER_MAX_MSG_LEN) */
#define LINK_WAIT_MS 3000       /* the longest a caller waits for the VPU: a free request, a reply, the requests' return */

/*
 * The link's own failures, and the clients' before or around the VPU (the
 * link's calls return 0 or one of these; the MMAL client's public calls turn
 * them into a VCERR_*, vcerr_from_status())
 */
#define LINK_ERR_TIMEOUT -1 /* the VPU did not answer */
#define LINK_ERR_STATE -2   /* no connection (vchiq.device missing or down), or the VPU closed the service */
#define LINK_ERR_NOSPACE -3 /* too many calls in flight, or no request free */
#define LINK_ERR_PARAM -4   /* a message too long for a request */
#define LINK_ERR_REMOTE -5  /* the VPU's answer made no sense, or a write of ours failed */

/*
 * What a request carries, which way, and where its completion goes.  A
 * message (MSG) travels in the shared slots, a payload (BULK) by DMA; RX is
 * from the VPU, TX to it.
 */
enum link_kind
{
	LINK_MSG_RX = 1,    /* a message from the VPU (CMD_READ): to mmal_message(), then posted again */
	LINK_MSG_TX_CALL,   /* a control message to the VPU (CMD_WRITE): if it fails, mmal_call_failed() */
	LINK_MSG_TX_BUFFER, /* a message to the VPU handing it a buffer (CMD_WRITE): if it fails, mmal_buffer_failed() */
	LINK_BULK_TX,       /* a payload to the VPU after its message (NSCMD_VQ_BULKWRITE): if it fails, logged */
	LINK_BULK_RX,       /* a payload from the VPU into one of our buffers (NSCMD_VQ_BULKREAD): to mmal_rx_done() */
	LINK_SMEM_RX,       /* a message from the VPU on SMEM (CMD_READ): to smem_message(), then posted again */
	LINK_SMEM_TX,       /* a message to the VPU on SMEM (CMD_WRITE): if it fails, smem_send_failed() */
};

/* Where the link is */
enum link_state
{
	LINK_CLOSED = 0, /* no service open (a cleared base) */
	LINK_READY,      /* the service is open and the VPU answers: requests go */
	LINK_CLOSING,    /* link_close() runs: nothing new is sent, no read is posted again */
	LINK_DEAD,       /* the VPU closed the service, or stopped answering: every call fails at once until the close */
};

struct link_request
{
	struct IOVchiqReq io; /* first: the request is the message that comes back */
	struct MinNode node;  /* on the link's free list while idle */
	enum link_kind kind;
	BOOL at_device; /* sent and not yet back: link_close() aborts the reads among these */
	/*
	 * What the completion is about, saved at the send and handed to the MMAL
	 * client when the request comes back; the link never looks inside:
	 *   LINK_MSG_TX_CALL    the call's cookie (mmal_call.c), as a number
	 *   LINK_MSG_TX_BUFFER  the struct mmal_buffer the message carries
	 *   LINK_BULK_RX        the struct mmal_buffer the payload goes into
	 *   LINK_SMEM_TX        the import's cookie (smem_client.c), as a number
	 *   LINK_MSG_RX, LINK_BULK_TX, LINK_SMEM_RX  NULL
	 * link_disown() clears a buffer that is let go while its request is out.
	 */
	APTR context;
	u8 msg[LINK_MESSAGE_BYTES]; /* a write's message, a read's buffer */
};

struct link
{
	struct IOVchiqReq mmal;        /* the opened "mmal" service: every request is a copy of it */
	struct IOVchiqReq smem;        /* the "SMEM" service, while smem_open: its requests take its unit */
	BOOL smem_open;                /* the SMEM unit is open (the SMEM client's own state is base->smem.up) */
	struct Task *task;             /* the receive task, NULL when there is none */
	struct MsgPort *port;          /* its port: every request replies here */
	struct link_request *requests; /* the pool, LINK_REQUESTS of them */
	struct MinList free;           /* the idle ones */
	u32 outstanding;               /* requests at the device */
	enum link_state state;
};

/**
 * link_ready() - Whether the link takes new requests.
 * @link: the link
 *
 * Return: TRUE if the service is open, alive and not closing.
 */
static inline BOOL link_ready(const struct link *link)
{
	return link->state == LINK_READY;
}

/* Open the "mmal" service and start the receive task; SMEM beside it if the VPU has it */
s32 link_open(struct VideoCoreBase *base);
/* Close the services and stop the receive task (it stays if requests never came back) */
void link_close(struct VideoCoreBase *base);
/* Whether the link is fully down, joining a receive task a close left behind */
BOOL link_settle(struct VideoCoreBase *base);
/* Send a control message whose reply a call waits for, with a payload or none */
s32 link_send_call(struct VideoCoreBase *base, const void *msg, u32 size, u32 cookie, APTR bulk, u32 bulk_size);
/* Send the message that hands the VPU a buffer, with its payload or none */
s32 link_send_buffer(struct VideoCoreBase *base, const void *msg, u32 size, struct mmal_buffer *buffer, APTR bulk,
                     u32 bulk_size);
/* Receive a payload the VPU announced; never waits */
s32 link_bulk_receive(struct VideoCoreBase *base, APTR data, u32 size, struct mmal_buffer *buffer);
/* Send a message on the SMEM service; @wait for a free request, or not (the receive task, a lock-held walk) */
s32 link_send_smem(struct VideoCoreBase *base, const void *msg, u32 size, u32 cookie, BOOL wait);
/* Ask vchiq.device where the VPU reaches host memory (reach_set()); from a task, no lock needed */
void link_reach(struct VideoCoreBase *base);
/* Make the requests at the device that carry a buffer name none: the buffer is let go */
void link_disown(struct VideoCoreBase *base, struct mmal_buffer *buffer);
/* Take the service as gone: nothing new is sent, both clients hear of it */
void link_dead(struct VideoCoreBase *base);
/* Wait under mmal_lock (released meanwhile) until a condition holds, @timeout_ms at most */
s32 link_wait(struct VideoCoreBase *base, drv_wait_pred_t pred, APTR arg, ULONG timeout_ms);

#endif /* VIDEOCORE_LINK_H */
