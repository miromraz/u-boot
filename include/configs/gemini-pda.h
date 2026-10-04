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
 * Load addresses for distro/bootstd boot. DRAM starts at 0x40000000; keep
 * these in the low, always-free part of the bank (below the LK framebuffer).
 */
#define CFG_EXTRA_ENV_SETTINGS \
	"kernel_addr_r=0x44000000\0" \
	"fdt_addr_r=0x43000000\0" \
	"ramdisk_addr_r=0x48000000\0" \
	"scriptaddr=0x42000000\0" \
	"pxefile_addr_r=0x42100000\0"

#endif
