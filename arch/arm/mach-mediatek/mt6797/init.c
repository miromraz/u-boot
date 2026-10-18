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
#include <fdt_simplefb.h>
#include <fdt_support.h>
#include <fdtdec.h>
#include <i2c.h>
#include <init.h>
#include <wdt.h>
#include <vsprintf.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <asm/system.h>
#include <linux/arm-smccc.h>
#include <linux/bitops.h>
#include <linux/delay.h>
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
 * Cortex-A72 ("big"/iDVFS "B") cluster power-on, the vendor 3.18
 * cpu_power_on_buck() sequence (docs/a72.md). The MT6797 ships a thin
 * closed PSCI that does NOT power the B cluster in EL3, so a plain
 * PSCI CPU_ON of cpu@200/@201 hangs the SoC. The vendor works around this
 * from the kernel; U-Boot owns the boot before Linux and can do it once here
 * so Linux then brings both A72 cores up with plain PSCI during boot-time SMP
 * (they are present at capability finalisation, avoiding the Spectre-v2
 * hot-online reject). Proven on hardware by the manual `oem run` sequence
 * (all 10 CPUs online, policy8 DVFS across the full table).
 *
 * The register pokes are raw readl()/writel(): these are bare SoC control
 * registers with no syscon/regmap node in the U-Boot DT, and this runs once.
 * Only buck B (over i2c6) can fail in a way that would leave the cluster
 * unpowered, so that is the step whose failure aborts the whole power-on.
 */
#define MT6797_SPM_CL2_PWR_REQ		0x10006218	/* bit0: CL2 req  */
#define MT6797_SPM_CL2_EXT_BUCK_ISO	0x10006290	/* bits1:0: B iso */
#define MT6797_IDVFS_PLL_DUMMY		0x102224a0	/* dummy read     */
#define MT6797_TOPRGU_SWSYSRST		0x10007018	/* key 0x88<<24   */
#define MT6797_TOPRGU_SWSYSRST_EN	0x100070fc
#define MT6797_TOPRGU_KEY		0x88000000
#define MT6797_PWRAP_SPI_RST		BIT(11)		/* PWRAP-SPI line */
#define MT6797_MCUCFG_MP2_DCM		0x10222274	/* MP2 sync DCM   */
#define MT6797_MP2_DCM_CFG		0xd		/* ON | DIV_SEL4  */
#define MT6797_MP2_DCM_TOG		BIT(1)		/* toggle bit     */

#define DA9214_I2C_BUS			6
#define DA9214_I2C_ADDR			0x68
#define DA9214_VBUCKB_A			0xd9	/* 300 mV + 10 mV/step */
#define DA9214_VBUCKB_1V00		0x46	/* vsel for 1.00 V     */
#define DA9214_BUCKB_CONT		0x5e
#define DA9214_BUCKB_EN			0x01	/* EN=1, sel=A, GPI off */

#define IDVFS_SMC_B_SRAM_LDO		0xc20003bf	/* mV * 100 */
#define B_SRAM_LDO_1V10			0x1adb0		/* 1100 mV  */

static int gemini_cl2_power_on(void)
{
	struct arm_smccc_res res;
	struct udevice *bus, *dev;
	int ret;
	u32 v;

	/* DA9214 buck B is the only step that can fail; resolve it first so a
	 * bus problem aborts before any SPM/TOPRGU/MCUCFG state is touched.
	 * Mirror the proven `i2c dev 6` + `i2c mw` command path exactly: fetch
	 * the bus by its (aliased) sequence and the chip with no presence
	 * probe. i2c_get_chip_for_busnum() would add an i2c_probe_chip() detect
	 * read, which i2c6 cannot do - the APPM controller merges the WRRD
	 * transfer length, so only writes land on this bus.
	 */
	ret = uclass_get_device_by_seq(UCLASS_I2C, DA9214_I2C_BUS, &bus);
	if (ret)
		return ret;
	ret = i2c_get_chip(bus, DA9214_I2C_ADDR, 1, &dev);
	if (ret)
		return ret;

	/* 1) SPM CL2 power request. */
	writel(readl(MT6797_SPM_CL2_PWR_REQ) | BIT(0), MT6797_SPM_CL2_PWR_REQ);

	/* 2) dummy read of the iDVFS PLL word. */
	readl(MT6797_IDVFS_PLL_DUMMY);

	/* 3) latch the TOPRGU PWRAP-SPI reset (SWSYSRST needs the 0x88 key). */
	writel(readl(MT6797_TOPRGU_SWSYSRST_EN) | MT6797_PWRAP_SPI_RST,
	       MT6797_TOPRGU_SWSYSRST_EN);
	writel((readl(MT6797_TOPRGU_SWSYSRST) | MT6797_PWRAP_SPI_RST) |
	       MT6797_TOPRGU_KEY, MT6797_TOPRGU_SWSYSRST);

	/* 4) set VBUCKB to 1.00 V, enable buck B, settle 1 ms. */
	ret = dm_i2c_reg_write(dev, DA9214_VBUCKB_A, DA9214_VBUCKB_1V00);
	if (ret)
		return ret;
	ret = dm_i2c_reg_write(dev, DA9214_BUCKB_CONT, DA9214_BUCKB_EN);
	if (ret)
		return ret;
	udelay(1000);

	/* 5) clear the B external-buck isolation. */
	writel(readl(MT6797_SPM_CL2_EXT_BUCK_ISO) & ~0x3u,
	       MT6797_SPM_CL2_EXT_BUCK_ISO);

	/* 6) unlatch the PWRAP reset. */
	writel((readl(MT6797_TOPRGU_SWSYSRST) & ~MT6797_PWRAP_SPI_RST) |
	       MT6797_TOPRGU_KEY, MT6797_TOPRGU_SWSYSRST);
	writel(readl(MT6797_TOPRGU_SWSYSRST_EN) & ~MT6797_PWRAP_SPI_RST,
	       MT6797_TOPRGU_SWSYSRST_EN);

	/* 7) set the B SRAM LDO to 1.10 V via the iDVFS SIP, 240 us each side.
	 * The SIP's return convention is undocumented and the vendor path does
	 * not check it, so the call is fire-and-forget; the i2c step above is
	 * the guard against an unpowered cluster.
	 */
	udelay(240);
	arm_smccc_smc(IDVFS_SMC_B_SRAM_LDO, B_SRAM_LDO_1V10,
		      0, 0, 0, 0, 0, 0, &res);
	udelay(240);

	/* 8) pre-configure the MP2 sync DCM (set the field, then toggle TOG). */
	v = (readl(MT6797_MCUCFG_MP2_DCM) & ~0x7fu) | MT6797_MP2_DCM_CFG;
	writel(v | MT6797_MP2_DCM_TOG, MT6797_MCUCFG_MP2_DCM);
	writel(v, MT6797_MCUCFG_MP2_DCM);

	return 0;
}

/*
 * Append " maxcpus=8" to /chosen/bootargs, keeping the A72 cores offline. Used
 * both when the user set no maxcpus token and when a requested A72 power-on
 * failed: a later maxcpus= overrides an earlier one (the kernel parses left to
 * right), so appending 8 caps a user "maxcpus=10" back to the A53s.
 */
static int gemini_append_maxcpus8(void *blob, int chosen, const char *old)
{
	char args[CONFIG_SYS_CBSIZE];
	int ret;

	ret = snprintf(args, sizeof(args), "%s%smaxcpus=8",
		       old ?: "", old ? " " : "");
	/* Never truncate the user's bootargs to fit ours. */
	if (ret >= sizeof(args))
		ret = -FDT_ERR_NOSPACE;
	else
		ret = fdt_setprop_string(blob, chosen, "bootargs", args);
	if (ret < 0)
		printf("gemini: failed to set maxcpus bootarg (%d)\n", ret);

	return ret;
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
 * Also handle the A72 cores (cpu@200/@201, i.e. CPU8/9). They are off at
 * kernel entry; a plain PSCI CPU_ON of an unpowered A72 hangs the SoC, so:
 *   - maxcpus absent or <= 8: keep them offline (append maxcpus=8 if absent);
 *   - maxcpus > 8: the user wants the A72s, so run the power-on sequence here,
 *     and only if it succeeds leave the bootargs so Linux brings them up.
 *     On any failure cap maxcpus back to 8 so Linux never CPU_ONs them.
 * fdt_chosen() has already written the extlinux "append" line into
 * /chosen/bootargs by the time image_setup_libfdt() calls us.
 */
int ft_board_setup(void *blob, struct bd_info *bd)
{
	const void *lk = gd->fdt_blob;
	const char *old;
	int ret, chosen;
	long maxcpus;

	ret = gemini_copy_reservations(blob, lk);
	if (ret < 0)
		return ret;

	/* The stock Android kernel gets LK's /chosen and its own framebuffer
	 * handling; only the Linux boot needs the maxcpus fixup. The
	 * "boot_android" command sets this env flag across its booti call.
	 */
	if (env_get("gemini_android"))
		return 0;

	/*
	 * The simple-framebuffer node sits under /chosen, which carries no
	 * #address-cells/#size-cells of its own. fdt_setup_simplefb_node()
	 * encodes the "reg" from the parent's cell counts: libfdt defaults
	 * #address-cells to 2 but #size-cells to 1, giving a 3-cell reg. The
	 * kernel decodes that reg with of_n_{addr,size}_cells(), which walk up
	 * to the mt6797 root (#address-cells = 2, #size-cells = 2) and need a
	 * 4-cell reg, so of_address_to_resource() returns -EINVAL and simpledrm
	 * fails probe. Make /chosen's cells explicit (2/2) before filling the
	 * node so both sides agree, independent of what the kernel DTB declares.
	 */
	chosen = fdt_path_offset(blob, "/chosen");
	if (chosen >= 0) {
		if (!fdt_getprop(blob, chosen, "#address-cells", NULL))
			fdt_setprop_u32(blob, chosen, "#address-cells", 2);
		if (!fdt_getprop(blob, chosen, "#size-cells", NULL))
			fdt_setprop_u32(blob, chosen, "#size-cells", 2);
	}

	/*
	 * Enable the kernel's simple-framebuffer node and reserve its memory
	 * so simpledrm can hold the MM display clocks and power domain across
	 * the clk_disable_unused() gap until mediatek-drm binds. Fills reg/
	 * format from the live video device; gated on video_is_active().
	 * Non-fatal: a kernel DTB without the node (or inactive video) just
	 * skips it, so older kernels still boot.
	 */
	ret = fdt_simplefb_enable_and_mem_rsv(blob);
	if (ret)
		printf("gemini: simple-framebuffer fixup skipped (%d)\n", ret);

	chosen = fdt_path_offset(blob, "/chosen");
	if (chosen < 0)
		return 0;

	old = fdt_getprop(blob, chosen, "bootargs", NULL);
	maxcpus = gemini_bootargs_maxcpus_value(old);

	if (maxcpus > 8) {
		ret = gemini_cl2_power_on();
		if (ret) {
			printf("gemini: Cortex-A72 power-on failed (%d), capping maxcpus=8\n",
			       ret);
			gemini_append_maxcpus8(blob, chosen, old);
		} else {
			printf("gemini: Cortex-A72 cluster powered on (maxcpus=%ld)\n",
			       maxcpus);
		}
		return 0;
	}

	/* No A72 requested: append our default only if the user set none. */
	if (maxcpus < 0)
		gemini_append_maxcpus8(blob, chosen, old);

	return 0;
}
