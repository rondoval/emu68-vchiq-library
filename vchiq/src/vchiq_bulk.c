/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * vchiq_bulk.c - bulk transfers: the fragment pool, cache maintenance and the
 * partial lines of a receive, the page lists, and a bulk's completion.
 */
#include <vchiq/vchiq_core_priv.h>

#include <cache_ops.h>

/* ---- the fragment pool ------------------------------------------------------ */

/*
 * The fragment pool: one 128-byte slot per receive with unaligned ends, handed
 * out from a bitmap (a set bit is a free slot), lowest free slot first.
 */
static s32 alloc_fragment(struct vchiq_ctx *ctx)
{
	for (u32 w = 0; w < VCHIQ_MAX_FRAGMENTS / 32u; w++)
	{
		u32 free = ctx->fragments_free[w];
		if (free != 0)
		{
			u32 bit = 31u - (u32)__builtin_clz(free & -free); /* x & -x keeps the lowest set bit; BFFFO finds it */
			ctx->fragments_free[w] = free & (free - 1u);       /* and this clears it */
			return (s32)(w * 32u + bit);
		}
	}
	return -1;
}

/* Fragment slot @i back to the pool. */
static void free_fragment(struct vchiq_ctx *ctx, u32 i)
{
	ctx->fragments_free[i >> 5] |= 1u << (i & 31u);
}

/* The bulk's fragment slot, if it has one, back to the pool. */
static void release_fragment(struct vchiq_ctx *ctx, struct vchiq_bulk *bulk)
{
	if (bulk->fragment == 0)
		return;
	free_fragment(ctx, bulk->fragment - 1u);
	bulk->fragment = 0;
}

/* ---- cache maintenance and the partial lines ------------------------------ */

/*
 * Emu68 maps its VRAM carve-out inner and outer write-through, which both
 * CPUs it runs on downgrade to non-cacheable (Cortex-A72 TRM table 6-1,
 * Cortex-A53 TRM section 7.5): a buffer in there has no cache lines to
 * maintain, and the range operations over a 6 MB picture would cost the 68k
 * milliseconds per frame for nothing.
 */
static BOOL needs_cache_ops(const struct vchiq_ctx *ctx, u32 start, u32 size)
{
	return !(start >= ctx->uncached_start && start + size <= ctx->uncached_end);
}

/* A receive with an end inside a cache line: the DMA writes whole lines only, so those ends go through a fragment. */
static BOOL has_partial_lines(u32 dir, u32 start, u32 size)
{
	return dir == VCHIQ_BULK_RX && (line_offset(start) != 0 || line_offset(start + size) != 0);
}

/*
 * The partial lines at the ends of @got received bytes, copied in from the
 * bulk's fragment slot: the head from its first line, the tail from its
 * second.
 */
static void copy_partial_lines(const struct vchiq_ctx *ctx, const struct vchiq_bulk *bulk, u32 got)
{
	u32 start = (u32)bulk->data;
	const volatile u8 *frag = ctx->fragments + (bulk->fragment - 1u) * VCHIQ_FRAGMENT_SIZE;
	u32 head = (VCHIQ_CACHE_LINE_SIZE - line_offset(start)) & LINE_MASK; /* bytes before the first whole line */
	u32 tail = line_offset(start + got);                                  /* bytes after the last whole line */

	if (head > got)
		head = got; /* all of it within the first line */
	memcpy((void *)start, (const void *)frag, head);
	if (head < got && tail != 0)
		memcpy((void *)((start + got) & ~LINE_MASK), (const void *)(frag + VCHIQ_CACHE_LINE_SIZE), tail);
}

/*
 * A receive done: the DMA wrote the whole cache lines, whose stale copies are
 * dropped; the partial lines at the ends are copied in by hand from the
 * fragment slot, never invalidated: they hold other people's bytes, which an
 * invalidate would lose.
 */
static void finish_receive(struct vchiq_ctx *ctx, struct vchiq_bulk *bulk, s32 actual)
{
	struct ExecBase *SysBase = ctx->SysBase;
	u32 start = (u32)bulk->data;
	u32 inner_start = (start + LINE_MASK) & ~LINE_MASK;
	u32 inner_end = (start + bulk->size) & ~LINE_MASK;

	if (inner_end > inner_start && needs_cache_ops(ctx, start, bulk->size))
	{
		PERF_T0(t0);
		cache_post_dma((APTR)inner_start, inner_end - inner_start, DMA_WriteToRAM);
		PERF_ADD(&ctx->perf, VQP_RX_INVAL, t0);
	}
	if (bulk->fragment != 0 && actual > 0)
		copy_partial_lines(ctx, bulk, (u32)actual);
	release_fragment(ctx, bulk);
}

/* ---- completion -------------------------------------------------------------- */

/* The oldest bulk of @dir in flight on @svc, taken off its queue; NULL if none. */
struct vchiq_bulk *vq_pop_bulk(struct vchiq_service *svc, u32 dir)
{
	if (svc->bulk_head[dir] == svc->bulk_tail[dir])
		return NULL;
	struct vchiq_bulk *bulk = &svc->bulks[dir][svc->bulk_head[dir] % VCHIQ_NUM_SERVICE_BULKS];
	svc->bulk_head[dir]++;
	return bulk;
}

/* A bulk is over (@actual bytes, or VCHIQ_BULK_ACTUAL_ABORTED): its buffer is the client's again. */
void vq_finish_bulk(struct vchiq_ctx *ctx, struct vchiq_service *svc, u32 dir, struct vchiq_bulk *bulk, s32 actual)
{
	static const u8 reasons[2][2] = {
	    /* [aborted][dir] */
	    {VCHIQ_REASON_BULK_TRANSMIT_DONE, VCHIQ_REASON_BULK_RECEIVE_DONE},
	    {VCHIQ_REASON_BULK_TRANSMIT_ABORTED, VCHIQ_REASON_BULK_RECEIVE_ABORTED},
	};
	BOOL aborted = actual == VCHIQ_BULK_ACTUAL_ABORTED;

	if (dir == VCHIQ_BULK_RX && !aborted)
	{
		PERF_ADD(&ctx->perf, VQP_RX_BULK, bulk->queued_at);
		finish_receive(ctx, bulk, actual); /* the DMA engine wrote RAM behind the cache */
	}
	else
		release_fragment(ctx, bulk);
	/* the record is free once popped: a bulk the callback queues on this service and direction may take it */
	struct vchiq_bulk done = *bulk;
	vq_make_service_callback(ctx, svc, reasons[aborted][dir], &done, actual);
}

/* ---- queueing ---------------------------------------------------------------- */

/* Fewer than VCHIQ_NUM_SERVICE_BULKS bulks of @dir in flight on @svc. */
static BOOL has_bulk_room(const struct vchiq_service *svc, u32 dir)
{
	return svc->bulk_tail[dir] - svc->bulk_head[dir] < VCHIQ_NUM_SERVICE_BULKS;
}

/* 36-bit page lists reach all RAM; 32-bit ones name bus addresses, so only where dma-ranges maps the VPU. */
static BOOL is_reachable(const struct vchiq_platform *pf, u32 start, u32 size)
{
	return pf->addr36 || (start >= pf->data_cpu_start && start < pf->data_cpu_end && size <= pf->data_cpu_end - start);
}

/* Pages one addrs[] entry can cover in the SoC's page-list format. */
static u32 pages_per_entry(const struct vchiq_platform *pf)
{
	return pf->addr36 ? VCHIQ_PAGELIST_PAGES_PER_ENTRY_36 : VCHIQ_PAGELIST_PAGES_PER_ENTRY_32;
}

/* Pages the buffer touches. */
static u32 page_count(u32 start, u32 size)
{
	return ((start & PAGE_MASK) + size + PAGE_MASK) / PAGE_SIZE;
}

/* Whether the transfer can be queued at all: 0, or the error; nothing changes. */
static s32 check_bulk(const struct vchiq_ctx *ctx, const struct vchiq_service *svc, u32 dir, APTR data, u32 size)
{
	if (svc->state != VCHIQ_SVC_OPEN)
		return VQERR_CLOSED;
	if (data == NULL)
		return IOERR_BADADDRESS;
	if (size == 0 || dir > VCHIQ_BULK_RX)
		return IOERR_BADLENGTH;
	if (!has_bulk_room(svc, dir))
		return IOERR_UNITBUSY;
	if (!is_reachable(&ctx->platform, (u32)data, size))
		return IOERR_BADADDRESS;
	u32 max_pages = pages_per_entry(&ctx->platform);
	if ((page_count((u32)data, size) + max_pages - 1u) / max_pages > VCHIQ_PAGELIST_MAX_ENTRIES)
		return IOERR_BADLENGTH; /* more runs than a page-list slot holds */
	return 0;
}

/*
 * The window offset of the page list of (@svc, @dir)'s bulk @index: one fixed
 * slot per bulk, so none is ever allocated.
 */
static u32 pagelist_offset(const struct vchiq_ctx *ctx, const struct vchiq_service *svc, u32 dir, u32 index)
{
	u32 slot = (svc->localport * 2u + dir) * VCHIQ_NUM_SERVICE_BULKS + index;
	return (u32)(ctx->pagelists - ctx->window) + slot * VCHIQ_PAGELIST_SLOT_SIZE;
}

/* The page list of the buffer: runs of contiguous pages in the SoC's format (vchiq_protocol.h). */
static void write_pagelist(const struct vchiq_ctx *ctx, volatile struct vchiq_pagelist *pl, u32 start, u32 size, u16 type)
{
	const struct vchiq_platform *pf = &ctx->platform;
	pl->length = le32(size);
	pl->type = le16(type);
	pl->offset = le16((u16)(start & PAGE_MASK));

	u32 max_pages = pages_per_entry(pf);
	u32 entry_page = pf->addr36 ? (start & ~PAGE_MASK) >> 4                   /* bits 8..31 = address bits 12..35 */
	                            : (start + bus_offset(ctx)) & ~PAGE_MASK;     /* the bus address, bits 12..31 */
	u32 one_page = pf->addr36 ? 1u << 8 : PAGE_SIZE;                          /* one page in the encoding */
	for (u32 entry = 0, pages_left = page_count(start, size); pages_left != 0; entry++)
	{
		u32 entry_pages = pages_left > max_pages ? max_pages : pages_left;
		pl->addrs[entry] = le32(entry_page | (entry_pages - 1u)); /* the page above, the count - 1 below */
		entry_page += entry_pages * one_page;
		pages_left -= entry_pages;
	}
	emu68_barrier(); /* the list before the message that names it */
}

/* Queue a bulk transfer on an OPEN service: its page list written, BULK_TX or BULK_RX in our stream. */
s32 vchiq_queue_bulk(struct vchiq_ctx *ctx, struct vchiq_service *svc, u32 dir, APTR data, u32 size, APTR userdata)
{
	s32 rc = check_bulk(ctx, svc, dir, data, size);
	if (rc != 0)
		return rc;

	u16 type = dir == VCHIQ_BULK_TX ? (u16)VCHIQ_PAGELIST_WRITE : (u16)VCHIQ_PAGELIST_READ;
	s32 frag = -1;
	if (has_partial_lines(dir, (u32)data, size))
	{
		frag = alloc_fragment(ctx);
		if (frag < 0)
			return IOERR_UNITBUSY;
		type = (u16)(VCHIQ_PAGELIST_READ_WITH_FRAGMENTS + (u32)frag);
	}

	/* the buffer's cache lines: out to RAM before the VPU reads, out and gone before it writes */
	if (needs_cache_ops(ctx, (u32)data, size))
	{
		struct ExecBase *SysBase = ctx->SysBase;
		PERF_T0(t0);
		cache_pre_dma(data, size, dir == VCHIQ_BULK_TX ? DMA_ReadFromRAM : 0u);
		PERF_ADD(&ctx->perf, dir == VCHIQ_BULK_TX ? VQP_TX_CLEAN : VQP_RX_PREP, t0);
	}

	u32 index = svc->bulk_tail[dir] % VCHIQ_NUM_SERVICE_BULKS; /* its place in the service's ring of bulks */
	u32 pl_offset = pagelist_offset(ctx, svc, dir, index);
	write_pagelist(ctx, (volatile struct vchiq_pagelist *)(ctx->window + pl_offset), (u32)data, size, type);

	struct vchiq_bulk_payload payload = {.pagelist = le32(ctx->window_bus + pl_offset), .size = le32(size)};
	u32 msg_type = dir == VCHIQ_BULK_TX ? VCHIQ_MSG_BULK_TX : VCHIQ_MSG_BULK_RX;
	u32 msgid = VCHIQ_MAKE_MSG(msg_type, svc->localport, svc->remoteport);
	rc = vq_queue_message(ctx, msgid, &payload, sizeof(payload), 0, FALSE);
	if (rc != 0)
	{
		if (frag >= 0)
			free_fragment(ctx, (u32)frag);
		return rc;
	}

	svc->bulks[dir][index] =
	    (struct vchiq_bulk){.data = data, .size = size, .userdata = userdata, .fragment = (u8)(frag + 1)}; /* 0: none */
#ifdef PROFILE
	svc->bulks[dir][index].queued_at = get_time();
#endif
	svc->bulk_tail[dir]++;
	return 0;
}
