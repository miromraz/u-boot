// SPDX-License-Identifier: GPL-2.0
/*
 * Board support for the Planet Computers Gemini PDA (MediaTek MT6797).
 */

#include <cpu_func.h>
#include <env.h>
#include <init.h>
#include <mmc.h>
#include <vsprintf.h>
#include <asm/global_data.h>
#include <linux/libfdt.h>

DECLARE_GLOBAL_DATA_PTR;

#if IS_ENABLED(CONFIG_VIDEO_SIMPLE)
/*
 * LK retains its framebuffer in DRAM and records its base in the /chosen
 * videolfb atags of the control FDT. Overwrite the static simple-framebuffer
 * reg base with that runtime value before the video driver probes, so U-Boot
 * draws into the live buffer. This runs before console_init_r() probes video.
 */
static void gemini_fixup_fb_base(void)
{
	void *fdt = (void *)gd->fdt_blob;
	const fdt32_t *hi, *lo, *old;
	fdt32_t reg[4];
	unsigned long long base;
	int chosen, fb, len;

	chosen = fdt_path_offset(fdt, "/chosen");
	if (chosen < 0)
		return;

	lo = fdt_getprop(fdt, chosen, "atag,videolfb-fb_base_l", NULL);
	if (!lo)
		return;
	hi = fdt_getprop(fdt, chosen, "atag,videolfb-fb_base_h", NULL);
	base = ((unsigned long long)(hi ? fdt32_to_cpu(*hi) : 0) << 32) |
	       fdt32_to_cpu(*lo);

	fb = fdt_node_offset_by_compatible(fdt, -1, "simple-framebuffer");
	if (fb < 0)
		return;

	/* reg = <base-hi base-lo size-hi size-lo>: patch base, keep size. */
	old = fdt_getprop(fdt, fb, "reg", &len);
	if (!old || len < (int)sizeof(reg))
		return;

	reg[0] = cpu_to_fdt32(base >> 32);
	reg[1] = cpu_to_fdt32(base & 0xffffffff);
	reg[2] = old[2];
	reg[3] = old[3];
	fdt_setprop_inplace(fdt, fb, "reg", reg, sizeof(reg));
}

/*
 * LK leaves a second OVL layer enabled with per-pixel alpha, scanning out
 * the buffer that follows the first frame (fb + 0x908000 on this unit). It
 * sits above U-Boot's layer, so zero everything past the first frame,
 * making it transparent, before the video console starts.
 */
static void gemini_clear_upper_layers(void)
{
	const void *fdt = gd->fdt_blob;
	const fdt32_t *reg, *stride, *height;
	ulong base, size, frame;
	int fb;

	fb = fdt_node_offset_by_compatible(fdt, -1, "simple-framebuffer");
	if (fb < 0)
		return;

	reg = fdt_getprop(fdt, fb, "reg", NULL);
	stride = fdt_getprop(fdt, fb, "stride", NULL);
	height = fdt_getprop(fdt, fb, "height", NULL);
	if (!reg || !stride || !height)
		return;

	base = ((u64)fdt32_to_cpu(reg[0]) << 32) | fdt32_to_cpu(reg[1]);
	size = ((u64)fdt32_to_cpu(reg[2]) << 32) | fdt32_to_cpu(reg[3]);
	frame = (ulong)fdt32_to_cpu(*stride) * fdt32_to_cpu(*height);
	if (frame >= size)
		return;

	memset((void *)(base + frame), 0, size - frame);
	flush_dcache_range(base + frame, base + size);
}
#endif

int board_init(void)
{
#if IS_ENABLED(CONFIG_VIDEO_SIMPLE)
	gemini_fixup_fb_base();
	gemini_clear_upper_layers();
#endif
	return 0;
}

#if IS_ENABLED(CONFIG_BOARD_LATE_INIT)
/*
 * Give fastboot a stable identity. "board" backs `fastboot getvar product`
 * and serial# backs both `getvar serialno` and the USB iSerial string; without
 * them the host shows a blank serial and product reads "Board not set". Derive
 * serial# from the eMMC CID (device 0) so it is unique per unit and stable
 * across boots. Only set what is not already defined in the environment.
 */
int board_late_init(void)
{
	if (!env_get("board"))
		env_set("board", CONFIG_SYS_BOARD);

	if (!env_get("serial#")) {
		struct mmc *mmc = find_mmc_device(0);
		char serial[17];

		if (mmc && !mmc_init(mmc)) {
			snprintf(serial, sizeof(serial), "%08x%08x",
				 mmc->cid[2], mmc->cid[3]);
			env_set("serial#", serial);
		}
	}

	return 0;
}
#endif
