/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * kv.h - the test tools' output grammar (tests/README.md): one fact per
 * line as key=value, prose for people indented two spaces, PASS or FAIL
 * last.  The first line of a run is format=1; the test suite parses nothing
 * else.
 *
 *   key   := [a-z][a-z0-9_]* ("." [a-z0-9_]+)*   indexed groups by number: stream.0.width
 *   value := the rest of the line: decimal, 0x hex, yes/no, one-decimal rates, names verbatim
 *
 * Every line goes to stdout; with the trace flag it is echoed to the debug
 * log as well, prefixed with the tool's name.  The begin and end marks go
 * to the debug log always: the suite slices the log by them.
 */
#ifndef TOOLS_KV_H
#define TOOLS_KV_H

#include <exec/types.h>

#define KV_FORMAT 1

void kv_init(const char *tool, BOOL trace);
void kv_begin(const char *args);
void kv_end(int rc);

void kv_str(const char *key, const char *value);
void kv_num(const char *key, LONG value);
void kv_unum(const char *key, ULONG value);
void kv_version(const char *key, ULONG version, ULONG revision);
void kv_hex(const char *key, ULONG value);
void kv_bool(const char *key, BOOL value);
void kv_fixed1(const char *key, ULONG num, ULONG den);
const char *kv_at(const char *group, ULONG index, const char *field);
void note(const char *fmt, ...);
void kv_verdict(int rc);

#endif /* TOOLS_KV_H */
