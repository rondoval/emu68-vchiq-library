/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * devtree_parse.c - what the device tree says about the VCHIQ hardware.
 *
 *   &vchiq (/__symbols__)      the doorbell block: reg, compatible (the SoC),
 *                              interrupts (a GIC SPI on the BCM2711 only)
 *   /soc dma-ranges            the window's address on the VPU's side; on the
 *                              BCM2836/7 also the bulk buffers' (32-bit page lists)
 *   (the window itself is a block of cma.resource, taken in vchiq_link.c:
 *    here it only gets its address on the VPU's side)
 *   /emu68/vc4-mem             the RTG card's VRAM (optional): mapped
 *                              write-through, so bulk buffers there need no
 *                              cache maintenance
 */
#include <device.h> /* first: __NOLIBBASE__ for the proto headers below */

#ifdef __INTELLISENSE__
#include <clib/devicetree_protos.h>
#else
#include <proto/devicetree.h>
#endif

#include <devtree.h>
#include <errors.h>

/* The doorbell block: the node labelled "vchiq" (the label overlays use; there is no alias). */
static APTR find_vchiq_node(APTR DeviceTreeBase, struct vchiq_platform *platform)
{
	APTR node = DT_OpenSymbol(DeviceTreeBase, (CONST_STRPTR) "vchiq");
	CONST_STRPTR compatible = DT_GetPropValue(DT_FindProperty(node, (CONST_STRPTR) "compatible"));
	if (compatible != NULL && vchiq_get_platform((const char *)compatible, platform))
		return node;

	if (node != NULL)
	{
		Kprintf("[vchiq] %s: %s is %s, not a supported SoC\n", __func__, DT_GetKeyName(node),
		        compatible != NULL ? (const char *)compatible : "unnamed");
		DT_CloseKey(node);
	}
	return NULL;
}

/* Fills the hardware fields of @base (the window, taken already, gets its VPU address); 0, or ENODEV with the reason logged. */
u32 DevTreeParse(struct VCHIQDevice *base)
{
	struct ExecBase *SysBase = base->sysBase;
	APTR DeviceTreeBase = OpenResource((CONST_STRPTR) "devicetree.resource");
	if (DeviceTreeBase == NULL)
	{
		Kprintf("[vchiq] %s: no devicetree.resource\n", __func__);
		return ENODEV;
	}

	struct vchiq_platform *pf = &base->platform;
	APTR node = find_vchiq_node(DeviceTreeBase, pf);
	if (node == NULL)
	{
		Kprintf("[vchiq] %s: no vchiq node of a supported SoC\n", __func__);
		return ENODEV;
	}
	base->bell = DT_GetBaseAddressVirtual(DeviceTreeBase, node, 0);
	base->irq = pf->doorbell_gic ? DT_GetInterrupt(DeviceTreeBase, node, 0) : -1; /* -1: the device polls */
	/* the window and, for 32-bit page lists, every bulk buffer go through the same dma-ranges record */
	BOOL reach = DT_GetDmaWindow(DeviceTreeBase, node, base->window_base, base->window_size, &base->window_bus,
	                             &pf->data_cpu_start, &pf->data_cpu_end);
	DT_CloseKey(node);
	if (base->bell == NULL || (pf->doorbell_gic && base->irq < 0))
	{
		Kprintf("[vchiq] %s: the vchiq node has no usable reg or interrupts\n", __func__);
		return ENODEV;
	}
	if (!reach)
	{
		Kprintf("[vchiq] %s: the VPU cannot reach the window at %08lx\n", __func__, base->window_base);
		return ENODEV;
	}

	u32 vram_start, vram_size;
	if (DT_GetEmu68Range(DeviceTreeBase, (CONST_STRPTR) "vc4-mem", &vram_start, &vram_size))
	{
		base->vram_start = vram_start;
		base->vram_end = vram_start + vram_size;
	}
	return 0;
}
