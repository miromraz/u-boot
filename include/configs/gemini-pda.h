/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Configuration for the Planet Computers Gemini PDA (MediaTek MT6797).
 *
 * Copyright (C) 2026
 */

#ifndef __GEMINI_PDA_H
#define __GEMINI_PDA_H

#include <linux/sizes.h>

#define CFG_SYS_SDRAM_BASE		0x40000000

/*
 * Load addresses for distro/bootstd boot, above the static reserved-memory
 * (ends 0x46400000) and LK's DTB (0x44000000): 64 MiB for the kernel Image,
 * 64 MiB to decompress an Image.gz, ramdisk after that.
 */
#define CFG_EXTRA_ENV_SETTINGS \
	"scriptaddr=0x46800000\0" \
	"pxefile_addr_r=0x46900000\0" \
	"fdt_addr_r=0x47000000\0" \
	"kernel_addr_r=0x48000000\0" \
	"kernel_comp_addr_r=0x4c000000\0" \
	"kernel_comp_size=0x4000000\0" \
	"ramdisk_addr_r=0x50000000\0" \
	"fdtfile=mediatek/mt6797-gemini-pda.dtb\0" \
	"stdout=serial,vidconsole\0" \
	"stderr=serial,vidconsole\0"

#endif
