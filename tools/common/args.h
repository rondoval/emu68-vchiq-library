/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * args.h - ReadArgs() values with defaults.
 */
#ifndef TOOLS_ARGS_H
#define TOOLS_ARGS_H

#include <exec/types.h>

/**
 * arg_num() - A /N argument's value, or a default when it was not given.
 * @args: ReadArgs()'s array
 * @i:    the argument's index
 * @def:  the default
 *
 * Return: the value.
 */
static inline ULONG arg_num(const LONG *args, ULONG i, ULONG def)
{
	return args[i] != 0 ? (ULONG)*(const LONG *)args[i] : def;
}

/**
 * arg_str() - A /K or /A string argument, NULL when it was not given.
 * @args: ReadArgs()'s array
 * @i:    the argument's index
 *
 * Return: the string, or NULL.
 */
static inline const char *arg_str(const LONG *args, ULONG i)
{
	return (const char *)args[i];
}

#endif /* TOOLS_ARGS_H */
