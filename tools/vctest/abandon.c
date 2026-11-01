/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * abandon.c - ABANDON: leave the connection as a crash in mid-flight would,
 * for the next reset to keep (the repair test; needs a vchiq.device built
 * with VCHIQ_TEST_HOOKS).
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#include <proto/exec.h>
#endif

#include <dos/dos.h>
#include <exec/errors.h>
#include <exec/io.h>

#include <kv.h>
#include <vchiq/vchiq_test.h> /* NSCMD_VQ_TEST_ABANDON */

#include "commands.h"

const char abandon_template[] = "";

/**
 * do_abandon() - ABANDON: arm the test hook and leave a gencmd reply unread.
 * @t:    the tool
 * @args: none
 *
 * Opens the gencmd service and arms NSCMD_VQ_TEST_ABANDON: the next reset
 * leaves the session unquiesced.  The reads are taken back first, so the
 * reply to the one command sent then stays held, unread, and nothing is ever
 * replied to this task after it ends; the unit stays open, its port and
 * requests are left for the reset.  The first open after the reset repairs
 * the session.
 *
 * Return: RETURN_OK; RETURN_ERROR if vchiq.device did not load or has no test
 * hooks; RETURN_FAIL if the service did not open or the command did not go out.
 */
int do_abandon(struct tool *t, LONG *args)
{
	(void)args;
	struct vq_service *s = &t->svc;
	if (!vq_open(s, &t->waiter.timer, GCMD_FOURCC, 1, 1, NULL))
	{
		kv_num("vchiq.error", s->error);
		kv_str("fail", "the GCMD service did not open");
		return s->error == IOERR_OPENFAIL ? RETURN_ERROR : RETURN_FAIL;
	}
	s->io->iovq_Req.io_Command = NSCMD_VQ_TEST_ABANDON;
	s->io->iovq_Req.io_Data = NULL;
	s->io->iovq_Req.io_Length = 0;
	LONG err = DoIO((struct IORequest *)s->io);
	if (err != 0)
	{
		kv_bool("abandon.hooks", err != IOERR_NOCMD);
		kv_num("error", err);
		kv_str("fail", err == IOERR_NOCMD ? "this vchiq.device has no test hooks (EMU68_VCHIQ_TEST_HOOKS=ON)" : "the test hook refused");
		vq_close(s);
		return RETURN_ERROR;
	}
	kv_bool("abandon.hooks", TRUE);
	for (ULONG k = 0; k < VQ_READS; k++)
	{
		if (!CheckIO((struct IORequest *)s->rd[k]))
			AbortIO((struct IORequest *)s->rd[k]);
		WaitIO((struct IORequest *)s->rd[k]);
	}
	static const char cmd[] = "version";
	err = vq_do(s, CMD_WRITE, cmd, sizeof(cmd), NULL);
	if (err != 0)
	{
		kv_num("error", err);
		kv_str("fail", "the gencmd write failed; armed all the same");
		return RETURN_FAIL; /* the unit stays open for the reset */
	}
	kv_bool("abandon.armed", TRUE);
	note("GCMD left open, a reply unread; the next reset leaves the session as a crash would\n");
	return RETURN_OK;
}
