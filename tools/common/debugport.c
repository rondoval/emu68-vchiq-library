/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * debugport.c - lines to Emu68's debug log: every byte written to the
 * PiStorm debug address comes out on the serial log.
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#include <proto/exec.h>
#endif

#include <exec/types.h>

#include "debugport.h"

#define DEBUG_PORT 0xdeadbeefUL

/**
 * putch() - RawDoFmt()'s character sink: one byte to the debug port.
 * @c:    the character
 * @data: unused
 */
static void putch(char c asm("d0"), APTR data asm("a3"))
{
	(void)data;
	*(volatile UBYTE *)DEBUG_PORT = (UBYTE)c;
}

/**
 * debugport_puts() - A string to the debug log, as it is.
 * @s: the string
 */
void debugport_puts(const char *s)
{
	while (*s != 0)
		*(volatile UBYTE *)DEBUG_PORT = (UBYTE)*s++;
}

/**
 * debugport_vprintf() - A formatted line to the debug log.
 * @fmt: RawDoFmt() format (%ld, %lu, %lx, %s)
 * @ap:  its arguments
 */
void debugport_vprintf(const char *fmt, va_list ap)
{
	RawDoFmt((CONST_STRPTR)fmt, (APTR)ap, (void (*)())putch, NULL);
}

/**
 * debugport_printf() - A formatted line to the debug log.
 * @fmt: RawDoFmt() format
 */
void debugport_printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	debugport_vprintf(fmt, ap);
	va_end(ap);
}
