// SPDX-License-Identifier: GPL-2.0
/*
 * "boot_android" command for the Planet Gemini PDA (MediaTek MT6797).
 *
 * Boots the stock Android 8.1 kernel (Linux 3.18.79) straight from the eMMC
 * "boot3" partition, with no key presses, alongside U-Boot's normal Linux
 * boot. Today the stock kernel is only reachable via LK's side+power key combo.
 *
 * The 3.18 kernel needs the device tree LK prepares for it: the vendor nodes
 * (from boot3's own appended DTB) plus LK's runtime /chosen - the full kernel
 * command line and the "atag,*" properties the MTK 3.18 drivers read (boot
 * reason, lcm, dram info, fuel-gauge OCV, the live framebuffer base, ...).
 *
 * We already run on exactly that DTB: LK applied these same /chosen fixups to
 * U-Boot's own control DTB and handed it to us in x0 (gd->fdt_blob). The
 * command line and atags LK builds are image-independent (built from LK
 * constants plus this boot session's runtime values, not from the boot image
 * header - see app/mt_boot/mt_boot.c), so LK's /chosen is valid for the stock
 * kernel verbatim. We therefore:
 *
 *   1. read boot3 (an Android boot image v0) into RAM;
 *   2. gunzip its kernel (gzip(Image) with the vendor DTB appended) to
 *      kernel_addr_r, and copy the ramdisk to ramdisk_addr_r;
 *   3. take the appended vendor DTB and overlay LK's live /chosen onto it;
 *   4. booti the kernel with that DTB and the ramdisk.
 *
 * LK's memory reservations and /memory are added by the normal boot fdt fixup
 * (ft_board_setup() copies the reservations from gd->fdt_blob; arch_fixup_fdt()
 * writes /memory from bi_dram), the same path the Linux boot uses. The two
 * Linux-only fixups (maxcpus=8, simple-framebuffer) are skipped for Android via
 * gemini_booting_android - see arch/arm/mach-mediatek/mt6797/init.c.
 */

#include <android_image.h>
#include <blk.h>
#include <command.h>
#include <env.h>
#include <gzip.h>
#include <mapmem.h>
#include <part.h>
#include <vsprintf.h>
#include <asm/global_data.h>
#include <linux/kernel.h>
#include <linux/libfdt.h>
#include <linux/string.h>

DECLARE_GLOBAL_DATA_PTR;

/*
 * Env flag set across the booti call so ft_board_setup() skips the Linux-only
 * DT fixups (maxcpus=8, simple-framebuffer) for the stock Android kernel. See
 * arch/arm/mach-mediatek/mt6797/init.c.
 */
#define GEMINI_ANDROID_FLAG	"gemini_android"

/* Scratch RAM for the raw boot3 image: above the kernel/ramdisk/fdt load
 * addresses and below the fastboot download buffer (0x58000000 is unused while
 * an "oem run" command executes).
 */
#define GEMINI_ANDROID_SCRATCH	0x5c000000

/* Overlay LK's live /chosen (gd->fdt_blob) onto the Android DTB: the full
 * bootargs plus every atag,* property the 3.18 drivers read. Skip the stale
 * initrd pointers - booti sets linux,initrd-* for the ramdisk we pass it.
 */
static int gemini_copy_lk_chosen(void *dst, const void *src)
{
	int s, d, prop;

	s = fdt_path_offset(src, "/chosen");
	if (s < 0)
		return 0;

	d = fdt_path_offset(dst, "/chosen");
	if (d < 0)
		d = fdt_add_subnode(dst, 0, "chosen");
	if (d < 0)
		return d;

	fdt_for_each_property_offset(prop, src, s) {
		const char *name;
		const void *val;
		int len, err;

		val = fdt_getprop_by_offset(src, prop, &name, &len);
		if (!val || !name)
			continue;
		if (!strcmp(name, "linux,initrd-start") ||
		    !strcmp(name, "linux,initrd-end"))
			continue;
		err = fdt_setprop(dst, d, name, val, len);
		if (err < 0)
			return err;
	}
	return 0;
}

/* Find the device tree appended after the gzip kernel stream: the DTB whose
 * header totalsize runs exactly to the end of the kernel payload. The vendor
 * tools append it on a byte boundary (not 4-aligned, see mkbootimg.sh), so scan
 * every offset and read the big-endian magic/size byte-wise to stay alignment
 * safe; the caller memmoves the result to an aligned buffer.
 */
static const void *gemini_find_appended_dtb(const u8 *kern, u32 ksize,
					    u32 *dsize)
{
	u32 off;

	for (off = 0; off + sizeof(struct fdt_header) <= ksize; off++) {
		const u8 *p = kern + off;
		u32 mag = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
		u32 ts = (p[4] << 24) | (p[5] << 16) | (p[6] << 8) | p[7];

		if (mag != FDT_MAGIC)
			continue;
		if (ts >= sizeof(struct fdt_header) && off + ts == ksize) {
			*dsize = ts;
			return p;
		}
	}
	return NULL;
}

static int do_boot_android(struct cmd_tbl *cmdtp, int flag, int argc,
			   char *const argv[])
{
	const char *part = (argc > 1) ? argv[1] : "boot3";
	struct andr_boot_img_hdr_v0 *hdr;
	struct disk_partition info;
	struct blk_desc *desc;
	const void *src_dtb;
	void *kern_dst, *fdt_dst;
	ulong kaddr, raddr, faddr, komp_sz;
	u32 page, koff, roff, ksize, rsize, total, dsize;
	unsigned long klen;
	char cmd[128];
	lbaint_t nblk;
	u8 *img;
	int ret;

	desc = blk_get_devnum_by_uclass_id(UCLASS_MMC, 0);
	if (!desc) {
		printf("boot_android: no eMMC (mmc 0)\n");
		return CMD_RET_FAILURE;
	}
	if (part_get_info_by_name(desc, part, &info) < 0) {
		printf("boot_android: partition '%s' not found\n", part);
		return CMD_RET_FAILURE;
	}

	img = map_sysmem(GEMINI_ANDROID_SCRATCH, 0);

	/* Read the header page, validate, then read the kernel+ramdisk span. */
	if (blk_dread(desc, info.start, 1, img) != 1) {
		printf("boot_android: header read failed\n");
		return CMD_RET_FAILURE;
	}
	hdr = (struct andr_boot_img_hdr_v0 *)img;
	if (memcmp(hdr->magic, ANDR_BOOT_MAGIC, ANDR_BOOT_MAGIC_SIZE)) {
		printf("boot_android: '%s' is not an Android boot image\n", part);
		return CMD_RET_FAILURE;
	}
	page = hdr->page_size;
	ksize = hdr->kernel_size;
	rsize = hdr->ramdisk_size;
	if (!page || page & (page - 1) || !ksize) {
		printf("boot_android: bad header (page=%u ksize=%u)\n",
		       page, ksize);
		return CMD_RET_FAILURE;
	}
	koff = page;
	roff = koff + ROUND(ksize, page);
	total = roff + ROUND(rsize, page);

	nblk = DIV_ROUND_UP(total, info.blksz);
	if (nblk > info.size) {
		printf("boot_android: image larger than partition\n");
		return CMD_RET_FAILURE;
	}
	if (blk_dread(desc, info.start, nblk, img) != nblk) {
		printf("boot_android: image read failed\n");
		return CMD_RET_FAILURE;
	}

	/* The MTK kernel payload is gzip(Image) with the vendor DTB appended. */
	if (img[koff] != 0x1f || img[koff + 1] != 0x8b) {
		printf("boot_android: kernel is not gzip-compressed\n");
		return CMD_RET_FAILURE;
	}

	kaddr = env_get_hex("kernel_addr_r", 0x48000000);
	raddr = env_get_hex("ramdisk_addr_r", 0x50000000);
	faddr = env_get_hex("fdt_addr_r", 0x47000000);
	komp_sz = env_get_hex("kernel_comp_size", 0x4000000);

	kern_dst = map_sysmem(kaddr, komp_sz);
	klen = ksize;
	ret = gunzip(kern_dst, komp_sz, img + koff, &klen);
	if (ret) {
		printf("boot_android: kernel gunzip failed (%d)\n", ret);
		return CMD_RET_FAILURE;
	}

	/* Build the kernel DTB at fdt_addr_r: the vendor DTB + LK's /chosen.
	 * memmove to the aligned load address first (src may be unaligned).
	 */
	src_dtb = gemini_find_appended_dtb(img + koff, ksize, &dsize);
	if (!src_dtb) {
		printf("boot_android: no appended DTB in kernel\n");
		return CMD_RET_FAILURE;
	}
	fdt_dst = map_sysmem(faddr, dsize + 0x10000);
	memmove(fdt_dst, src_dtb, dsize);
	ret = fdt_open_into(fdt_dst, fdt_dst, dsize + 0x10000);
	if (ret) {
		printf("boot_android: fdt_open_into failed (%d)\n", ret);
		return CMD_RET_FAILURE;
	}
	ret = gemini_copy_lk_chosen(fdt_dst, gd->fdt_blob);
	if (ret) {
		printf("boot_android: copying LK /chosen failed (%d)\n", ret);
		return CMD_RET_FAILURE;
	}

	/* Copy the ramdisk to its load address. */
	if (rsize)
		memcpy(map_sysmem(raddr, rsize), img + roff, rsize);

	/* booti reads /chosen/bootargs from the DTB only when the "bootargs"
	 * env var is unset; keep LK's command line by clearing it.
	 */
	env_set("bootargs", NULL);

	if (rsize)
		snprintf(cmd, sizeof(cmd), "booti 0x%lx 0x%lx:0x%x 0x%lx",
			 kaddr, raddr, rsize, faddr);
	else
		snprintf(cmd, sizeof(cmd), "booti 0x%lx - 0x%lx", kaddr, faddr);

	printf("boot_android: %s (kernel %u B, ramdisk %u B)\n",
	       part, ksize, rsize);

	env_set(GEMINI_ANDROID_FLAG, "1");
	ret = run_command(cmd, 0);
	env_set(GEMINI_ANDROID_FLAG, NULL);	/* reached only if booti returned */

	return ret ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

U_BOOT_CMD(boot_android, 2, 0, do_boot_android,
	   "boot the stock Android kernel from eMMC",
	   "[partition]\n"
	   "    - load an Android boot image (default: boot3), merge LK's\n"
	   "      live /chosen into its DTB, and boot it");
