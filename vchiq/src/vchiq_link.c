/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * vchiq_link.c - the connection to the VPU: bring-up, teardown, reset path.
 *
 * vchiq_link_up() runs at the first OpenDevice(): it takes the window from
 * cma.resource, finds the hardware, classifies what the window holds (fresh,
 * a quiesced session, a session left mid-flight) and initialises, resumes or
 * repairs it, then starts the unit task and the interrupt server.
 *  vchiq_link_down() runs at expunge, quiesces the connection and gives the
 * block back (its contents stay); a reset (soft or keyboard) quiesces it
 * through vchiq_link_reset(), the reset guard's work, so that the next session
 * resumes.
 */
#include <drv_timer.h>
#include <timing.h>

#include <device.h> /* first: __NOLIBBASE__ for the proto header below */

#include <resources/cma.h>
#ifdef __INTELLISENSE__
#include <clib/cma_protos.h>
#else
#include <proto/cma.h>
#endif

/* ---- the reset path ------------------------------------------------------ */

/*
 * The reset guard's work: interrupt-safe, bounded, no Wait().  Takes the ring
 * over from the task and the clients, closes every open service with the
 * polled core and quiesces, so the next boot resumes.  If someone is inside
 * a critical section and does not leave within 50 ms (an interrupt-context
 * reset cannot let a task run), the connection is marked for repair instead.
 */
void vchiq_link_reset(APTR user)
{
	struct VCHIQDevice *base = user;
	struct vchiq_ctx *ctx = &base->ctx;

	if (!base->up)
		return; /* no connection yet (or any more): nothing to quiesce */

	/* No new critical sections from here on: vchiq_ring_enter() fails for the task and the clients. */
	base->halted = 1;
	emu68_barrier(); /* raised before the count is read: see vchiq_ring_enter() */
	/* Those already inside get 50 ms to leave; under a keyboard reset no task runs, so they may never. */
	for (u32 i = 0; i < 50 && base->ring_busy != 0; i++)
		delay_us(1000);
	if (base->ring_busy != 0)
	{
		/* someone is mid-update: the ring cannot be trusted, the next session repairs it */
		Kprintf("[vchiq] reset: ring in use, leaving the connection for repair\n");
		vchiq_mark_dirty(ctx);
		return;
	}
#ifdef VCHIQ_TEST_HOOKS
	if (base->test_abandon)
	{
		/* NSCMD_VQ_TEST_ABANDON: no quiesce, the record left busy as a crash leaves it; the next session repairs it */
		Kprintf("[vchiq] reset: left as a crash would (test hook)\n");
		return;
	}
#endif

	/* The ring is ours alone now: close every service, polled, and quiesce so the next boot resumes. */
	ctx->timer = NULL; /* the unit task's: here no Wait(), the waits spin */
	s32 rc __attribute__((unused)) = vchiq_shutdown(ctx, 100000u); /* <= 100 ms per service; with the 50 ms above <= 1.65 s, reset_guard allows 5 s */
	Kprintf("[vchiq] reset: %s\n", rc == 0 ? "quiesced" : "could not quiesce, repair at next boot");
}

/* ---- the window ----------------------------------------------------------- */

/**
 * window_alloc() - Take the shared window from cma.resource.
 * @base: the device, lock held; fills CMABase, window_base, window_size
 *
 * The first page-aligned block of a fresh zone is its base + 4096, whatever
 * the size: cma.resource rebuilds its heap at every 68k reset and never writes
 * into a block, so the first client gets the same window back with the last
 * session's record in it.  vchiq.device has to be that first client.
 *
 * Return: 0, or VQERR_NOCONNECTION with the cause logged.
 */
static s32 window_alloc(struct VCHIQDevice *base)
{
	struct ExecBase *SysBase = base->sysBase;
	APTR CMABase = base->CMABase = OpenResource((CONST_STRPTR)CMANAME);
	if (CMABase == NULL)
	{
		Kprintf("[vchiq] no cma.resource: add dtoverlay=cma to config.txt\n");
		return VQERR_NOCONNECTION;
	}
	APTR window = CMA_AllocAligned(VCHIQ_WINDOW_SIZE, 4096);
	if (window == NULL)
	{
		Kprintf("[vchiq] cma.resource: no %ld KiB block (zone too small, or taken)\n", (ULONG)(VCHIQ_WINDOW_SIZE / 1024u));
		return VQERR_NOCONNECTION;
	}
	base->window_base = (u32)window;
	base->window_size = VCHIQ_WINDOW_SIZE;
	return 0;
}

/**
 * window_free() - Give the window back to cma.resource.
 * @base: the device, lock held
 *
 * The block's contents stay: the next first client (this device reloaded)
 * gets the same block and resumes the session in it.
 */
static void window_free(struct VCHIQDevice *base)
{
	if (base->window_base == 0)
		return;
	APTR CMABase = base->CMABase;
	CMA_FreeMem((APTR)base->window_base);
	base->window_base = 0;
}

/* ---- link up / link down ------------------------------------------------- */

/* only the debug log reads it, which a tier of "off" compiles out */
static __attribute__((unused)) const char *state_name(s32 state)
{
	switch (state)
	{
	case VCHIQ_STATE_FRESH:
		return "fresh";
	case VCHIQ_STATE_LIVE_CLEAN:
		return "live, quiesced";
	default:
		return "live, dirty";
	}
}

/* Called with the lock held, at the first open.  0, or VQERR_NOCONNECTION with the cause logged. */
s32 vchiq_link_up(struct VCHIQDevice *base)
{
	struct ExecBase *SysBase = base->sysBase;
	struct vchiq_ctx *ctx = &base->ctx;

	base->MailboxBase = OpenResource((CONST_STRPTR) "mailbox.resource");
	if (base->MailboxBase == NULL)
	{
		Kprintf("[vchiq] no mailbox.resource\n");
		return VQERR_NOCONNECTION;
	}

	if (base->window_base == 0 && window_alloc(base) != 0) /* the first open; a failed bring-up keeps the block for the next try */
		return VQERR_NOCONNECTION;

	if (DevTreeParse(base) != 0)
		return VQERR_NOCONNECTION;

	base->halted = 0;
	base->ring_busy = 0;
	s32 rc = vchiq_attach(ctx, SysBase, (volatile void *)base->window_base, base->window_size, base->window_bus, base->bell,
	                      &base->platform);
	if (rc != 0)
	{
		Kprintf("[vchiq] window at %08lx (%ld KiB) unusable: %ld\n", base->window_base, base->window_size / 1024u, rc);
		return VQERR_NOCONNECTION;
	}

	/* the RTG card's VRAM (an ISP writing a screen's bitmap): mapped write-through, so never cached */
	ctx->uncached_start = base->vram_start;
	ctx->uncached_end = base->vram_end;

	s32 state = vchiq_classify(ctx);
	Kprintf("[vchiq] window at %08lx (VPU bus %08lx), connection %s\n", base->window_base, base->window_bus,
	        state_name(state));

	struct drv_timer timer; /* the bring-up's waits sleep in the opener's task */
	if (drv_timer_open(&timer, SysBase))
		ctx->timer = &timer;
	rc = vchiq_connect(ctx, base->MailboxBase, VCHIQ_BRINGUP_US);
	ctx->timer = NULL;
	drv_timer_close(&timer);
	
	if (rc == 0 && state != VCHIQ_STATE_FRESH && ctx->host->sessions == 1)
		Kprintf("[vchiq] the record was stale (the VPU had restarted): re-initialised%s\n",
		        state == VCHIQ_STATE_LIVE_DIRTY ? " (no repair possible)" : "");
	else if (state == VCHIQ_STATE_LIVE_DIRTY)
		Kprintf("[vchiq] repair of the previous session: %s\n", rc == 0 ? "ok" : "failed, and the VPU still answers");
	if (rc != 0)
	{
		Kprintf("[vchiq] bring-up failed: %ld (tag value %08lx)\n", rc, ctx->last_mailbox_value);
		return VQERR_NOCONNECTION;
	}
	if (state == VCHIQ_STATE_FRESH)
		base->bringup = VQBU_FRESH;
	else if (ctx->host->sessions == 1)
		base->bringup = VQBU_REINIT;
	else
		base->bringup = state == VCHIQ_STATE_LIVE_DIRTY ? VQBU_REPAIRED : VQBU_RESUMED;

	rc = UnitTaskStart(base);
	if (rc != 0)
	{
		Kprintf("[vchiq] task failed to start\n");
		return VQERR_NOCONNECTION;
	}

	rc = vchiq_int_enable(base);
	if (rc != 0)
	{
		UnitTaskStop(base);
		return VQERR_NOCONNECTION;
	}

	base->up = TRUE;
	Kprintf("[vchiq] up: session %ld, VCHIQ version %ld, %s %ld, task %08lx\n", ctx->host->sessions,
	        ctx->host->version_common, base->irq >= 0 ? "interrupt" : "polling every ms:",
	        base->irq >= 0 ? base->irq : (LONG)UNIT_POLL_MS, (ULONG)base->task);
	/* the task has not polled yet: wake it so it arms the doorbell */
	Signal(base->task, 1UL << base->irq_signal);
	return 0;
}

/*
 * Called without the lock, at expunge.  The task is halted and joined with the
 * lock released: it takes the lock at the top of every round.  The window goes
 * back to cma.resource whether the connection came up or not.
 */
void vchiq_link_down(struct VCHIQDevice *base)
{
	struct ExecBase *SysBase = base->sysBase;
	struct vchiq_ctx *ctx = &base->ctx;

	ObtainSemaphore(&base->lock);
	if (base->up)
	{
		vchiq_int_shutdown(base);
		base->halted = 1; /* the task and the clients stay off the ring from here on */
		ReleaseSemaphore(&base->lock);

		UnitTaskStop(base);

		ObtainSemaphore(&base->lock);
		base->ring_busy++; /* a reset meanwhile waits for this, or leaves the connection for repair */
		emu68_barrier();

		struct drv_timer timer; /* the task is gone: the waits sleep in the expunging task */
		if (drv_timer_open(&timer, SysBase))
			ctx->timer = &timer;
		(void)vchiq_shutdown(ctx, 500000u); /* a failure leaves the connection for repair */
		ctx->timer = NULL;
		drv_timer_close(&timer);

		base->up = FALSE;
		emu68_barrier();
		base->ring_busy--;
	}
	window_free(base);
	ReleaseSemaphore(&base->lock);
}
