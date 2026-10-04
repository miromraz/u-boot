// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6797 (Helio X20/X25/X27) SoC support.
 *
 * U-Boot runs as a payload chainloaded from the vendor LK bootloader on the
 * Planet Gemini PDA. LK gunzips U-Boot to 0x40080000, patches the DTB that is
 * appended to the boot image (memory, /chosen, CPU clock-frequency, mblock
 * reserved-memory) and jumps to U-Boot in EL2 with x0 pointing at that DTB.
 * We therefore use the LK-provided FDT as the control device tree.
 */

#include <dm.h>
#include <errno.h>
#include <fdt_simplefb.h>
#include <fdt_support.h>
#include <fdtdec.h>
#include <init.h>
#include <video.h>
#include <vsprintf.h>
#include <asm/global_data.h>
#include <asm/system.h>
#include <linux/libfdt.h>
#include <linux/sizes.h>

#include "ram_top.h"

DECLARE_GLOBAL_DATA_PTR;

/* Upper bound on reserved region bases we collect from the LK FDT. */
#define GEMINI_MAX_RSV		32

int dram_init(void)
{
	return fdtdec_setup_mem_size_base();
}

int dram_init_banksize(void)
{
	return fdtdec_setup_memory_banksize();
}

/* Base of LK's retained framebuffer, from the /chosen videolfb atags. */
static int gemini_fb_base(const void *fdt, unsigned long long *base)
{
	const fdt32_t *hi, *lo;
	int chosen;

	chosen = fdt_path_offset(fdt, "/chosen");
	if (chosen < 0)
		return -1;

	lo = fdt_getprop(fdt, chosen, "atag,videolfb-fb_base_l", NULL);
	if (!lo)
		return -1;
	hi = fdt_getprop(fdt, chosen, "atag,videolfb-fb_base_h", NULL);

	*base = ((unsigned long long)(hi ? fdt32_to_cpu(*hi) : 0) << 32) |
		fdt32_to_cpu(*lo);
	return 0;
}

/*
 * The Gemini PDA has up to 4 GiB of LPDDR3 from 0x40000000. The MSDC (eMMC)
 * DMA on MT6797 is 32-bit (the Linux driver leaves SUPPORT_64G off), so every
 * buffer U-Boot touches - its relocation target, malloc arena and MMC DMA
 * descriptors - must live below 4 GiB. LK also reserves memory high in DRAM
 * for the retained framebuffer and its "mblock-*" regions, whose addresses are
 * runtime dependent. Derive the usable top from the LK-patched control FDT:
 * start at min(ram end, 4 GiB) and drop below the lowest reserved region (see
 * ram_top.h), falling back to a safe fixed cap if the FDT data is missing.
 */
phys_addr_t board_get_usable_ram_top(phys_size_t total_size)
{
	unsigned long long starts[GEMINI_MAX_RSV];
	const void *fdt = gd->fdt_blob;
	unsigned long long top, fb;
	int n = 0, ok, rsv, node;

	top = gd->ram_base + gd->ram_size;

	if (fdt && !fdt_check_header(fdt)) {
		if (!gemini_fb_base(fdt, &fb) && n < GEMINI_MAX_RSV)
			starts[n++] = fb;

		rsv = fdt_path_offset(fdt, "/reserved-memory");
		if (rsv >= 0) {
			int ac = fdt_address_cells(fdt, rsv);
			int sc = fdt_size_cells(fdt, rsv);
			int stride = ac + sc;

			fdt_for_each_subnode(node, fdt, rsv) {
				const fdt32_t *reg;
				int len, i, c;

				reg = fdt_getprop(fdt, node, "reg", &len);
				if (!reg || ac < 1 || stride < 1)
					continue;

				len /= sizeof(fdt32_t);
				for (i = 0; i + stride <= len &&
					    n < GEMINI_MAX_RSV; i += stride) {
					unsigned long long s = 0;

					for (c = 0; c < ac; c++)
						s = (s << 32) |
						    fdt32_to_cpu(reg[i + c]);
					starts[n++] = s;
				}
			}
		}
	}

	top = gemini_cap_ram_top(top, starts, n, &ok);
	if (!ok)
		printf("gemini: no usable RAM top from LK FDT, capping at %#llx\n",
		       top);

	return top;
}

/*
 * Use the FDT handed over by the previous stage (LK) in x0 as the control DT.
 * Fall back to a built-in FDT only if LK did not provide a valid one.
 */
int board_fdt_blob_setup(void **fdtp)
{
	struct fdt_header *fdt;

	fdt = (struct fdt_header *)get_prev_bl_fdt_addr();
	if (fdt && !fdt_check_header(fdt)) {
		*fdtp = fdt;
		return 0;
	}

	if (!fdt_check_header(*fdtp))
		return -EEXIST;

	panic("No valid FDT from LK and no built-in FDT\n");
}

void reset_cpu(void)
{
	psci_system_reset();
}

int print_cpuinfo(void)
{
	printf("CPU:   MediaTek MT6797 (Helio X20/X25/X27)\n");
	return 0;
}

#if IS_ENABLED(CONFIG_FDT_SIMPLEFB)
/*
 * Hand the kernel an enabled simple-framebuffer node plus a reserved-memory
 * entry for the live LK framebuffer, so Linux keeps the console U-Boot drew on.
 */
static void gemini_setup_simplefb(void *blob)
{
	struct udevice *dev;
	struct video_uc_plat *plat;
	struct fdt_memory fb;

	if (!video_is_active())
		return;

	/* mainline mt6797 has no framebuffer node; add one (configured from
	 * the live fb base/size). fdt_simplefb_configure_node() sets it okay.
	 */
	if (fdt_node_offset_by_compatible(blob, -1, "simple-framebuffer") < 0)
		fdt_simplefb_add_node(blob);

	/*
	 * Reserve the real framebuffer memory. fdt_add_fb_mem_rsv() cannot: with
	 * VIDEO_SIMPLE U-Boot never allocates a buffer, so gd->video_bottom ==
	 * gd->video_top and it would reserve nothing.
	 */
	if (uclass_first_device_err(UCLASS_VIDEO, &dev))
		return;
	plat = dev_get_uclass_plat(dev);
	fb.start = plat->base;
	fb.end = plat->base + plat->size - 1;
	fdtdec_add_reserved_memory(blob, "framebuffer", &fb, NULL, 0, NULL,
				   FDTDEC_RESERVED_MEMORY_NO_MAP);
}
#endif

/*
 * The kernel loads its own DTB from the rootfs, so its reserved-memory does
 * not include the "mblock-*" regions that LK carved out of DRAM at runtime.
 * Copy them from the LK-provided control FDT into the kernel FDT so the kernel
 * does not reuse memory LK (or its secure world) still owns. U-Boot fixes up
 * /memory itself, and bootargs come from extlinux, so we touch neither here.
 */
int ft_board_setup(void *blob, struct bd_info *bd)
{
	const void *lk = gd->fdt_blob;
	int lk_rsv, dst_rsv, node;

	lk_rsv = fdt_path_offset(lk, "/reserved-memory");
	if (lk_rsv < 0)
		return 0;

	dst_rsv = fdt_path_offset(blob, "/reserved-memory");
	if (dst_rsv < 0) {
		dst_rsv = fdt_add_subnode(blob, 0, "reserved-memory");
		if (dst_rsv < 0)
			return dst_rsv;
		fdt_setprop_u32(blob, dst_rsv, "#address-cells", 2);
		fdt_setprop_u32(blob, dst_rsv, "#size-cells", 2);
		fdt_setprop_empty(blob, dst_rsv, "ranges");
	}

	fdt_for_each_subnode(node, lk, lk_rsv) {
		const char *name = fdt_get_name(lk, node, NULL);
		const void *reg;
		int len, dst;

		if (!name || strncmp(name, "mblock", 6))
			continue;

		reg = fdt_getprop(lk, node, "reg", &len);
		if (!reg)
			continue;

		dst = fdt_add_subnode(blob, dst_rsv, name);
		if (dst == -FDT_ERR_EXISTS)
			continue;
		if (dst < 0)
			return dst;

		fdt_setprop(blob, dst, "reg", reg, len);
		fdt_setprop_empty(blob, dst, "no-map");
	}

#if IS_ENABLED(CONFIG_FDT_SIMPLEFB)
	gemini_setup_simplefb(blob);
#endif

	return 0;
}
