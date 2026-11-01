/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * kv.c - the test tools' output grammar: key=value lines, notes, the
 * verdict, and the marks in the debug log.  Hosted (libnix): Printf() is
 * DOS's.
 */
#ifdef __INTELLISENSE__
#include <clib/dos_protos.h>
#else
#include <proto/dos.h>
#endif

#include <dos/dos.h>
#include <stdarg.h>

#include <string.h>

#include "debugport.h"
#include "kv.h"

static const char *tool_name = "?";
static BOOL trace_on;

/**
 * out() - One line to stdout, flushed (a tool broken mid-run leaves what it printed), and to the debug log when tracing.
 * @fmt: DOS/RawDoFmt() format
 */
static void out(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	VPrintf((CONST_STRPTR)fmt, (APTR)ap);
	va_end(ap);
	Flush(Output());
	if (!trace_on)
		return;
	va_start(ap, fmt);
	debugport_printf("[%s] ", tool_name);
	debugport_vprintf(fmt, ap);
	va_end(ap);
}

/**
 * kv_init() - Name the tool and choose tracing; print the format line.
 * @tool:  the tool's name, the debug-log prefix
 * @trace: TRUE to echo every line to the debug log
 */
void kv_init(const char *tool, BOOL trace)
{
	tool_name = tool;
	trace_on = trace;
	kv_num("format", KV_FORMAT);
}

/**
 * kv_begin() - The begin mark in the debug log: the tool and its arguments.
 * @args: the command line (GetArgStr()), newline included or not
 */
void kv_begin(const char *args)
{
	debugport_printf("[%s] begin %s", tool_name, args);
	if (args[0] == 0 || args[(ULONG)strlen(args) - 1u] != '\n')
		debugport_puts("\n");
}

/**
 * kv_end() - The end mark in the debug log: the return code.
 * @rc: the tool's return code
 */
void kv_end(int rc)
{
	debugport_printf("[%s] end rc=%ld\n", tool_name, (LONG)rc);
}

/**
 * kv_str() - key=string.
 * @key:   the key
 * @value: the string, as it is
 */
void kv_str(const char *key, const char *value)
{
	out("%s=%s\n", key, value);
}

/**
 * kv_num() - key=signed decimal.
 * @key:   the key
 * @value: the number
 */
void kv_num(const char *key, LONG value)
{
	out("%s=%ld\n", key, value);
}

/**
 * kv_unum() - key=unsigned decimal.
 * @key:   the key
 * @value: the number
 */
void kv_unum(const char *key, ULONG value)
{
	out("%s=%lu\n", key, value);
}

/**
 * kv_version() - key=version.revision.
 * @key:      the key
 * @version:  the version
 * @revision: the revision
 */
void kv_version(const char *key, ULONG version, ULONG revision)
{
	out("%s=%lu.%lu\n", key, version, revision);
}

/**
 * kv_hex() - key=0x hex, eight digits.
 * @key:   the key
 * @value: the number
 */
void kv_hex(const char *key, ULONG value)
{
	out("%s=0x%08lx\n", key, value);
}

/**
 * kv_bool() - key=yes or key=no.
 * @key:   the key
 * @value: the flag
 */
void kv_bool(const char *key, BOOL value)
{
	kv_str(key, value ? "yes" : "no");
}

/**
 * kv_fixed1() - key=quotient with one decimal (a rate: frames per second).
 * @key: the key
 * @num: the numerator
 * @den: the denominator; 0 prints 0.0
 */
void kv_fixed1(const char *key, ULONG num, ULONG den)
{
	ULONG tenths = den != 0 ? (ULONG)((unsigned long long)num * 10u / den) : 0;
	out("%s=%lu.%lu\n", key, tenths / 10u, tenths % 10u);
}

/**
 * kv_at() - An indexed group's key: group.index.field.
 * @group: the group
 * @index: the entry's number, from 0
 * @field: the field
 *
 * Return: the key in a static buffer, good until the next call.
 */
const char *kv_at(const char *group, ULONG index, const char *field)
{
	static char key[96];
	ULONG n = 0;
	while (*group != 0 && n < sizeof(key) - 16u)
		key[n++] = *group++;
	key[n++] = '.';
	char digits[12];
	ULONG d = 0;
	do
	{
		digits[d++] = (char)('0' + index % 10u);
		index /= 10u;
	} while (index != 0);
	while (d != 0)
		key[n++] = digits[--d];
	key[n++] = '.';
	while (*field != 0 && n < sizeof(key) - 1u)
		key[n++] = *field++;
	key[n] = 0;
	return key;
}

/**
 * note() - A line for people, indented two spaces: never parsed.
 * @fmt: DOS/RawDoFmt() format
 */
void note(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	PutStr((CONST_STRPTR) "  ");
	VPrintf((CONST_STRPTR)fmt, (APTR)ap);
	va_end(ap);
	Flush(Output());
	if (!trace_on)
		return;
	va_start(ap, fmt);
	debugport_printf("[%s]   ", tool_name);
	debugport_vprintf(fmt, ap);
	va_end(ap);
}

/**
 * kv_verdict() - The last line: PASS for RETURN_OK, FAIL for anything else.
 * @rc: the tool's return code
 */
void kv_verdict(int rc)
{
	out("%s\n", rc == RETURN_OK ? "PASS" : "FAIL");
}
