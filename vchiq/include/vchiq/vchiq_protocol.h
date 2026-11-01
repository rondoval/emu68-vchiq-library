/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * vchiq_protocol.h - the VCHIQ shared-memory wire format of the VideoCore VPU
 * firmware on the BCM2711 (Raspberry Pi 4).
 *
 * Everything the VPU reads or writes is little-endian and 32-bit.  The
 * structures here only pin offsets; every access goes through le32()/le16().
 * The shared area must be mapped non-cacheable on the ARM side, because the
 * two sides write different words of the same cache line (see
 * docs/architecture.md, section 3).
 *
 * Derived from the Linux VCHIQ interface headers, which carry
 * "SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause",
 * Copyright (c) 2010-2012 Broadcom. All rights reserved.
 */
#ifndef VCHIQ_PROTOCOL_H
#define VCHIQ_PROTOCOL_H

#include <types.h>

#define VCHIQ_SLOT_SIZE 4096u                                /* one slot: messages never cross its end */
#define VCHIQ_SLOT_MASK (VCHIQ_SLOT_SIZE - 1u)               /* the offset bits of a stream position */
#define VCHIQ_SLOT_OFFSET(pos) ((u32)(pos) & VCHIQ_SLOT_MASK) /* stream position -> offset in its slot */
#define VCHIQ_SLOT_QUEUE_INDEX(pos) ((u32)(pos) / VCHIQ_SLOT_SIZE) /* stream position -> its slot's slot_queue[] entry */
#define VCHIQ_MAX_SLOTS 128u                                 /* slot indexes the format can name (slot_info[]) */
#define VCHIQ_MAX_SLOTS_PER_SIDE 64u                         /* entries of a side's slot_queue[] */
#define VCHIQ_SLOT_QUEUE_MASK (VCHIQ_MAX_SLOTS_PER_SIDE - 1u) /* slot_queue[] index wrap */
#define VCHIQ_MAX_SERVICES 4096u                             /* port numbers are 12 bits: 0..4095 */
#define VCHIQ_PORT_FREE 0x1000u                              /* "no port": the remote port before OPENACK */
#define VCHIQ_NUM_CURRENT_BULKS 32u                          /* named for bulks, but only sizes the fragment pool */
#define VCHIQ_NUM_SERVICE_BULKS 4u                           /* outstanding bulks per service and direction */
#define VCHIQ_MAX_FRAGMENTS (2u * VCHIQ_NUM_CURRENT_BULKS)  /* fragment slots in the pool */
#define VCHIQ_CACHE_LINE_SIZE 64u                            /* A72 and A53 (bcm2711, bcm2836/7): a receive's unaligned ends go by fragment */
#define VCHIQ_FRAGMENT_SIZE (2u * VCHIQ_CACHE_LINE_SIZE)    /* one fragment slot: head line + tail line */
#define VCHIQ_DEBUG_ENTRIES 11u /* the layout the firmware expects (VCHIQ_ENABLE_DEBUG=1) */

#define VCHIQ_MAGIC 0x56434849u /* 'VCHI': slot_zero.magic */
#define VCHIQ_VERSION 8         /* the VCHIQ protocol version we speak (slot zero) ... */
#define VCHIQ_VERSION_MIN 3     /* ... and the oldest we accept from the VPU */

/* One slot for slot zero, 32 data slots per side, the fragment pool after the slots. */
#define VCHIQ_SLOT_ZERO_SLOTS 1u                                          /* struct vchiq_slot_zero fits one slot */
#define VCHIQ_SLOTS_PER_SIDE 32u                                          /* data slots the VPU sends from, and as many for us */
#define VCHIQ_TOTAL_SLOTS (VCHIQ_SLOT_ZERO_SLOTS + 2u * VCHIQ_SLOTS_PER_SIDE) /* slot zero + both sides */
#define VCHIQ_SLOT_MEM_SIZE (VCHIQ_TOTAL_SLOTS * VCHIQ_SLOT_SIZE)        /* 260 KiB */
#define VCHIQ_FRAG_MEM_SIZE (VCHIQ_MAX_FRAGMENTS * VCHIQ_FRAGMENT_SIZE)  /* 8 KiB */

/* Mailbox property tag that hands the slot area (VPU bus address, from /soc dma-ranges) to the firmware. */
#define VCHIQ_FIRMWARE_TAG_INIT 0x00048010u /* value in: slot zero's bus address; out: 0 on success */

/*
 * Register offsets in the doorbell block (the device-tree node whose compatible
 * names vchiq, e.g. "brcm,bcm2711-vchiq" at 0x7e00b840; base and interrupt come
 * from its reg and interrupts).  Reading BELL0 acknowledges; bit 2 set means the
 * VPU rang us.  Writing anything to BELL2 rings the VPU.
 */
#define VCHIQ_BELL0 0x00u         /* our doorbell: read to acknowledge */
#define VCHIQ_BELL2 0x08u         /* the VPU's doorbell: write to ring */
#define VCHIQ_BELL0_ACTIVE (1u << 2) /* BELL0: the VPU rang */

#define VCHIQ_MAKE_FOURCC(a, b, c, d) (((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | (u32)(d))

/* Message types (msgid bits 24..31), with what follows the header. */
enum vchiq_msg_type
{
	VCHIQ_MSG_PADDING = 0,            /* fills the rest of a slot; no payload, skipped */
	VCHIQ_MSG_CONNECT = 1,            /* the link is up; sent once by each side */
	VCHIQ_MSG_OPEN = 2,               /* (src, -): struct vchiq_open_payload */
	VCHIQ_MSG_OPENACK = 3,            /* (src, dst): struct vchiq_openack_payload */
	VCHIQ_MSG_CLOSE = 4,              /* (src, dst): close request, or its acknowledgement */
	VCHIQ_MSG_DATA = 5,               /* (src, dst): a service message */
	VCHIQ_MSG_BULK_RX = 6,            /* (src, dst): struct vchiq_bulk_payload; sender receives */
	VCHIQ_MSG_BULK_TX = 7,            /* (src, dst): struct vchiq_bulk_payload; sender transmits */
	VCHIQ_MSG_BULK_RX_DONE = 8,       /* (src, dst): __le32 actual bytes, or VCHIQ_BULK_ACTUAL_ABORTED */
	VCHIQ_MSG_BULK_TX_DONE = 9,       /* (src, dst): the same */
	VCHIQ_MSG_PAUSE = 10,             /* link suspend handshake; not used here */
	VCHIQ_MSG_RESUME = 11,            /* its end; not used here */
	VCHIQ_MSG_REMOTE_USE = 12,        /* the peer wants us kept awake (keep-alive count); ignored */
	VCHIQ_MSG_REMOTE_RELEASE = 13,    /* the peer drops that wish; ignored */
	VCHIQ_MSG_REMOTE_USE_ACTIVE = 14, /* the peer is active; ignored */
};

/* msgid: type in bits 24..31, source port in 12..23, destination port in 0..11. */
#define VCHIQ_MAKE_MSG(type, srcport, dstport) (((u32)(type) << 24) | ((u32)(srcport) << 12) | (u32)(dstport))
#define VCHIQ_MSG_TYPE(msgid) ((u32)(msgid) >> 24)
#define VCHIQ_MSG_SRCPORT(msgid) (((u32)(msgid) >> 12) & 0xfffu)
#define VCHIQ_MSG_DSTPORT(msgid) ((u32)(msgid) & 0xfffu)

#define VCHIQ_MSGID_PADDING VCHIQ_MAKE_MSG(VCHIQ_MSG_PADDING, 0, 0)
#define VCHIQ_MSGID_CLAIMED 0x40000000u /* set by the receiver in the sender's slot while it holds the message */

/* Header-aligned stride of a message of @size payload bytes. */
#define VCHIQ_MSG_STRIDE(size) (((u32)(size) + 8u + 7u) & ~7u) /* header + payload, rounded to 8 */
#define VCHIQ_MAX_MSG_SIZE (VCHIQ_SLOT_SIZE - 8u)               /* payload of a message that fills a slot */

/* The receiver writes armed (before it waits), the sender writes fired. */
struct vchiq_remote_event
{
	__le32 armed;  /* the receiver waits and wants the doorbell */
	__le32 fired;  /* the event happened; the receiver clears it when it looks */
	__le32 unused; /* padding */
};

/*
 * One side's state in slot zero; "owner" is the side it describes.  Both sides
 * write into it:
 *   owner: initialised, tx_pos, debug[], the armed word of each event (and
 *          clears fired), slot_queue[] and slot_queue_recycle at init;
 *   peer:  the fired word of each event, recycled slots appended to
 *          slot_queue[], slot_queue_recycle advanced past them;
 *   host:  slot_first, slot_last and slot_sync of both sides, at init.
 */
struct vchiq_shared_state
{
	__le32 initialised;                      /* non-zero once the owner has filled this in */
	__le32 slot_first;                       /* first and last (inclusive) slot the owner sends from */
	__le32 slot_last;
	__le32 slot_sync;                        /* the owner's synchronous-message slot */
	struct vchiq_remote_event trigger;       /* peer to owner: "a message is waiting, run your slot handler" */
	__le32 tx_pos;                           /* byte position of the owner's next message */
	struct vchiq_remote_event recycle;       /* peer to owner: "a slot of yours came back" */
	__le32 slot_queue_recycle;               /* where the peer writes the next recycled slot in slot_queue[] */
	struct vchiq_remote_event sync_trigger;  /* peer to owner: "a synchronous message is in my sync slot" */
	struct vchiq_remote_event sync_release;  /* peer to owner: "your sync slot is free again" */
	__le32 slot_queue[VCHIQ_MAX_SLOTS_PER_SIDE]; /* circular list of slot indexes: owner's free slots, peer appends recycled ones */
	__le32 debug[VCHIQ_DEBUG_ENTRIES];       /* the owner's debug counters */
};

/* Two counters instead of one, so claimer and releaser never write the same word. */
struct vchiq_slot_info
{
	__le16 use_count;     /* claims: 1 while the reader is in the slot, +1 per message held */
	__le16 release_count; /* claims given back; equal to use_count = the slot can be recycled */
};

/*
 * The memory shared between the host and the VPU.  The host allocates it, the
 * VPU is the master (it owns the DMA engine for bulk transfers), the host is
 * the slave.  Data slots follow slot zero at index * VCHIQ_SLOT_SIZE.
 */
struct vchiq_slot_zero
{
	__le32 magic;              /* VCHIQ_MAGIC */
	__le16 version;            /* VCHIQ_VERSION ... */
	__le16 version_min;        /* ... VCHIQ_VERSION_MIN */
	__le32 slot_zero_size;     /* sizeof(struct vchiq_slot_zero) */
	__le32 slot_size;          /* VCHIQ_SLOT_SIZE */
	__le32 max_slots;          /* VCHIQ_MAX_SLOTS */
	__le32 max_slots_per_side; /* VCHIQ_MAX_SLOTS_PER_SIDE */
	__le32 platform_data[2];   /* [0] bus address of the fragment pool, [1] fragment count */
	struct vchiq_shared_state master; /* the VPU's side */
	struct vchiq_shared_state slave;  /* ours */
	struct vchiq_slot_info slots[VCHIQ_MAX_SLOTS]; /* for the VPU's bookkeeping; the host keeps its own */
};

/* A message in a slot, 8-byte aligned; the next one follows at VCHIQ_MSG_STRIDE(size). */
struct vchiq_header
{
	__le32 msgid; /* VCHIQ_MAKE_MSG(); VCHIQ_MSGID_CLAIMED or'd in while the receiver holds it */
	__le32 size;  /* payload bytes */
	u8 data[];
};

struct vchiq_open_payload
{
	__le32 fourcc;      /* the service */
	__le32 client_id;   /* the opener's id, passed to the server; we send 1 */
	__le16 version;     /* the service protocol version the opener speaks ... */
	__le16 version_min; /* ... and the oldest it can still speak */
};

struct vchiq_openack_payload
{
	__le16 version; /* the version the server speaks */
};

/*
 * Bulk transfers: the host describes the buffer with a page list in VC-reachable
 * memory and sends its bus address; the VPU's DMA engine moves the bytes.  Each
 * addrs[] entry is a run of contiguous 4 KiB pages:
 *   BCM2711: a 36-bit physical page address, bits 8..31 = address bits 12..35,
 *            bits 0..7 = page count - 1 (up to 256 pages);
 *   BCM2836/7: a 32-bit bus address (through the 0xC0000000 alias), bits
 *            12..31 = the page, bits 0..11 = page count - 1 (up to 4096 pages).
 */
#define VCHIQ_PAGELIST_WRITE 0u                /* host to VPU (bulk transmit) */
#define VCHIQ_PAGELIST_READ 1u                 /* VPU to host (bulk receive) */
#define VCHIQ_PAGELIST_READ_WITH_FRAGMENTS 2u  /* + fragment slot index: the VPU writes the partial head and tail lines there */
#define VCHIQ_PAGELIST_PAGES_PER_ENTRY_36 256u  /* pages one 36-bit addrs[] entry can cover */
#define VCHIQ_PAGELIST_PAGES_PER_ENTRY_32 4096u /* pages one 32-bit addrs[] entry can cover */
#define VCHIQ_BULK_ACTUAL_ABORTED (-1)         /* BULK_*_DONE: the transfer was aborted */

struct vchiq_pagelist
{
	__le32 length; /* bytes to move */
	__le16 type;   /* VCHIQ_PAGELIST_* */
	__le16 offset; /* of the data within its first page */
	__le32 addrs[]; /* runs of contiguous pages, encoded as above */
};

struct vchiq_bulk_payload
{
	__le32 pagelist; /* VC bus address of the page list */
	__le32 size;     /* bytes to move */
};

_Static_assert(sizeof(struct vchiq_remote_event) == 12, "remote_event layout");
_Static_assert(sizeof(struct vchiq_shared_state) == 372, "shared_state layout");
_Static_assert(sizeof(struct vchiq_slot_zero) == 1288, "slot_zero layout");
_Static_assert(sizeof(struct vchiq_header) == 8, "header layout");
_Static_assert(sizeof(struct vchiq_open_payload) == 12, "open payload layout");
_Static_assert(sizeof(struct vchiq_pagelist) == 8, "pagelist layout");

#endif /* VCHIQ_PROTOCOL_H */
