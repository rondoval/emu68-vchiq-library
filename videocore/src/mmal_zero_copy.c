/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * mmal_zero_copy.c - zero copy: the VPU working in the caller's memory
 * itself instead of a copy of its own.
 *
 * Without it every port buffer exists twice, ours and the firmware's copy in
 * gpu_mem, with a VPU DMA pass between them for each payload.  When a port
 * carries pictures, or a video decoder's packets of IMPORT_PACKET_MIN bytes
 * and up (port_worth_importing()), and every buffer attached to it qualifies,
 * the attach imports them through the SMEM service (smem_client.c) and sets
 * the port's ZERO_COPY; each buffer then travels by its import's handle with
 * no bulk, and the decoder, the ISP or the encoder reads and writes our
 * memory.  All or none: otherwise the port goes by bulk.
 *
 * A buffer qualifies inside the memory the VPU addresses (the reach
 * vchiq.device reports: the lowest GiB on a Pi 4), in Pi RAM or the VRAM
 * (never Chip RAM, whose addresses are Emu68's own Pi RAM) and, when the VPU
 * writes it, on whole cache lines from data to data + room: the invalidate
 * afterwards drops whole lines, so a line shared with the owner's other data
 * would lose that.  The VRAM is write-through and needs no cache work.
 *
 * The cache maintenance a device bulk would do is the client's here, by the
 * cache_ops.h contract: an input is cleaned out to RAM before the VPU reads
 * it (zero_copy_arm()); an output is cleaned and dropped whole before the
 * VPU writes it and dropped again when it is back (zero_copy_landed()), so
 * no line the prefetcher refilled hides what the VPU wrote.
 *
 * What zero copy saves is the VPU's: no copy in gpu_mem and no DMA pass.
 * The 68k saves nothing: bulks are zero copy on its side already.
 */
#include <byteorder.h>
#include <memory.h>

#include <videocore_private.h>
#include <mmal_internal.h>

#include <cache_ops.h> /* after proto/exec.h (videocore_private.h): its LVO fallback needs it */

#define IMPORT_PACKET_MIN (512u << 10) /* a video decoder's input packets from here on go by import */

/* ---- the reach ------------------------------------------------------------- */

/**
 * reach_set() - Take where the VPU reaches host memory from vchiq.device's information.
 * @base: the library base
 * @info: the device's information
 */
void reach_set(struct VideoCoreBase *base, const struct VchiqInfo *info)
{
	struct vc_reach *reach = &base->reach;
	reach->start = info->vi_ReachStart;
	reach->end = info->vi_ReachEnd;
	reach->bus = info->vi_ReachBus;
	reach->uncached_start = info->vi_UncachedStart;
	reach->uncached_end = info->vi_UncachedEnd;
	reach->known = reach->end > reach->start;
	KprintfT("[videocore] the VPU reaches %08lx-%08lx at %08lx; uncached %08lx-%08lx\n", reach->start, reach->end, reach->bus,
	         reach->uncached_start, reach->uncached_end);
}

/**
 * range_inside() - Whether a range lies inside another.
 * @start:  the range
 * @extent: its bytes
 * @lo:     the other, [lo, hi)
 * @hi:
 *
 * Return: TRUE if every byte of the range is inside.
 */
static BOOL range_inside(u32 start, u32 extent, u32 lo, u32 hi)
{
	return hi > lo && start >= lo && start < hi && extent <= hi - start;
}

/**
 * reach_uncached() - Whether a range needs no cache maintenance.
 * @base: the library base
 * @data: the range
 * @size: its bytes
 *
 * Return: TRUE inside the RTG card's VRAM, which Emu68 maps write-through
 * (both CPUs it runs on treat that as non-cacheable).
 */
BOOL reach_uncached(const struct VideoCoreBase *base, APTR data, u32 size)
{
	const struct vc_reach *reach = &base->reach;
	return range_inside((u32)data, size, reach->uncached_start, reach->uncached_end);
}

/**
 * reach_bus() - An address as the VPU addresses it.
 * @base: the library base, the reach known
 * @data: the address, inside the reach
 *
 * Return: the bus address.
 */
u32 reach_bus(const struct VideoCoreBase *base, APTR data)
{
	return (u32)data + (base->reach.bus - base->reach.start);
}

/**
 * zero_copy_qualifies() - Whether the VPU could work in a buffer over some memory itself.
 * @base:    the library base, mmal_lock held
 * @data:    the buffer
 * @size:    the bytes the port uses of it
 * @room:    the bytes from @data its owner holds (at least @size; 0 for @size)
 * @written: the VPU writes it (an output port's)
 *
 * The one rule: the attach (zero_copy_choose()) and the image calls' budget
 * (mmal_importable()) ask it alike.
 *
 * Return: TRUE if an import of it would serve the port.
 */
static BOOL zero_copy_qualifies(struct VideoCoreBase *base, APTR data, u32 size, u32 room, BOOL written)
{
	const struct vc_reach *reach = &base->reach;
	u32 start = (u32)data;
	u32 extent = umax(room, size);
	if (!base->smem.up || !reach->known || size == 0)
		return FALSE;
	if (!range_inside(start, extent, reach->start, reach->end))
		return FALSE;
	if (reach_uncached(base, data, extent))
		return TRUE;
	if (!dma_addr_reachable(&base->dma, data, extent))
		return FALSE;
	return !written || ((start | extent) & DMA_ALIGN_MIN_MASK) == 0;
}

/**
 * mmal_importable() - Whether a port's buffer over some memory would be imported, not sent by bulk.
 * @base:    the library base
 * @data:    the memory, or NULL for the backend's own (from its DMA pool)
 * @size:    its bytes
 * @written: the VPU writes it (an output port's)
 *
 * Return: TRUE if it would be imported while the service stays as it is.
 */
BOOL mmal_importable(struct VideoCoreBase *base, APTR data, u32 size, BOOL written)
{
	struct ExecBase *SysBase = base->SysBase;

	ObtainSemaphore(&base->mmal_lock);
	BOOL importable = data == NULL ? base->smem.up && base->reach.own_in_reach : zero_copy_qualifies(base, data, size, size, written);
	ReleaseSemaphore(&base->mmal_lock);
	return importable;
}

/* ---- the imports ----------------------------------------------------------- */

/**
 * port_worth_importing() - Whether a port's buffers are worth importing.
 * @port: the port, its format committed and its buffers attached
 *
 * Pictures are.  So is a video decoder's input of IMPORT_PACKET_MIN bytes a
 * packet and up, a camera's frames say: four of them in gpu_mem cost more
 * than the attach's round trip.  Other compressed packets are not: a small
 * one's copy in the VPU costs little, and an encoder's the VPU would write,
 * which a client's memory rarely allows.
 *
 * Return: TRUE for a raw video format, or for packets as above.
 */
static BOOL port_worth_importing(const struct mmal_port *port)
{
	static const u32 packed[] = {MMAL_ENCODING_H264, MMAL_ENCODING_MJPEG, MMAL_ENCODING_JPEG, MMAL_ENCODING_GIF,
	                             MMAL_ENCODING_PNG,  MMAL_ENCODING_PPM,   MMAL_ENCODING_TGA,  MMAL_ENCODING_BMP};
	u32 encoding = port->format.encoding;
	if (port->format.type != MMAL_ES_TYPE_VIDEO)
		return FALSE;
	for (u32 i = 0; i < ARRAY_SIZE(packed); i++)
		if (encoding == packed[i])
			return port->type == MMAL_PORT_TYPE_INPUT && (encoding == MMAL_ENCODING_H264 || encoding == MMAL_ENCODING_MJPEG) &&
			       port->buffer_size >= IMPORT_PACKET_MIN;
	return TRUE;
}

/**
 * imports_make() - Import a port's buffers into the VPU, all of them or none.
 * @base: the library base, mmal_lock held (released while waiting)
 * @port: a data port, its buffers attached
 *
 * Return: TRUE with every buffer's import set; FALSE with none.
 */
static BOOL imports_make(struct VideoCoreBase *base, struct mmal_port *port)
{
	BOOL written = port->type == MMAL_PORT_TYPE_OUTPUT;
	if ((port->type != MMAL_PORT_TYPE_INPUT && !written) || !port_worth_importing(port))
		return FALSE;
	struct smem_range ranges[CLIENT_PORT_BUFFERS_MAX];
	for (u32 i = 0; i < port->attached_count; i++)
	{
		struct mmal_buffer *buffer = port->buffers[i];
		if (!zero_copy_qualifies(base, buffer->data, buffer->alloc_size, buffer->room, written))
			return FALSE;
		ranges[i] = (struct smem_range){.data = buffer->data, .bus = reach_bus(base, buffer->data), .size = buffer->alloc_size};
	}
	u32 ids[CLIENT_PORT_BUFFERS_MAX];
	if (smem_import(base, ranges, port->attached_count, ids) != 0)
		return FALSE;
	for (u32 i = 0; i < port->attached_count; i++)
		port->buffers[i]->import = ids[i];
	return TRUE;
}

/**
 * imports_release() - Let go of the imports of a port's attached buffers.
 * @base: the library base, mmal_lock held
 * @port: the port
 *
 * Nothing waits: the memory is its owner's again after smem_released_wait().
 */
void imports_release(struct VideoCoreBase *base, struct mmal_port *port)
{
	for (u32 i = 0; i < port->attached_count; i++)
	{
		struct mmal_buffer *buffer = port->buffers[i];
		if (buffer->import != 0)
			smem_release(base, buffer->import);
		buffer->import = 0;
	}
}

/**
 * zero_copy_choose() - Run a port zero copy if its buffers can be imported, else by bulk.
 * @base: the library base, mmal_lock held once (released while waiting)
 * @port: the port, its buffers just attached, not enabled
 *
 * The port's ZERO_COPY is sent only when it changes (a kept component's
 * port holds its last use's).  A port that does not take it (a VPU status)
 * goes by bulk, its imports let go.
 *
 * Return: MMAL_SUCCESS, or the VPU's status or LINK_ERR_* if ZERO_COPY could
 * not be cleared (the VPU would take bulk buffers for handles).
 */
s32 zero_copy_choose(struct VideoCoreBase *base, struct mmal_port *port)
{
	BOOL want = imports_make(base, port);
	if (want == port->zero_copy)
		return MMAL_SUCCESS;
	u32 value = want ? 1u : 0u;
	s32 status = port_param_set(base, port, MMAL_PARAMETER_ZERO_COPY, &value, 1);
	if (status == MMAL_SUCCESS)
	{
		port->zero_copy = want;
		Kprintf("[videocore] %s port %ld: zero copy %s, %ld buffers of %ld bytes\n", port->component->name, (ULONG)port->index_all,
		        want ? "on" : "off", port->attached_count, port->buffer_size);
		return MMAL_SUCCESS;
	}
	if (!want)
		return status;
	imports_release(base, port); /* it does not take zero copy: by bulk */
	return MMAL_SUCCESS;
}

/* ---- the cache work -------------------------------------------------------- */

/**
 * zero_copy_arm() - The cache work before the VPU works in an imported buffer itself.
 * @base:   the library base
 * @buffer: the buffer, imported, about to be sent
 *
 * What vchiq.device does for a bulk: an input's payload is cleaned out to
 * RAM for the VPU to read; an output is cleaned and dropped whole, so that
 * no dirty line lands on what the VPU writes (cache_ops.h; not the
 * WriteToRAM promise: a picture placed inside a frame leaves the frame's
 * other bytes as they were).  The VRAM needs neither.  vchiq.device, which
 * the link needs, refuses to start where the range operations are missing.
 */
void zero_copy_arm(struct VideoCoreBase *base, const struct mmal_buffer *buffer)
{
	struct ExecBase *SysBase = base->SysBase;
	BOOL written = buffer->port->type == MMAL_PORT_TYPE_OUTPUT;
	u32 size = written ? buffer->alloc_size : buffer->length;
	if (size == 0 || reach_uncached(base, buffer->data, size))
		return;
	cache_pre_dma(buffer->data, size, written ? 0u : (ULONG)DMA_ReadFromRAM);
}

/**
 * zero_copy_landed() - An imported buffer is back: the cache work after the VPU worked in it.
 * @base:   the library base, mmal_lock held
 * @buffer: the buffer, its header taken (buffer_from_wire())
 * @offset: where in the buffer the VPU says the payload starts
 *
 * Over what the VPU wrote into an output, the lines the prefetcher may have
 * refilled since the arm are dropped; the buffer owns them all
 * (zero_copy_qualifies()).  The payload starts at the buffer's start: an
 * offset is logged, as the sessions read from the start.
 *
 * Return: TRUE with the payload in; FALSE if the VPU wrote past the buffer
 * (deliver it failed).
 */
BOOL zero_copy_landed(struct VideoCoreBase *base, struct mmal_buffer *buffer, u32 offset)
{
	struct ExecBase *SysBase = base->SysBase;
	u32 written = offset + buffer->length;
	if (offset != 0)
		Kprintf("[videocore] a zero-copy buffer back at offset %ld\n", offset);
	if (written > buffer->alloc_size)
	{
		Kprintf("[videocore] %ld bytes in a %ld-byte zero-copy buffer\n", written, buffer->alloc_size);
		return FALSE;
	}
	if (buffer->port->type == MMAL_PORT_TYPE_OUTPUT && written != 0 && !reach_uncached(base, buffer->data, written))
		cache_post_dma(buffer->data, written, 0);
	return TRUE;
}
