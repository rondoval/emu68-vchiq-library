/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * smem_client.h - the SMEM client (smem_client.c): memory of ours imported
 * into the VPU, so an MMAL port works in it with no copy of its own.
 *
 * An import wraps one physically contiguous range, given by its VPU bus
 * address, in a handle the VPU knows.  Whether a range may be imported at
 * all (in the VPU's reach, Pi RAM or the VRAM, whole cache lines where the
 * VPU writes) is the MMAL client's question (mmal_zero_copy.c); this file
 * is the protocol and the table of imports.
 *
 * The service opens with the link and closes with it; without it nothing is
 * imported.  The VPU answers an import with its handle, a free not at all;
 * told our protocol version (2), it reports RELEASED when its last reference
 * to an import goes, and the memory is the owner's again only then
 * (smem_released_wait()).  It may also ask for memory of ours: refused.
 *
 * Everything here runs under mmal_lock.  The callers' functions may sleep
 * with it released (link_wait()); the receive task's never wait.
 */
#ifndef SMEM_CLIENT_H
#define SMEM_CLIENT_H

#include <exec/types.h>

#include <types.h>

struct VideoCoreBase;

#define SMEM_IMPORTS_MAX 64 /* imports live at once: a port whose buffers find no room goes by bulk */

/* Where an entry of the table is.  FREE is 0: a cleared table is all free. */
enum smem_entry_state
{
	SMEM_ENTRY_FREE = 0,  /* unused */
	SMEM_ENTRY_IMPORTING, /* the IMPORT went, its answer is awaited */
	SMEM_ENTRY_LIVE,      /* the VPU holds it: the handle names it */
	SMEM_ENTRY_FAILED,    /* the VPU refused it, or the message did not go: back to FREE by its importer */
	SMEM_ENTRY_RELEASING, /* the FREE message went, RELEASED is awaited */
	SMEM_ENTRY_LOST,      /* RELEASED never came: the entry stays out of use until the service closes */
};

struct smem_import
{
	u8 state;       /* enum smem_entry_state */
	u16 generation; /* with the index, the cookie the VPU echoes (transaction and kernel ids) */
	u32 addr;       /* the memory, as the host addresses it (the logs) */
	u32 size;       /* its bytes */
	u32 handle;     /* the VPU's handle, while LIVE or RELEASING */
};

/* The client's state, in the library base under mmal_lock */
struct smem_client
{
	BOOL up;                /* the service is open and answers: imports can be made */
	BOOL releases_reported; /* the VPU took protocol version 2: it reports RELEASED for every free */
	u16 generation;         /* the last cookie generation handed out */
	u32 live;               /* entries not FREE */
	u32 stray_messages;     /* messages that matched nothing (logged at the close) */
	struct smem_import imports[SMEM_IMPORTS_MAX];
};

/* The memory a caller wants imported */
struct smem_range
{
	APTR data; /* as the host addresses it */
	u32 bus;   /* as the VPU addresses it (reach_bus()) */
	u32 size;
};

/* The callers' (a task, mmal_lock held) */
/* Import ranges, one round trip for all: ids[] get the imports (0 for none); 0, or an error with none made */
s32 smem_import(struct VideoCoreBase *base, const struct smem_range *ranges, u32 count, u32 *ids);
/* The VPU's handle of an import */
u32 smem_handle(const struct VideoCoreBase *base, u32 id);
/* Let go of an import: its FREE goes, nothing waits */
void smem_release(struct VideoCoreBase *base, u32 id);
/* Sleep until the VPU reported every released import back (LINK_WAIT_MS at most) */
void smem_released_wait(struct VideoCoreBase *base);

/* The link's (videocore_link.c), mmal_lock held */
/* The service opened: tell the VPU our version, imports may be made */
void smem_up(struct VideoCoreBase *base);
/* The service is about to close: what is still imported is logged and forgotten */
void smem_closing(struct VideoCoreBase *base);
/* A message from the VPU (the receive task) */
void smem_message(struct VideoCoreBase *base, const u8 *data, u32 size);
/* A message of ours did not reach the VPU (the receive task) */
void smem_send_failed(struct VideoCoreBase *base, u32 cookie);
/* The service went: nothing more is imported, what waits gives up */
void smem_lost(struct VideoCoreBase *base);

#endif /* SMEM_CLIENT_H */
