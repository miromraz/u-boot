// SPDX-License-Identifier: GPL-2.0
/*
 * Board support for the Planet Computers Gemini PDA (MediaTek MT6797).
 */

#include <bootcount.h>
#include <button.h>
#include <console.h>
#include <cpu_func.h>
#include <env.h>
#include <errno.h>
#include <g_dnl.h>
#include <i2c.h>
#include <init.h>
#include <memalign.h>
#include <mmc.h>
#include <vsprintf.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <dm/uclass.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/libfdt.h>
#include <power/pmic.h>
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
 * MT6351 RTC spare bits, mirroring the kernel reboot-mode series (rtc-mt6397):
 * on a warm reset LK inspects the always-on RTC PDN1 register to pick the boot
 * partition. This unit's LK compiles out the FAST_BOOT (bit 13) reader
 * (MTK_FASTBOOT_SUPPORT := no), so it is free for U-Boot - which runs after LK
 * - to consume: "reboot bootloader" (kernel mode-bootloader = <0x2000>) sets
 * the bit, and U-Boot enters fastboot instead of autobooting. The RTC lives in
 * the MT6351 PMIC register space (base 0x4000), reached over PWRAP WACS2 just
 * like the power/home keys. The pwrap read/write ops move a full 16-bit WACS2
 * word regardless of the PMIC uclass transfer length, so pmic_reg_read()/
 * pmic_reg_write() carry all of PDN1 (including bit 13).
 */
#define MT6351_RTC_BBPU		0x4000	/* RTC base + RTC_BBPU (0x00) */
#define RTC_BBPU_CBUSY		BIT(6)
#define MT6351_RTC_PDN1		0x402c	/* RTC base + RTC_PDN1 (0x2c) */
#define RTC_PDN1_FASTBOOT	BIT(13)
#define MT6351_RTC_WRTGR	0x403c	/* RTC base + RTC_WRTGR (0x3c) */

static struct udevice *gemini_pmic(void)
{
	struct udevice *dev;

	/* The pwrap wrapper is the board's only UCLASS_PMIC device. */
	return uclass_first_device_err(UCLASS_PMIC, &dev) ? NULL : dev;
}

/* Commit an RTC register write: pulse WRTGR, then wait up to ~1 s (like the
 * kernel driver) for BBPU CBUSY to clear. */
static int gemini_rtc_commit(struct udevice *pmic)
{
	int i, val;

	val = pmic_reg_write(pmic, MT6351_RTC_WRTGR, 1);
	if (val)
		return val;

	for (i = 0; i < 1000; i++) {
		val = pmic_reg_read(pmic, MT6351_RTC_BBPU);
		if (val < 0)
			return val;
		if (!(val & RTC_BBPU_CBUSY))
			return 0;
		udelay(1000);
	}
	return -ETIMEDOUT;
}

#if IS_ENABLED(CONFIG_BOOTCOUNT_LIMIT)
/*
 * Boot-failure fallback (CONFIG_BOOTCOUNT_LIMIT).
 *
 * A cold-booted kernel that hangs before it takes over the RGU watchdog (for
 * example an extlinux label whose DTB is missing, so USB/NCM never comes up)
 * lets the LK-armed watchdog fire and reset the SoC, which otherwise drops the
 * user into the preloader download loop until they re-enter fastboot by hand.
 * Instead, count consecutive watchdog resets and, once the count exceeds
 * "bootlimit", run "altbootcmd" (fastboot usb 0) in place of the normal boot,
 * so a bad boot recovers to fastboot with no physical interaction.
 *
 * The count must survive the warm RGU reset. DRAM does not survive it reliably
 * on this board (pstore/ramoops came back empty after a watchdog reset, see
 * docs/kexec-oneshot.md), so the generic DRAM backend is unusable. The MT6351
 * RTC is always-on and already reached over PWRAP for the fastboot-request bit
 * above, so store the count there: override the weak bootcount_store()/
 * bootcount_load() that BOOTCOUNT_GENERIC provides. A magic byte in the high
 * half distinguishes a real count from a cleared or battery-pulled RTC.
 *
 * TODO(hw-verify): confirm RTC_SPAR1 (0x32) is free on this unit - dump
 * PDN1/PDN2/SPAR0/SPAR1 and check that LK/kernel reboot-mode and the RTC alarm
 * leave it alone (see docs/uboot-boot-fallback.md).
 */
#define MT6351_RTC_BOOTCOUNT	0x4032	/* RTC base + RTC_SPAR1 (0x32) */
#define RTC_BOOTCOUNT_MAGIC	0xbc00	/* high byte; low byte holds the count */
#define RTC_BOOTCOUNT_MASK	0x00ff

void bootcount_store(ulong a)
{
	struct udevice *pmic = gemini_pmic();

	if (!pmic)
		return;

	if (a > RTC_BOOTCOUNT_MASK)
		a = RTC_BOOTCOUNT_MASK;

	if (pmic_reg_write(pmic, MT6351_RTC_BOOTCOUNT,
			   RTC_BOOTCOUNT_MAGIC | (a & RTC_BOOTCOUNT_MASK)) ||
	    gemini_rtc_commit(pmic))
		printf("bootcount: RTC store failed\n");
}

ulong bootcount_load(void)
{
	struct udevice *pmic = gemini_pmic();
	int val;

	if (!pmic)
		return 0;

	val = pmic_reg_read(pmic, MT6351_RTC_BOOTCOUNT);
	if (val < 0 || (val & ~RTC_BOOTCOUNT_MASK) != RTC_BOOTCOUNT_MAGIC)
		return 0;	/* cleared, battery-pulled or never written */

	return val & RTC_BOOTCOUNT_MASK;
}

/*
 * Reset the streak on any reset that is NOT a watchdog timeout: a cold
 * power-on, a clean reboot and a fastboot reboot all mean the previous boot did
 * not hang, so the count starts over. Only consecutive watchdog resets - the
 * signature of a kernel that hangs before it can pet the RGU - accumulate
 * towards "bootlimit". The TOPRGU status latch holds the last reset cause
 * across the reset.
 *
 * TODO(hw-verify): confirm the preloader/LK leave WDT_STATUS latched until this
 * runs, and the HW/SW-WDT bit positions. If the status is always clear here the
 * count is only ever reset (a safe no-op) and must instead be cleared from
 * Linux once userspace is healthy - see docs/uboot-boot-fallback.md.
 */
#define MT6797_TOPRGU_STATUS	(0x10007000 + 0x0c)
#define WDT_STATUS_WDT_RST	(BIT(31) | BIT(30))	/* HW | SW watchdog */

static void gemini_bootcount_init(void)
{
	u32 status = readl((void __iomem *)MT6797_TOPRGU_STATUS);
	ulong count;

	/*
	 * "bootlimit" and "altbootcmd" are baked into the compiled default
	 * environment (CONFIG_BOOTCOUNT_BOOTLIMIT / CONFIG_BOOTCOUNT_ALTBOOTCMD),
	 * not set here: env is CONFIG_ENV_IS_NOWHERE, so the default env is
	 * authoritative and present from the first boot instruction.
	 */
	count = bootcount_load();
	/* Reset the streak on a non-watchdog reset; skip the write if clear. */
	if (!(status & WDT_STATUS_WDT_RST) && count) {
		bootcount_store(0);
		count = 0;
	}

	/*
	 * No UART on this unit: echo to the panel console so the HW test can
	 * watch the counter climb and see the raw reset cause.
	 */
	printf("bootcount: %lu, TOPRGU status 0x%08x\n", count, status);
}
#endif /* CONFIG_BOOTCOUNT_LIMIT */

/*
 * Enter fastboot instead of autobooting when "reboot bootloader" requested it,
 * so remote reflash/test cycles need no physical interaction. Read the RTC
 * FAST_BOOT bit, clear it (read-modify-write preserving the other PDN1 fields,
 * then WRTGR) so the request is one-shot, and only enter fastboot once a
 * read-back confirms the bit is gone - otherwise a stuck bit would trap every
 * boot in fastboot.
 */
static void gemini_check_fastboot_request(void)
{
	struct udevice *pmic = gemini_pmic();
	int pdn1;

	if (!pmic)
		return;

	pdn1 = pmic_reg_read(pmic, MT6351_RTC_PDN1);
	if (pdn1 < 0 || !(pdn1 & RTC_PDN1_FASTBOOT))
		return;

	if (pmic_reg_write(pmic, MT6351_RTC_PDN1, pdn1 & ~RTC_PDN1_FASTBOOT) ||
	    gemini_rtc_commit(pmic))
		printf("fastboot: failed to clear RTC FAST_BOOT bit\n");

	pdn1 = pmic_reg_read(pmic, MT6351_RTC_PDN1);
	if (pdn1 < 0 || (pdn1 & RTC_PDN1_FASTBOOT)) {
		printf("fastboot: RTC FAST_BOOT still set, booting normally\n");
		return;
	}

	printf("fastboot requested -> entering fastboot\n");
	/* Keep CONFIG_BOOTCOMMAND after it so leaving fastboot (Ctrl-C /
	 * "fastboot continue") falls through to a normal boot, not a prompt. */
	env_set("bootcmd", "fastboot usb 0; " CONFIG_BOOTCOMMAND);
}

/*
 * TI BQ25896 single-cell charger on i2c0 (datasheet SLUSC76). LK enables
 * charging before handing over, and the chip charges on its own, but its I2C
 * watchdog resets the charge registers ~40 s after the last host access
 * (REG07 WATCHDOG, default 40 s). U-Boot has no timer loop to pet it, so while
 * the board sits in fastboot waiting for a rescue the watchdog fires, input
 * detection re-runs and the input limit can fall to the 100 mA floor, and the
 * battery drains. Mirror the vendor firmware, which disables this watchdog in
 * its charger init: pet it once, turn it off, make sure charging is enabled
 * with HIZ cleared, and hold the input current limit at the 500 mA USB-host
 * baseline (never lower a higher limit the chip set for an adapter). The chip's
 * power-on charge voltage/current defaults already match the vendor settings,
 * so nothing else is reprogrammed. Best-effort: every I2C error is logged and
 * ignored, so a charger fault never blocks boot.
 */
#define BQ25896_I2C_BUS			0	/* aliases: i2c0 = &i2c0 */
#define BQ25896_I2C_ADDR		0x6b
#define BQ25896_REG00			0x00	/* EN_HIZ[7], IINLIM[5:0] */
#define BQ25896_REG00_EN_HIZ		BIT(7)
#define BQ25896_REG00_IINLIM		GENMASK(5, 0)
#define BQ25896_IINLIM_500MA		0x08	/* (500 - 100) / 50 mA */
#define BQ25896_REG03			0x03	/* WD_RST[6], OTG_CONFIG[5], CHG_CONFIG[4] */
#define BQ25896_REG03_WD_RST		BIT(6)
#define BQ25896_REG03_OTG_CONFIG	BIT(5)
#define BQ25896_REG03_CHG_CONFIG	BIT(4)
#define BQ25896_REG07			0x07	/* WATCHDOG[5:4] */
#define BQ25896_REG07_WATCHDOG		GENMASK(5, 4)

static void gemini_charger_init(void)
{
	struct udevice *dev;
	int ret, reg00;

	ret = i2c_get_chip_for_busnum(BQ25896_I2C_BUS, BQ25896_I2C_ADDR, 1,
				      &dev);
	if (ret) {
		printf("charger: BQ25896 not found on i2c0 (%d), skipping\n",
		       ret);
		return;
	}

	reg00 = dm_i2c_reg_read(dev, BQ25896_REG00);
	if (reg00 < 0) {
		printf("charger: BQ25896 read failed (%d), skipping\n", reg00);
		return;
	}

	/* Clear HIZ and raise the input limit to >= 500 mA, never below it. */
	if ((reg00 & BQ25896_REG00_IINLIM) < BQ25896_IINLIM_500MA)
		dm_i2c_reg_clrset(dev, BQ25896_REG00,
				  BQ25896_REG00_EN_HIZ | BQ25896_REG00_IINLIM,
				  BQ25896_IINLIM_500MA);
	else
		dm_i2c_reg_clrset(dev, BQ25896_REG00, BQ25896_REG00_EN_HIZ, 0);

	/* Pet the watchdog, enable charging, keep boost (OTG) off. */
	dm_i2c_reg_clrset(dev, BQ25896_REG03, BQ25896_REG03_OTG_CONFIG,
			  BQ25896_REG03_WD_RST | BQ25896_REG03_CHG_CONFIG);

	/* Disable the I2C watchdog so the settings persist with no kick loop. */
	dm_i2c_reg_clrset(dev, BQ25896_REG07, BQ25896_REG07_WATCHDOG, 0);

	printf("charger: BQ25896 charging enabled, I2C watchdog off\n");
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

	gemini_charger_init();

	gemini_check_fastboot_request();

#if IS_ENABLED(CONFIG_BOOTCOUNT_LIMIT)
	gemini_bootcount_init();
#endif

	gemini_wait_keys_released();

	return 0;
}
#endif
