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
#include <env.h>
#include <errno.h>
#include <fdt_support.h>
#include <fdtdec.h>
#include <init.h>
#include <wdt.h>
#include <vsprintf.h>
#include <asm/global_data.h>
#include <asm/system.h>
#include <linux/libfdt.h>
#include <linux/sizes.h>

#include "maxcpus.h"
#include "ram_top.h"
#include "reservations.h"

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

/*
 * PSCI SYSTEM_RESET hangs on this device (the mainline Gemini project sees the
 * same), so reset through the MTK RGU watchdog instead: wdt_expire_now() drives
 * the driver's SWRST path, which sets AUTO_RESTART so the SoC restarts rather
 * than waiting for the power key. Both the "reset" command and "fastboot reboot"
 * route through do_reset() -> reset_cpu(). Fall back to PSCI if the WDT is
 * somehow unavailable.
 */
void reset_cpu(void)
{
	struct udevice *dev;

	if (!uclass_first_device_err(UCLASS_WDT, &dev))
		wdt_expire_now(dev, 0);

	psci_system_reset();
}

int print_cpuinfo(void)
{
	printf("CPU:   MediaTek MT6797 (Helio X20/X25/X27)\n");
	return 0;
}

/*
 * The kernel loads its own DTB from the rootfs, so its reserved-memory does
 * not include the regions LK carved out of DRAM at runtime (TEE/ATF/modem, the
 * retained framebuffer and firmware logs, and the "mblock-*" blocks). Copy
 * every reservation from the LK-provided control FDT - both /memreserve/
 * entries and /reserved-memory nodes - into the kernel FDT so the kernel does
 * not reuse memory LK (or its secure world) still owns. U-Boot fixes up
 * /memory itself.
 *
 * Also keep the A72 cores (cpu@200/@201, i.e. CPU8/9) offline unless the user
 * asked for a specific maxcpus: bringing them up without the DA9214 buck
 * sequencing hangs the kernel. fdt_chosen() has already written the extlinux
 * "append" line into /chosen/bootargs by the time image_setup_libfdt() calls
 * us, so append " maxcpus=8" only when the user set no maxcpus token.
 */
int ft_board_setup(void *blob, struct bd_info *bd)
{
	const void *lk = gd->fdt_blob;
	int ret, chosen;

	ret = gemini_copy_reservations(blob, lk);
	if (ret < 0)
		return ret;

	/* The stock Android kernel gets LK's /chosen and its own framebuffer
	 * handling; only the Linux boot needs the maxcpus fixup. The
	 * "boot_android" command sets this env flag across its booti call.
	 */
	if (env_get("gemini_android"))
		return 0;

	chosen = fdt_path_offset(blob, "/chosen");
	if (chosen >= 0) {
		const char *old = fdt_getprop(blob, chosen, "bootargs", NULL);

		if (!gemini_bootargs_has_maxcpus(old)) {
			char args[CONFIG_SYS_CBSIZE];

			ret = snprintf(args, sizeof(args), "%s%smaxcpus=8",
				       old ?: "", old ? " " : "");
			/* Never truncate the user's bootargs to fit ours. */
			if (ret >= sizeof(args))
				ret = -FDT_ERR_NOSPACE;
			else
				ret = fdt_setprop_string(blob, chosen,
							 "bootargs", args);
			if (ret < 0)
				printf("gemini: failed to set maxcpus bootarg (%d)\n",
				       ret);
		}
	}

	return 0;
}
