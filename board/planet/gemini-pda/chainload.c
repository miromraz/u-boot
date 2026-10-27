// SPDX-License-Identifier: GPL-2.0
/*
 * Two-stage boot for the Planet Gemini PDA (MediaTek MT6797).
 *
 * LK write-protects the "boot" partition as eMMC power-on WP in NORMAL boot
 * mode (docs/emmc-boot-wp.md), so the U-Boot flashed into "boot" cannot be
 * rewritten at runtime. To keep that U-Boot ("stage 1") frozen yet still
 * iterate on U-Boot, stage 1 chainloads a "stage 2" U-Boot stored as an
 * ordinary file on the unlocked "linux" partition, started before the normal
 * bootmenu. Future U-Boot updates are then plain file writes - no reflash.
 *
 * The stage-2 image is the same LK-compatible Android boot image the package
 * builds for "boot" (gzip(u-boot.bin) ++ appended DTB; see mkbootimg.sh), built
 * with CONFIG_GEMINI_CHAINLOAD disabled so stage 2 has no chainload code and can
 * never chainload again: that is the build-time loop guard.
 *
 * Hand-off state across the chainload. LK handed stage 1 the control device
 * tree in x0 - /memory, /chosen (the videolfb framebuffer base and the atag,*
 * values), CPU clock-frequency and the mblock reserved-memory regions. Stage 1
 * hands stage 2 a verbatim copy of that same FDT through booti's x0, so the DT
 * pointer, reserved memory and the live display are preserved exactly as LK
 * left them: stage 2 initialises identically to stage 1 (same board_fdt_blob_
 * setup(), same board_get_usable_ram_top(), same framebuffer). The Linux-only
 * DT fixups (A72 power-on, simple-framebuffer) and the display quiesce are
 * skipped for this jump via the "gemini_chainload" env flag, read by
 * ft_board_setup() (mt6797/init.c) and board_quiesce_devices() (gemini-pda.c):
 * stage 2 is U-Boot, not a kernel, so it runs those itself at its own OS boot
 * and needs the display pipeline still scanning out for its own console.
 *
 * Any failure (missing file, bad/short hash, oversize, load or boot error)
 * returns to the caller, which continues booting stage 1 normally (bootmenu,
 * bootfallback, fastboot, extlinux). The caller also skips the chainload when a
 * key is held or fastboot was requested, so stage 1 stays fully recoverable.
 */

#include <android_image.h>
#include <blk.h>
#include <bootcount.h>
#include <command.h>
#include <dm.h>
#include <env.h>
#include <fs.h>
#include <gzip.h>
#include <mapmem.h>
#include <part.h>
#include <vsprintf.h>
#include <wdt.h>
#include <asm/global_data.h>
#include <linux/libfdt.h>
#include <linux/string.h>
#include <u-boot/sha256.h>

DECLARE_GLOBAL_DATA_PTR;

#define STAGE2_PART	"linux"
#define STAGE2_IMG	"/boot/u-boot/u-boot-stage2.img"
#define STAGE2_SHA	"/boot/u-boot/u-boot-stage2.img.sha256"
#define STAGE2_MAX	0x400000	/* 4 MiB cap; the image is ~0.5 MiB */
#define STAGE2_LOAD	0x58000000	/* fastboot download buffer, free here */
#define STAGE2_SCRATCH	0x58400000	/* above the image, for the hash file */

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Parse the leading 64 hex chars (the digest field of sha256sum output). */
static int parse_sha256_hex(const char *s, int len, u8 out[32])
{
	int i;

	if (len < 64)
		return -1;
	for (i = 0; i < 32; i++) {
		int hi = hexval(s[2 * i]);
		int lo = hexval(s[2 * i + 1]);

		if (hi < 0 || lo < 0)
			return -1;
		out[i] = (hi << 4) | lo;
	}
	return 0;
}

/* fs_set_blk_dev() is consumed by one fs op, so re-set it before each. */
static int stage2_fs_size(const char *part, const char *path, loff_t *sz)
{
	if (fs_set_blk_dev("mmc", part, FS_TYPE_EXT))
		return -1;
	return fs_size(path, sz);
}

static int stage2_fs_read(const char *part, const char *path, ulong addr,
			  loff_t len, loff_t *rd)
{
	if (fs_set_blk_dev("mmc", part, FS_TYPE_EXT))
		return -1;
	return fs_read(path, addr, 0, len, rd);
}

void gemini_chainload(void)
{
	struct andr_boot_img_hdr_v0 *hdr;
	struct disk_partition info;
	struct blk_desc *desc;
	u8 want[32], got[32];
	loff_t sz = 0, rd = 0;
	ulong kaddr, komp, faddr;
	unsigned long klen;
	char part[16], cmd[64];
	const void *lk;
	u32 page, ksize, fdtsz;
	const u8 *kern;
	int idx;

#if IS_ENABLED(CONFIG_BOOTCOUNT_LIMIT)
	/*
	 * Loop guard. A stage-2 image can pass the sha256 check yet hang before
	 * Linux clears the boot counter; the LK watchdog then resets the SoC and
	 * stage 1 would chainload the same image again, forever. autoboot's
	 * bootcount_inc() runs only in main_loop, which a successful chainload
	 * never reaches, so count each chainload attempt here instead (just
	 * before booti below). Once the count exceeds "bootlimit", stop
	 * chainloading and fall through to stage 1's own boot - whose main_loop
	 * then selects altbootcmd (fastboot) via its own bootcount_error(). The
	 * counter is reset by Linux clearing the RTC bootcount nvmem cell once a
	 * boot reaches userspace, so normal boots never trip this.
	 */
	if (bootcount_error())
		return;
#endif

	desc = blk_get_devnum_by_uclass_id(UCLASS_MMC, 0);
	if (!desc)
		return;
	idx = part_get_info_by_name(desc, STAGE2_PART, &info);
	if (idx < 0)
		return;		/* no "linux" partition, nothing to chainload */
	snprintf(part, sizeof(part), "0:%d", idx);

	/* Size limit before reading: refuse anything larger than the cap. */
	if (stage2_fs_size(part, STAGE2_IMG, &sz) || sz == 0 || sz > STAGE2_MAX)
		return;

	if (stage2_fs_read(part, STAGE2_IMG, STAGE2_LOAD, sz, &rd) || rd != sz)
		return;

	/* Read and parse the companion sha256 file, then verify the image. */
	if (stage2_fs_read(part, STAGE2_SHA, STAGE2_SCRATCH, 80, &rd))
		return;
	if (parse_sha256_hex(map_sysmem(STAGE2_SCRATCH, 80), (int)rd, want))
		return;
	sha256_csum_wd(map_sysmem(STAGE2_LOAD, sz), (unsigned int)sz, got,
		       0x10000);
	if (memcmp(want, got, 32)) {
		printf("stage2: sha256 mismatch, skipping\n");
		return;
	}

	/* Parse the Android boot image header staged at STAGE2_LOAD. */
	hdr = map_sysmem(STAGE2_LOAD, 0);
	if (memcmp(hdr->magic, ANDR_BOOT_MAGIC, ANDR_BOOT_MAGIC_SIZE))
		return;
	page = hdr->page_size;
	ksize = hdr->kernel_size;
	if (!page || (page & (page - 1)) || !ksize)
		return;
	kern = (const u8 *)hdr + page;

	/* The kernel payload is gzip(u-boot.bin) ++ DTB; gunzip the u-boot.bin
	 * (the appended DTB is after the gzip stream and gunzip ignores it). */
	if (kern[0] != 0x1f || kern[1] != 0x8b) {
		printf("stage2: kernel is not gzip-compressed\n");
		return;
	}
	kaddr = env_get_hex("kernel_addr_r", 0x48000000);
	komp = env_get_hex("kernel_comp_size", 0x4000000);
	faddr = env_get_hex("fdt_addr_r", 0x47000000);
	klen = ksize;
	if (gunzip(map_sysmem(kaddr, komp), komp, (u8 *)kern, &klen)) {
		printf("stage2: kernel gunzip failed\n");
		return;
	}

	/* Hand stage 2 a verbatim copy of LK's control FDT (see the file head).
	 * Copy it so bootm's own /memory fixup writes into the copy, not the
	 * live control DT stage 1 is still running on. */
	lk = gd->fdt_blob;
	fdtsz = fdt_totalsize(lk);
	memcpy(map_sysmem(faddr, fdtsz), lk, fdtsz);

	/* Skip the Linux-only DT fixups and the display quiesce for this jump. */
	env_set("gemini_chainload", "1");

#if IS_ENABLED(CONFIG_BOOTCOUNT_LIMIT)
	/*
	 * Count this chainload attempt before handing over (see the loop guard at
	 * the top): a stage 2 that hangs before Linux clears the counter then loops
	 * only up to "bootlimit" times before stage 1 drops to fastboot. Read the
	 * value back - the counter lives behind the PMIC/RTC and bootcount_load()/
	 * _store() fail open (load returns 0, store only warns), so if the write did
	 * not stick the count can never climb and the guard could never fire. A
	 * failed store therefore aborts the chainload: stage 1 is the safe place to
	 * stay.
	 */
	{
		ulong bc = bootcount_load();

		bootcount_store(bc + 1);
		if (bootcount_load() != bc + 1) {
			printf("stage2: bootcount not persisted (PMIC/RTC?), not chainloading\n");
			env_set("gemini_chainload", NULL);
			return;
		}
	}
#endif

#if IS_ENABLED(CONFIG_WDT)
	/*
	 * Arm the RGU watchdog across the hand-over. Nothing kicks it once we jump,
	 * so a stage 2 that hangs before it re-arms/services the WDT itself still
	 * resets (the mtk_wdt driver data sets AUTO_RESTART, so the reset is not
	 * gated on the power key). With CONFIG_WATCHDOG the running U-Boot services
	 * it through the cyclic framework, so an idle fastboot is fine but a true
	 * hang resets. 15 s is within the mtk_wdt range (the driver clamps anything
	 * above ~16 s) and leaves the kernel time to take the watchdog over.
	 */
	{
		struct udevice *wdt;

		if (!uclass_first_device_err(UCLASS_WDT, &wdt))
			wdt_start(wdt, 15000, 0);
		else
			printf("stage2: no WDT device, chainloading without a hang guard\n");
	}
#endif

	snprintf(cmd, sizeof(cmd), "booti 0x%lx - 0x%lx", kaddr, faddr);
	printf("stage2: %s verified (%llu B), chainloading\n",
	       STAGE2_IMG, (unsigned long long)sz);
	run_command(cmd, 0);

	/*
	 * Reached only if booti returned. U-Boot relocates itself to the top of
	 * usable DRAM at init (board_get_usable_ram_top() in mt6797/init.c returns
	 * ram_base + ram_size minus the reservations, so gd->relocaddr sits near the
	 * top), while booti's Image lands in low RAM (kernel_addr_r 0x48000000 and
	 * down); the running stage-1 code, stack and heap are therefore not
	 * clobbered, so do_reset() here is safe. Reset rather than resume the normal
	 * boot: the bootcount above is already incremented, so a persistent failure
	 * trips the loop guard on the next pass and lands in fastboot.
	 */
	printf("stage2: chainload returned, resetting\n");
	do_reset(NULL, 0, 0, NULL);
}
