/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * waiter.c - what a tool waits with: its task, a signal and a timer.  Hosted
 * (libnix): SysBase is libnix's.
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#include <proto/exec.h>
#endif

#include <dos/dos.h>

#include "waiter.h"

/**
 * waiter_open() - Take the task, a signal and a MICROHZ timer.
 * @w: the waiter, zeroed
 *
 * Return: TRUE when all three are there; FALSE with nothing held.
 */
BOOL waiter_open(struct waiter *w)
{
	w->task = FindTask(NULL);
	w->sig = AllocSignal(-1);
	if (w->sig != -1 && drv_timer_open(&w->timer, SysBase))
		return TRUE;
	waiter_close(w);
	return FALSE;
}

/**
 * waiter_close() - Give the signal and the timer back.
 * @w: the waiter, open or half open
 */
void waiter_close(struct waiter *w)
{
	if (w->timer.req != NULL)
		drv_timer_close(&w->timer);
	if (w->sig != -1)
		FreeSignal(w->sig);
	w->sig = -1;
}

/**
 * waiter_wait() - Sleep until the waiter's signal, one of @extra, Ctrl-C or the timeout.
 * @w:          the waiter, open
 * @extra:      more signals to wake for (a window's port), or 0
 * @timeout_ms: how long at most; 0 is one millisecond
 *
 * Return: the signals seen; 0 on the timeout.
 */
ULONG waiter_wait(struct waiter *w, ULONG extra, ULONG timeout_ms)
{
	return drv_timer_wait(&w->timer, waiter_sigmask(w) | extra | SIGBREAKF_CTRL_C, timeout_ms);
}
