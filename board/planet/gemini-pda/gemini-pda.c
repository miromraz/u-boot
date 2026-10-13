// SPDX-License-Identifier: GPL-2.0
/*
 * Board support for the Planet Computers Gemini PDA (MediaTek MT6797).
 */

#include <button.h>
#include <console.h>
#include <cpu_func.h>
#include <env.h>
#include <g_dnl.h>
#include <init.h>
#include <memalign.h>
#include <mmc.h>
#include <vsprintf.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <linux/delay.h>
#include <linux/libfdt.h>
/* mmc_switch() is only declared in the mmc driver's private header. */
#include "../../../drivers/mmc/mmc_private.h"

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

/*
 * LK hands control over with the display pipeline still running (DSI video
 * mode, OVL0 -> OVL0_2L -> OVL1_2L -> RDMA0 -> DSI0), OVL0 fetching the live
 * framebuffer through SMI/M4U. Mainline Linux power-cycles the MM power domain
 * early in boot (mtk-pm-domains cleanup) while that DMA is still in flight; the
 * orphaned SMI/M4U transactions then wedge every later display fetch, so
 * mediatek-drm never completes a frame. Stop the pipeline here, right before
 * the kernel jump, so no scanout DMA is outstanding when Linux resets MM:
 * disable the display mutex (no new frame starts), wait for the current frame
 * to drain, then turn the OVL and RDMA engines off.
 *
 * Skip it for the stock Android kernel, which takes over LK's live display and
 * must keep it scanning out ("boot_android" sets this env flag across booti).
 */
void board_quiesce_devices(void)
{
	/*
	 * The OS is often booted from inside a live fastboot gadget session
	 * (e.g. "fastboot oem run:booti ...", "bootflow scan", or the Boot
	 * Linux menu entry), so do_fastboot_usb() never returns and its
	 * g_dnl_unregister() teardown never runs. That leaves the MTU3 D+
	 * soft-connect asserted across the kernel jump: the host sees no
	 * disconnect, the kernel's gadget binds behind descriptors the host
	 * already enumerated, and the OS NCM/ADB gadget never appears until the
	 * cable is physically replugged. Release the download gadget here (the
	 * weak hook bootm runs right before every OS jump) so SOFT_CONN drops
	 * and the host re-enumerates the OS gadget cleanly. Safe no-op when no
	 * gadget is registered (usb_composite_unregister() ignores it), so this
	 * also covers the Android path. The mdelay lets the host register the
	 * electrical disconnect before the OS re-asserts the pull-up.
	 */
	if (IS_ENABLED(CONFIG_USB_GADGET_DOWNLOAD)) {
		g_dnl_unregister();
		mdelay(50);
	}

	if (env_get("gemini_android"))
		return;

	writel(0, (void __iomem *)0x1401f020);	/* DISP MUTEX0_EN = 0 */
	mdelay(50);
	writel(0, (void __iomem *)0x1400b00c);	/* OVL0 EN = 0 */
	writel(0, (void __iomem *)0x1400d00c);	/* OVL0_2L EN = 0 */
	writel(0, (void __iomem *)0x1400e00c);	/* OVL1_2L EN = 0 */
	writel(0, (void __iomem *)0x1400f010);	/* RDMA0 GLOBAL_CON = 0 */
	mdelay(100);
}

#if IS_ENABLED(CONFIG_BOARD_LATE_INIT)
static bool gemini_key_held(const char *label)
{
	struct udevice *dev;

	return !button_get_by_label(label, &dev) &&
	       button_get_state(dev) == BUTTON_ON;
}

static void gemini_flush_input(void)
{
	while (tstc())
		getchar();
}

/*
 * Esc (PWRKEY, mapped to Enter) and the side button are usually still held
 * from the LK power-on/boot selection when U-Boot starts. Wait up to 5 s for
 * both to be released while draining input, then drain once more, so no stale
 * Enter/Space reaches the boot menu.
 */
static void gemini_wait_keys_released(void)
{
	int i;

	for (i = 0; i < 50 && (gemini_key_held("pwrkey") ||
			       gemini_key_held("homekey")); i++) {
		gemini_flush_input();
		mdelay(100);
	}
	mdelay(100);
	gemini_flush_input();
}

/*
 * The vendor preloader/LK leaves the eMMC volatile write cache enabled, and
 * U-Boot's mmc driver has no cache handling, so small writes (e.g. "fastboot
 * flash boot") report OKAY but are lost from the card's cache on reset.
 * Disable the cache on device 0: per JEDEC eMMC 5.x, turning CACHE_CTRL off
 * forces a flush, so this both persists anything pending and makes every later
 * U-Boot write write-through. All errors are logged and ignored, never fatal.
 */
static void gemini_disable_emmc_cache(void)
{
	ALLOC_CACHE_ALIGN_BUFFER(u8, ext_csd, MMC_MAX_BLOCK_LEN);
	struct mmc *mmc = find_mmc_device(0);
	u32 cache_size;

	if (!mmc || mmc_init(mmc)) {
		printf("eMMC cache: device 0 unavailable, skipping\n");
		return;
	}

	if (mmc_send_ext_csd(mmc, ext_csd)) {
		printf("eMMC cache: EXT_CSD read failed, skipping\n");
		return;
	}

	cache_size = ext_csd[EXT_CSD_CACHE_SIZE] |
		     (ext_csd[EXT_CSD_CACHE_SIZE + 1] << 8) |
		     (ext_csd[EXT_CSD_CACHE_SIZE + 2] << 16) |
		     (ext_csd[EXT_CSD_CACHE_SIZE + 3] << 24);
	if (!cache_size)
		return;		/* card has no cache, nothing to do */

	printf("eMMC cache: CACHE_SIZE=%u, CACHE_CTRL=%u\n",
	       cache_size, ext_csd[EXT_CSD_CACHE_CTRL]);
	if (ext_csd[EXT_CSD_CACHE_CTRL] != 1)
		return;

	if (mmc_switch(mmc, EXT_CSD_CMD_SET_NORMAL, EXT_CSD_CACHE_CTRL, 0))
		printf("eMMC cache: disable failed, writes may not persist\n");
	else
		printf("eMMC cache: CACHE_CTRL 1 -> 0 (flushed, write-through)\n");
}

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

	gemini_disable_emmc_cache();

	gemini_wait_keys_released();

	return 0;
}
#endif
