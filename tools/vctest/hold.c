/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * hold.c - HOLD: the free RAM the VPU reaches taken, all but LEAVE MiB at the
 * end of the largest chunk, until Ctrl-C (tests/test_low_memory.py).  By
 * Allocate() on each memory header, as the DMA pool takes its arenas
 * (dma_mem.c): AllocMem() passes one of those headers over.
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#include <proto/exec.h>
#endif

#include <dos/dos.h>
#include <exec/execbase.h>
#include <exec/memory.h>

#include <args.h>
#include <kv.h>

#include "commands.h"

const char hold_template[] = "LEAVE/N/A";

enum
{
	ARG_LEAVE
};

#define REACH_END 0x40000000u /* the VPU reaches the lowest GiB */
#define CHIP_END 0x00200000u
#define CHUNK_MIN (1024u * 1024u)
#define HELD_MAX 64u

struct held
{
	struct MemHeader *header;
	APTR at;
	ULONG size;
};

/* Whether a memory header is Fast RAM the VPU reaches. */
static BOOL in_reach(const struct MemHeader *header)
{
	ULONG upper = (ULONG)header->mh_Upper;
	return upper <= REACH_END && upper > CHIP_END;
}

/* A header's largest free chunk but @kept; NULL for none. */
static struct MemChunk *chunk_largest(const struct MemHeader *header, const struct MemChunk *kept)
{
	struct MemChunk *best = NULL;
	for (struct MemChunk *chunk = header->mh_First; chunk != NULL; chunk = chunk->mc_Next)
		if (chunk != kept && (best == NULL || chunk->mc_Bytes > best->mc_Bytes))
			best = chunk;
	return best;
}

/**
 * do_hold() - HOLD: take the memory, wait for Ctrl-C, give it back.
 * @t:    the tool
 * @args: hold_template's
 *
 * Return: RETURN_OK.
 */
int do_hold(struct tool *t, LONG *args)
{
	(void)t;
	static struct held held[HELD_MAX];
	ULONG leave = arg_num(args, ARG_LEAVE, 0) << 20, count = 0, taken = 0, best = 0;
	struct MemHeader *largest = NULL;
	const struct MemChunk *kept = NULL;

	Forbid();
	for (struct Node *node = SysBase->MemList.lh_Head; node->ln_Succ != NULL; node = node->ln_Succ)
	{
		struct MemHeader *header = (struct MemHeader *)node;
		struct MemChunk *chunk = in_reach(header) ? chunk_largest(header, NULL) : NULL;
		if (chunk != NULL && chunk->mc_Bytes > best)
		{
			best = chunk->mc_Bytes;
			largest = header;
		}
	}
	for (struct Node *node = SysBase->MemList.lh_Head; node->ln_Succ != NULL; node = node->ln_Succ)
	{
		struct MemHeader *header = (struct MemHeader *)node;
		while (in_reach(header) && count < HELD_MAX)
		{
			const struct MemChunk *chunk = chunk_largest(header, kept);
			if (chunk == NULL || chunk->mc_Bytes < CHUNK_MIN)
				break;
			BOOL leaving = header == largest && kept == NULL;
			ULONG size = leaving ? chunk->mc_Bytes - leave : chunk->mc_Bytes;
			APTR at = Allocate(header, size);
			if (at == NULL)
				break;
			held[count++] = (struct held){header, at, size};
			taken += size;
			if (leaving)
				kept = (const struct MemChunk *)((ULONG)at + size); /* what is left starts there */
		}
	}
	Permit();

	kv_unum("hold.chunks", count);
	kv_unum("hold.mib", taken >> 20);
	Wait(SIGBREAKF_CTRL_C);

	Forbid();
	for (ULONG i = 0; i < count; i++)
		Deallocate(held[i].header, held[i].at, held[i].size);
	Permit();
	return RETURN_OK;
}
