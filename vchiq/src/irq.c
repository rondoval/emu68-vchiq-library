/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * irq.c - the doorbell interrupt server.
 *
 * The VPU rings BELL0 when it fires one of our remote events (a message, a
 * recycled slot).  The server only acknowledges the doorbell and signals the
 * unit task, which does the work.  Only a GIC SPI (BCM2711, gic400.library) is
 * taken; without one (BCM2836/7: no driver for its interrupt controller yet)
 * nothing is installed and the unit task polls.
 */
#include <device.h> /* first: __NOLIBBASE__ for the proto headers below */

#ifdef __INTELLISENSE__
#include <clib/gic400_protos.h>
#else
#define GIC400_BASE_NAME base->gic400Base
#include <proto/gic400.h>
#endif

#include <stddef.h> /* offsetof, for the server's operands */
#include <byteorder.h>

/* exec.library Signal(), from <lvo/exec_lib.i>. */
#define LVO_SIGNAL (-324)

/**
 * vchiq_int_isr() - The doorbell's interrupt server, which gic400 calls.
 * @SysBase: exec, for Signal()
 * @base: the device (is_Data)
 * @irq: the interrupt number, unused
 *
 * Acknowledges BELL0 by reading it and, when the VPU rang, signals the unit task.
 * The caller tests the Z flag, not D0 (the Exec interrupt-server convention), so
 * this is assembly: a C server's epilogue decides that flag, and GCC 16.2.1
 * restored D2/A6 with two MOVEs after the MOVEQ, which left Z from the restored D2
 * (see interrupt-chaining.md in emu68-gic400-library).  Written out, the body needs
 * no saved register: Signal() is the last call, so each MOVEQ is followed by RTS.
 * A0/D0/D1 are scratch; A1 becomes Signal()'s task once base is finished with.
 * BELL0 is little-endian, so the ACTIVE bit is tested swapped.  File-scope asm has
 * no IR for LTO, so this TU is compiled -fno-lto (CMakeLists).
 *
 * Return: 1 (Z clear) if the doorbell was ours, 0 (Z set) otherwise.
 */
ULONG vchiq_int_isr(struct ExecBase *SysBase asm("a6"), struct VCHIQDevice *base asm("a1"), ULONG irq asm("d0"));

#ifndef __INTELLISENSE__ /* no file-scope extended asm there */
__asm__(
	/* Its own section, as EMU68_INTSERVER() gives a C server one; GCC tracks the
	 * current section itself, so the block hands .text back at the end. */
	"	.section .text.isr.vchiq_int_isr,\"ax\"\n"
	"	.even\n"
	"	.globl	_vchiq_int_isr\n"
	"_vchiq_int_isr:\n"
	"	movea.l	%c[bell](%%a1),%%a0\n"   /* A0 = base->ctx.bell */
	"	move.l	%c[bell0](%%a0),%%d0\n"  /* BELL0: the read is the acknowledge */
	"	andi.l	#%c[active],%%d0\n"
	"	bne.s	1f\n"
	"	moveq	#0,%%d0\n"               /* not ours: Z set, the chain goes on */
	"	rts\n"
	"1:	addq.l	#1,%c[ints](%%a1)\n"     /* base->interrupts++ */
	"	move.b	%c[signal](%%a1),%%d1\n"
	"	extb.l	%%d1\n"
	"	moveq	#1,%%d0\n"
	"	lsl.l	%%d1,%%d0\n"             /* D0 = 1 << base->irq_signal */
	"	movea.l	%c[task](%%a1),%%a1\n"   /* A1 = base->task */
	"	jsr	%c[lvosignal](%%a6)\n"       /* Signal(task, mask) */
	"	moveq	#1,%%d0\n"               /* ours: Z clear ends the chain walk */
	"	rts\n"
	"	.text\n"
	:
	: [bell] "i"(offsetof(struct VCHIQDevice, ctx.bell)),
	  [bell0] "i"(VCHIQ_BELL0),
	  [active] "i"(le32(VCHIQ_BELL0_ACTIVE)),
	  [ints] "i"(offsetof(struct VCHIQDevice, interrupts)),
	  [signal] "i"(offsetof(struct VCHIQDevice, irq_signal)),
	  [task] "i"(offsetof(struct VCHIQDevice, task)),
	  [lvosignal] "i"(LVO_SIGNAL));
#endif

/* The unit task must be running: the server signals it.  0 without an interrupt too: the task polls. */
s32 vchiq_int_enable(struct VCHIQDevice *base)
{
	struct ExecBase *SysBase = base->sysBase;

	if (base->irq < 0)
		return 0;
	base->gic400Base = OpenLibrary((CONST_STRPTR) "gic400.library", 0);
	if (base->gic400Base == NULL)
	{
		Kprintf("[vchiq] gic400.library missing\n");
		return VQERR_NOCONNECTION;
	}

	base->isr.is_Node.ln_Type = NT_INTERRUPT;
	base->isr.is_Node.ln_Name = (char *)"vchiq doorbell";
	base->isr.is_Data = base;
	base->isr.is_Code = (APTR)vchiq_int_isr;
	if (AddIntServerEx((ULONG)base->irq, 0, FALSE, &base->isr) < 0)
	{
		Kprintf("[vchiq] cannot register interrupt %ld\n", base->irq);
		CloseLibrary(base->gic400Base);
		base->gic400Base = NULL;
		return VQERR_NOCONNECTION;
	}
	base->isr_installed = TRUE;
	return 0;
}

/* Before the unit task stops. */
void vchiq_int_shutdown(struct VCHIQDevice *base)
{
	struct ExecBase *SysBase = base->sysBase;

	if (!base->isr_installed)
		return;
	RemIntServerEx((ULONG)base->irq, &base->isr);
	base->isr_installed = FALSE;
	CloseLibrary(base->gic400Base);
	base->gic400Base = NULL;
}
