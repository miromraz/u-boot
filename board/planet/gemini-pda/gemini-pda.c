// SPDX-License-Identifier: GPL-2.0
/*
 * Board support for the Planet Computers Gemini PDA (MediaTek MT6797).
 */

#include <init.h>
#include <asm/global_data.h>
#include <linux/libfdt.h>

DECLARE_GLOBAL_DATA_PTR;

void gemini_crumb(u32 argb);	/* DEBUG */

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
#endif

int board_init(void)
{
	gemini_crumb(0xff00ff00);	/* green, DEBUG */
#if IS_ENABLED(CONFIG_VIDEO_SIMPLE)
	gemini_fixup_fb_base();
#endif
	return 0;
}

/* DEBUG: last breadcrumb before the console/menu. */
int board_late_init(void)
{
	gemini_crumb(0xff0000ff);	/* blue */
	return 0;
}
