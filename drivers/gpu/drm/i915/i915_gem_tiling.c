/*
 * Copyright © 2008 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Eric Anholt <eric@anholt.net>
 *
 */
#include <linux/acpi.h>

#include "drmP.h"
#include "drm.h"
#include "i915_drm.h"
#include "i915_drv.h"

/** @file i915_gem_tiling.c
 *
 * Support for managing tiling state of buffer objects.
 *
 * The idea behind tiling is to increase cache hit rates by rearranging
 * pixel data so that a group of pixel accesses are in the same cacheline.
 * Performance improvement from doing this on the back/depth buffer are on
 * the order of 30%.
 *
 * Intel architectures make this somewhat more complicated, though, by
 * adjustments made to addressing of data when the memory is in interleaved
 * mode (matched pairs of DIMMS) to improve memory bandwidth.
 * For interleaved memory, the CPU sends every sequential 64 bytes
 * to an alternate memory channel so it can get the bandwidth from both.
 *
 * The GPU also rearranges its accesses for increased bandwidth to interleaved
 * memory, and it matches what the CPU does for non-tiled.  However, when tiled
 * it does it a little differently, since one walks addresses not just in the
 * X direction but also Y.  So, along with alternating channels when bit
 * 6 of the address flips, it also alternates when other bits flip --  Bits 9
 * (every 512 bytes, an X tile scanline) and 10 (every two X tile scanlines)
 * are common to both the 915 and 965-class hardware.
 *
 * The CPU also sometimes XORs in higher bits as well, to improve
 * bandwidth doing strided access like we do so frequently in graphics.  This
 * is called "Channel XOR Randomization" in the MCH documentation.  The result
 * is that the CPU is XORing in either bit 11 or bit 17 to bit 6 of its address
 * decode.
 *
 * All of this bit 6 XORing has an effect on our memory management,
 * as we need to make sure that the 3d driver can correctly address object
 * contents.
 *
 * If we don't have interleaved memory, all tiling is safe and no swizzling is
 * required.
 *
 * When bit 17 is XORed in, we simply refuse to tile at all.  Bit
 * 17 is not just a page offset, so as we page an objet out and back in,
 * individual pages in it will have different bit 17 addresses, resulting in
 * each 64 bytes being swapped with its neighbor!
 *
 * Otherwise, if interleaved, we have to tell the 3d driver what the address
 * swizzling it needs to do is, since it's writing with the CPU to the pages
 * (bit 6 and potentially bit 11 XORed in), and the GPU is reading from the
 * pages (bit 6, 9, and 10 XORed in), resulting in a cumulative bit swizzling
 * required by the CPU of XORing in bit 6, 9, 10, and potentially 11, in order
 * to match what the GPU expects.
 */

#define MCHBAR_I915 0x44
#define MCHBAR_I965 0x48
#define MCHBAR_SIZE (4*4096)

#define DEVEN_REG 0x54
#define   DEVEN_MCHBAR_EN (1 << 28)

/*
 * ACPI resource checking fun.  So the MCHBAR has *probably* been set
 * up by the BIOS since drivers need to poke at it, but out of paranoia
 * or whatever, many BIOSes disable the MCHBAR at boot.  So we check
 * to make sure any existing address is reserved before using it.  If
 * we can't find a match or there is no address, allocate some new PCI
 * space for it, and then enable it.  And of course 915 has to be different
 * and put its enable bit somewhere else...
 */
static acpi_status __init check_mch_resource(struct acpi_resource *res,
					     void *data)
{
	struct resource *mch_res = data;
	struct acpi_resource_address64 address;
	acpi_status status;

	if (res->type == ACPI_RESOURCE_TYPE_FIXED_MEMORY32) {
		struct acpi_resource_fixed_memory32 *fixmem32 =
			&res->data.fixed_memory32;
		if (!fixmem32)
			return AE_OK;

		if ((mch_res->start >= fixmem32->address) &&
		    (mch_res->end < (fixmem32->address +
				      fixmem32->address_length))) {
			mch_res->flags = 1;
			return AE_CTRL_TERMINATE;
		}
	}
	if ((res->type != ACPI_RESOURCE_TYPE_ADDRESS32) &&
	    (res->type != ACPI_RESOURCE_TYPE_ADDRESS64))
		return AE_OK;

	status = acpi_resource_to_address64(res, &address);
	if (ACPI_FAILURE(status) ||
	   (address.address_length <= 0) ||
	   (address.resource_type != ACPI_MEMORY_RANGE))
		return AE_OK;

	if ((mch_res->start >= address.minimum) &&
	    (mch_res->end < (address.minimum + address.address_length))) {
		mch_res->flags = 1;
		return AE_CTRL_TERMINATE;
	}
	return AE_OK;
}

static acpi_status __init find_mboard_resource(acpi_handle handle, u32 lvl,
					       void *context, void **rv)
{
	struct resource *mch_res = context;

	acpi_walk_resources(handle, METHOD_NAME__CRS,
			    check_mch_resource, context);

	if (mch_res->flags)
		return AE_CTRL_TERMINATE;

	return AE_OK;
}

static int __init is_acpi_reserved(u64 start, u64 end, unsigned not_used)
{
	struct resource mch_res;

	mch_res.start = start;
	mch_res.end = end;
	mch_res.flags = 0;

	acpi_get_devices("PNP0C01", find_mboard_resource, &mch_res, NULL);

	if (!mch_res.flags)
		acpi_get_devices("PNP0C02", find_mboard_resource, &mch_res,
				 NULL);

	return mch_res.flags;
}

/* Allocate space for the MCH regs if needed, return nonzero on error */
static int
intel_alloc_mchbar_resource(struct drm_device *dev)
{
	struct pci_dev *bridge_dev;
	drm_i915_private_t *dev_priv = dev->dev_private;
	int reg = IS_I965G(dev) ? MCHBAR_I965 : MCHBAR_I915;
	u32 temp_lo, temp_hi = 0;
	u64 mchbar_addr;
	int ret;

	bridge_dev = pci_get_bus_and_slot(0, PCI_DEVFN(0,0));
	if (!bridge_dev) {
		DRM_DEBUG("no bridge dev?!\n");
		return -ENODEV;
	}

	if (IS_I965G(dev))
		pci_read_config_dword(bridge_dev, reg + 4, &temp_hi);
	pci_read_config_dword(bridge_dev, reg, &temp_lo);
	mchbar_addr = ((u64)temp_hi << 32) | temp_lo;

	/* If ACPI doesn't have it, assume we need to allocate it ourselves */
	if (mchbar_addr &&
	    is_acpi_reserved(mchbar_addr, mchbar_addr + MCHBAR_SIZE, 0))
		return 0;

	/* Get some space for it */
	ret = pci_bus_alloc_resource(bridge_dev->bus, &dev_priv->mch_res,
				     MCHBAR_SIZE, MCHBAR_SIZE,
				     PCIBIOS_MIN_MEM,
				     0,   pcibios_align_resource,
				     bridge_dev);
	if (ret) {
		DRM_DEBUG("failed bus alloc: %d\n", ret);
		dev_priv->mch_res.start = 0;
		return ret;
	}

	if (IS_I965G(dev))
		pci_write_config_dword(bridge_dev, reg + 4,
				       upper_32_bits(dev_priv->mch_res.start));

	pci_write_config_dword(bridge_dev, reg,
			       lower_32_bits(dev_priv->mch_res.start));

	return 0;
}

/* Setup MCHBAR if possible, return true if we should disable it again */
static bool
intel_setup_mchbar(struct drm_device *dev)
{
	struct pci_dev *bridge_dev;
	int mchbar_reg = IS_I965G(dev) ? MCHBAR_I965 : MCHBAR_I915;
	u32 temp;
	bool need_disable = false, enabled;

	bridge_dev = pci_get_bus_and_slot(0, PCI_DEVFN(0,0));
	if (!bridge_dev) {
		DRM_DEBUG("no bridge dev?!\n");
		goto out;
	}

	if (IS_I915G(dev) || IS_I915GM(dev)) {
		pci_read_config_dword(bridge_dev, DEVEN_REG, &temp);
		enabled = !!(temp & DEVEN_MCHBAR_EN);
	} else {
		pci_read_config_dword(bridge_dev, mchbar_reg, &temp);
		enabled = temp & 1;
	}

	/* If it's already enabled, don't have to do anything */
	if (enabled)
		goto out;

	if (intel_alloc_mchbar_resource(dev))
		goto out;

	need_disable = true;

	if (IS_I915G(dev) || IS_I915GM(dev))
		pci_write_config_dword(bridge_dev, DEVEN_REG,
				       temp | DEVEN_MCHBAR_EN);
	else
		pci_write_config_dword(bridge_dev, mchbar_reg, temp | 1);

out:
	return need_disable;
}

static void
intel_teardown_mchbar(struct drm_device *dev, bool disable)
{
	drm_i915_private_t *dev_priv = dev->dev_private;
	struct pci_dev *bridge_dev;
	int mchbar_reg = IS_I965G(dev) ? MCHBAR_I965 : MCHBAR_I915;
	u32 temp;

	bridge_dev = pci_get_bus_and_slot(0, PCI_DEVFN(0,0));
	if (!bridge_dev) {
		DRM_DEBUG("no bridge dev?!\n");
		return;
	}

	if (disable) {
		if (IS_I915G(dev) || IS_I915GM(dev)) {
			pci_read_config_dword(bridge_dev, DEVEN_REG, &temp);
			temp &= ~DEVEN_MCHBAR_EN;
			pci_write_config_dword(bridge_dev, DEVEN_REG, temp);
		} else {
			pci_read_config_dword(bridge_dev, mchbar_reg, &temp);
			temp &= ~1;
			pci_write_config_dword(bridge_dev, mchbar_reg, temp);
		}
	}

	if (dev_priv->mch_res.start)
		release_resource(&dev_priv->mch_res);
}

/**
 * Detects bit 6 swizzling of address lookup between IGD access and CPU
 * access through main memory.
 */
void
i915_gem_detect_bit_6_swizzle(struct drm_device *dev)
{
	drm_i915_private_t *dev_priv = dev->dev_private;
	uint32_t swizzle_x = I915_BIT_6_SWIZZLE_UNKNOWN;
	uint32_t swizzle_y = I915_BIT_6_SWIZZLE_UNKNOWN;
	bool need_disable;

	if (!IS_I9XX(dev)) {
		/* As far as we know, the 865 doesn't have these bit 6
		 * swizzling issues.
		 */
		swizzle_x = I915_BIT_6_SWIZZLE_NONE;
		swizzle_y = I915_BIT_6_SWIZZLE_NONE;
	} else if ((!IS_I965G(dev) && !IS_G33(dev)) || IS_I965GM(dev) ||
		   IS_GM45(dev)) {
		uint32_t dcc;

		/* Try to make sure MCHBAR is enabled before poking at it */
		need_disable = intel_setup_mchbar(dev);

		/* On 915-945 and GM965, channel interleave by the CPU is
		 * determined by DCC.  The CPU will alternate based on bit 6
		 * in interleaved mode, and the GPU will then also alternate
		 * on bit 6, 9, and 10 for X, but the CPU may also optionally
		 * alternate based on bit 17 (XOR not disabled and XOR
		 * bit == 17).
		 */
		dcc = I915_READ(DCC);
		switch (dcc & DCC_ADDRESSING_MODE_MASK) {
		case DCC_ADDRESSING_MODE_SINGLE_CHANNEL:
		case DCC_ADDRESSING_MODE_DUAL_CHANNEL_ASYMMETRIC:
			swizzle_x = I915_BIT_6_SWIZZLE_NONE;
			swizzle_y = I915_BIT_6_SWIZZLE_NONE;
			break;
		case DCC_ADDRESSING_MODE_DUAL_CHANNEL_INTERLEAVED:
			if (IS_I915G(dev) || IS_I915GM(dev) ||
			    dcc & DCC_CHANNEL_XOR_DISABLE) {
				swizzle_x = I915_BIT_6_SWIZZLE_9_10;
				swizzle_y = I915_BIT_6_SWIZZLE_9;
			} else if ((IS_I965GM(dev) || IS_GM45(dev)) &&
				   (dcc & DCC_CHANNEL_XOR_BIT_17) == 0) {
				/* GM965/GM45 does either bit 11 or bit 17
				 * swizzling.
				 */
				swizzle_x = I915_BIT_6_SWIZZLE_9_10_11;
				swizzle_y = I915_BIT_6_SWIZZLE_9_11;
			} else {
				/* Bit 17 or perhaps other swizzling */
				swizzle_x = I915_BIT_6_SWIZZLE_UNKNOWN;
				swizzle_y = I915_BIT_6_SWIZZLE_UNKNOWN;
			}
			break;
		}
		if (dcc == 0xffffffff) {
			swizzle_x = I915_BIT_6_SWIZZLE_UNKNOWN;
			swizzle_y = I915_BIT_6_SWIZZLE_UNKNOWN;
		}

		intel_teardown_mchbar(dev, need_disable);
	} else {
		/* The 965, G33, and newer, have a very flexible memory
		 * configuration.  It will enable dual-channel mode
		 * (interleaving) on as much memory as it can, and the GPU
		 * will additionally sometimes enable different bit 6
		 * swizzling for tiled objects from the CPU.
		 *
		 * Here's what I found on the G965:
		 *    slot fill         memory size  swizzling
		 * 0A   0B   1A   1B    1-ch   2-ch
		 * 512  0    0    0     512    0     O
		 * 512  0    512  0     16     1008  X
		 * 512  0    0    512   16     1008  X
		 * 0    512  0    512   16     1008  X
		 * 1024 1024 1024 0     2048   1024  O
		 *
		 * We could probably detect this based on either the DRB
		 * matching, which was the case for the swizzling required in
		 * the table above, or from the 1-ch value being less than
		 * the minimum size of a rank.
		 */
		if (I915_READ16(C0DRB3) != I915_READ16(C1DRB3)) {
			swizzle_x = I915_BIT_6_SWIZZLE_NONE;
			swizzle_y = I915_BIT_6_SWIZZLE_NONE;
		} else {
			swizzle_x = I915_BIT_6_SWIZZLE_9_10;
			swizzle_y = I915_BIT_6_SWIZZLE_9;
		}
	}

	dev_priv->mm.bit_6_swizzle_x = swizzle_x;
	dev_priv->mm.bit_6_swizzle_y = swizzle_y;
}

/**
 * Sets the tiling mode of an object, returning the required swizzling of
 * bit 6 of addresses in the object.
 */
int
i915_gem_set_tiling(struct drm_device *dev, void *data,
		   struct drm_file *file_priv)
{
	struct drm_i915_gem_set_tiling *args = data;
	drm_i915_private_t *dev_priv = dev->dev_private;
	struct drm_gem_object *obj;
	struct drm_i915_gem_object *obj_priv;

	obj = drm_gem_object_lookup(dev, file_priv, args->handle);
	if (obj == NULL)
		return -EINVAL;
	obj_priv = obj->driver_private;

	mutex_lock(&dev->struct_mutex);

	if (args->tiling_mode == I915_TILING_NONE) {
		obj_priv->tiling_mode = I915_TILING_NONE;
		args->swizzle_mode = I915_BIT_6_SWIZZLE_NONE;
	} else {
		if (args->tiling_mode == I915_TILING_X)
			args->swizzle_mode = dev_priv->mm.bit_6_swizzle_x;
		else
			args->swizzle_mode = dev_priv->mm.bit_6_swizzle_y;
		/* If we can't handle the swizzling, make it untiled. */
		if (args->swizzle_mode == I915_BIT_6_SWIZZLE_UNKNOWN) {
			args->tiling_mode = I915_TILING_NONE;
			args->swizzle_mode = I915_BIT_6_SWIZZLE_NONE;
		}
	}
	obj_priv->tiling_mode = args->tiling_mode;

	mutex_unlock(&dev->struct_mutex);

	drm_gem_object_unreference(obj);

	return 0;
}

/**
 * Returns the current tiling mode and required bit 6 swizzling for the object.
 */
int
i915_gem_get_tiling(struct drm_device *dev, void *data,
		   struct drm_file *file_priv)
{
	struct drm_i915_gem_get_tiling *args = data;
	drm_i915_private_t *dev_priv = dev->dev_private;
	struct drm_gem_object *obj;
	struct drm_i915_gem_object *obj_priv;

	obj = drm_gem_object_lookup(dev, file_priv, args->handle);
	if (obj == NULL)
		return -EINVAL;
	obj_priv = obj->driver_private;

	mutex_lock(&dev->struct_mutex);

	args->tiling_mode = obj_priv->tiling_mode;
	switch (obj_priv->tiling_mode) {
	case I915_TILING_X:
		args->swizzle_mode = dev_priv->mm.bit_6_swizzle_x;
		break;
	case I915_TILING_Y:
		args->swizzle_mode = dev_priv->mm.bit_6_swizzle_y;
		break;
	case I915_TILING_NONE:
		args->swizzle_mode = I915_BIT_6_SWIZZLE_NONE;
		break;
	default:
		DRM_ERROR("unknown tiling mode\n");
	}

	mutex_unlock(&dev->struct_mutex);

	drm_gem_object_unreference(obj);

	return 0;
}
