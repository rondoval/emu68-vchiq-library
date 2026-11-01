/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * vchiq_client.c - a VCHIQ service from a CLI tool (VCInfo, VCTest): open,
 * close, writes and bulks, the next message, GETINFO.
 *
 * Hosted (libnix): SysBase and DOSBase are libnix's.
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/dos_protos.h>
#else
#include <proto/exec.h>
#include <proto/dos.h>
#endif

#include <exec/errors.h>
#include <dos/dos.h>

#include "vchiq_client.h"

/**
 * clone() - A copy of the opened request: the same device, unit and port.
 * @s: the service, opened
 *
 * Return: the request, NULL if out of memory.
 */
static struct IOVchiqReq *clone(struct vq_service *s)
{
	struct IOVchiqReq *r = (struct IOVchiqReq *)CreateIORequest(s->port, sizeof(struct IOVchiqReq));
	if (r != NULL)
	{
		r->iovq_Req.io_Device = s->io->iovq_Req.io_Device;
		r->iovq_Req.io_Unit = s->io->iovq_Req.io_Unit;
	}
	return r;
}

/**
 * post_read() - Send one of the service's reads.
 * @s: the service
 * @k: which read
 */
static void post_read(struct vq_service *s, ULONG k)
{
	s->rd[k]->iovq_Req.io_Command = CMD_READ;
	s->rd[k]->iovq_Req.io_Data = s->buf[k];
	s->rd[k]->iovq_Req.io_Length = VQ_MSG_BYTES;
	SendIO((struct IORequest *)s->rd[k]);
}

/**
 * vq_close() - Close a service: its reads aborted and collected, the device closed.
 * @s: the service, open, half open or closed
 */
void vq_close(struct vq_service *s)
{
	for (ULONG k = 0; k < VQ_READS; k++)
		if (s->rd[k] != NULL)
		{
			if (!CheckIO((struct IORequest *)s->rd[k]))
				AbortIO((struct IORequest *)s->rd[k]);
			WaitIO((struct IORequest *)s->rd[k]);
			DeleteIORequest((struct IORequest *)s->rd[k]);
			s->rd[k] = NULL;
		}
	if (s->wr != NULL)
		DeleteIORequest((struct IORequest *)s->wr);
	s->wr = NULL;
	if (s->io != NULL)
	{
		if (s->io->iovq_Req.io_Device != NULL)
			CloseDevice((struct IORequest *)s->io);
		DeleteIORequest((struct IORequest *)s->io);
		s->io = NULL;
	}
	if (s->port != NULL)
		DeleteMsgPort(s->port);
	s->port = NULL;
}

/**
 * vq_open() - Open a service of vchiq.device, its reads posted.
 * @s:           the service, zeroed
 * @timer:       the caller's timer, for the waits
 * @fourcc:      the service; 0 opens an information-only unit (no service, no reads)
 * @version:     the version we speak
 * @version_min: the oldest we accept
 * @what:        its name, for the messages; NULL: no messages (the cause is in @s->error)
 *
 * Return: TRUE if open; FALSE with the cause in @s->error (and printed, given @what).
 */
BOOL vq_open(struct vq_service *s, struct drv_timer *timer, ULONG fourcc, UWORD version, UWORD version_min,
             const char *what)
{
	s->timer = timer;
	s->next = 0;
	s->error = -1;
	s->port = CreateMsgPort();
	s->io = s->port != NULL ? (struct IOVchiqReq *)CreateIORequest(s->port, sizeof(struct IOVchiqReq)) : NULL;
	if (s->io == NULL)
	{
		vq_close(s);
		return FALSE;
	}
	s->io->iovq_FourCC = fourcc;
	s->io->iovq_Version = version;
	s->io->iovq_VersionMin = version_min;
	if (OpenDevice((CONST_STRPTR)VCHIQ_NAME, 0, (struct IORequest *)s->io, 0) != 0)
	{
		s->error = s->io->iovq_Req.io_Error;
		if (what != NULL)
			Printf((CONST_STRPTR) "open %s: error %ld\n", what, s->error);
		s->io->iovq_Req.io_Device = NULL;
		vq_close(s);
		return FALSE;
	}
	s->error = 0;
	if (fourcc == 0)
		return TRUE;
	s->wr = clone(s);
	for (ULONG k = 0; k < VQ_READS; k++)
		s->rd[k] = clone(s);
	for (ULONG k = 0; k < VQ_READS; k++)
		if (s->rd[k] == NULL || s->wr == NULL)
		{
			s->error = -1;
			vq_close(s);
			return FALSE;
		}
	for (ULONG k = 0; k < VQ_READS; k++)
		post_read(s, k);
	if (what != NULL)
		Printf((CONST_STRPTR) "open %s: peer version %ld\n", what, (ULONG)s->io->iovq_PeerVersion);
	return TRUE;
}

/**
 * vq_do() - CMD_WRITE or a bulk transfer, waited for.
 * @s:       the service, open
 * @command: CMD_WRITE, NSCMD_VQ_BULKWRITE or NSCMD_VQ_BULKREAD
 * @data:    the bytes
 * @size:    their count
 * @actual:  where io_Actual goes, or NULL
 *
 * Return: the device's error, 0 when done.
 */
LONG vq_do(struct vq_service *s, UWORD command, CONST_APTR data, ULONG size, ULONG *actual)
{
	s->wr->iovq_Req.io_Command = command;
	s->wr->iovq_Req.io_Data = (APTR)data;
	s->wr->iovq_Req.io_Length = size;
	LONG err = DoIO((struct IORequest *)s->wr);
	if (actual != NULL)
		*actual = s->wr->iovq_Req.io_Actual;
	return err;
}

/**
 * vq_next() - The next message from the VPU, or the timeout, or Ctrl-C.
 * @s:          the service, open
 * @data:       where the message's bytes go (the read's buffer: valid until vq_consume())
 * @len:        where their count goes (at most VQ_MSG_BYTES)
 * @timeout_ms: how long to wait
 *
 * The timer request is always collected before returning, so it is never left
 * with timer.device when the port is deleted at exit.
 *
 * Return: TRUE with a message; FALSE on a read error (printed), the timeout or Ctrl-C.
 */
BOOL vq_next(struct vq_service *s, UBYTE **data, ULONG *len, ULONG timeout_ms)
{
	struct IOVchiqReq *rd = s->rd[s->next];
	ULONG tmask = drv_timer_sigmask(s->timer);
	drv_timer_arm_ms(s->timer, timeout_ms);

	BOOL ok = FALSE;
	for (;;)
	{
		if (CheckIO((struct IORequest *)rd))
		{
			WaitIO((struct IORequest *)rd);
			ok = rd->iovq_Req.io_Error == 0 || rd->iovq_Req.io_Error == VQERR_TRUNCATED;
			if (!ok)
				Printf((CONST_STRPTR) "  read: error %ld\n", (LONG)rd->iovq_Req.io_Error);
			break;
		}
		ULONG sig = Wait((1UL << s->port->mp_SigBit) | tmask | SIGBREAKF_CTRL_C);
		if (sig & SIGBREAKF_CTRL_C)
			break;
		if (sig & tmask)
		{
			if (CheckIO((struct IORequest *)rd))
				continue; /* the reply landed together with the timeout */
			break;
		}
	}
	drv_timer_cancel(s->timer); /* AbortIO of a completed request is harmless; WaitIO collects it */
	if (!ok)
		return FALSE;
	*data = s->buf[s->next];
	*len = rd->iovq_Req.io_Actual < VQ_MSG_BYTES ? rd->iovq_Req.io_Actual : VQ_MSG_BYTES;
	return TRUE;
}

/**
 * vq_consume() - Done with the message vq_next() gave: its read goes out again.
 * @s: the service, open
 */
void vq_consume(struct vq_service *s)
{
	post_read(s, s->next);
	s->next = (s->next + 1u) % VQ_READS;
}

/**
 * vq_get_info() - The device's state and counters (NSCMD_VQ_GETINFO).
 * @s:    the service or information-only unit, open
 * @info: where they go; fields the device does not know stay zero
 *
 * Return: TRUE if the device answered.
 */
BOOL vq_get_info(struct vq_service *s, struct VchiqInfo *info)
{
	for (ULONG i = 0; i < sizeof(*info); i++)
		((UBYTE *)info)[i] = 0;
	struct IOVchiqReq *io = s->io;
	io->iovq_Req.io_Command = NSCMD_VQ_GETINFO;
	io->iovq_Req.io_Data = info;
	io->iovq_Req.io_Length = sizeof(*info);
	return DoIO((struct IORequest *)io) == 0;
}
