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

#include <errno.h>
#include <fdt_support.h>
#include <fdtdec.h>
#include <init.h>
#include <vsprintf.h>
#include <asm/global_data.h>
#include <asm/system.h>
#include <linux/sizes.h>

DECLARE_GLOBAL_DATA_PTR;

/*
 * The Gemini PDA has 4 GiB of LPDDR3 starting at 0x40000000, so DRAM spans
 * 0x40000000-0x140000000. The MSDC (eMMC) controller on MT6797 uses 32-bit
 * DMA (the Linux mt6797 MSDC support leaves SUPPORT_64G off and does not
 * populate descriptor high-address fields), so every buffer U-Boot touches -
 * its own relocation target, malloc arena and MMC DMA descriptors - must live
 * below the 4 GiB line.
 *
 * On top of that LK reserves memory high in DRAM for the retained framebuffer
 * and its "mblock-*" regions, the exact addresses of which depend on the live
 * display configuration and are not known without hardware. We therefore cap
 * the usable RAM top well below those regions, at the 2 GiB physical line. The
 * low reserved-memory block (CCCI/ramoops/etc.) ends at 0x46400000, so this
 * leaves U-Boot a clean 0x46400000-0x80000000 window and keeps relocation,
 * malloc and DMA both below 4 GiB and clear of the LK framebuffer.
 *
 * TODO(hw-verify): once the live LK-patched FDT can be inspected, tighten this
 * to the lowest LK mblock/framebuffer reservation instead of the fixed 2 GiB.
 */
#define GEMINI_RAM_TOP_MAX	0x80000000ULL

int dram_init(void)
{
	return fdtdec_setup_mem_size_base();
}

int dram_init_banksize(void)
{
	return fdtdec_setup_memory_banksize();
}

phys_addr_t board_get_usable_ram_top(phys_size_t total_size)
{
	phys_addr_t top = gd->ram_base + gd->ram_size;

	if (top > GEMINI_RAM_TOP_MAX)
		top = GEMINI_RAM_TOP_MAX;

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

	return 0;
}
