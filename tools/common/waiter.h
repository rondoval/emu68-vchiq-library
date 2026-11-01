/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * waiter.h - what a tool waits with: its task and signal, which a videocodec
 * session is told to use, and a timer.
 */
#ifndef TOOLS_WAITER_H
#define TOOLS_WAITER_H

#include <exec/types.h>
#include <exec/tasks.h>

#include <drv_timer.h>

struct waiter
{
	struct Task *task;
	BYTE sig; /* the session's signal, -1 while unallocated */
	struct drv_timer timer;
};

BOOL waiter_open(struct waiter *w);
void waiter_close(struct waiter *w);
ULONG waiter_wait(struct waiter *w, ULONG extra, ULONG timeout_ms);

/**
 * waiter_sigmask() - The waiter's own signal as a mask (VCA_SigMask).
 * @w: the waiter, open
 *
 * Return: the mask.
 */
static inline ULONG waiter_sigmask(const struct waiter *w)
{
	return 1UL << w->sig;
}

#endif /* TOOLS_WAITER_H */
