// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6797 SSUSB clock gates.
 *
 * U-Boot only drives the USB (SSUSB/MTU3) block on the Planet Gemini PDA, so
 * this is deliberately not a full MT6797 clock driver: it provides just the
 * three infracfg gates the MTU3 controller consumes (sys_ck, ref_ck, mcu_ck)
 * and reparents the one topckgen mux that feeds their source. There is no PLL,
 * factor or full mux modelling, and the clock IDs below are local to U-Boot's
 * control device tree, not the Linux mt6797-clk.h numbering.
 *
 * Register layout mirrors the Linux MT6797 clock driver:
 *   infracfg (0x10001000) ICG2: set 0xa8, clr 0xac, sta 0xb0
 *     CLK_INFRA_SSUSB_SYS bit 24, CLK_INFRA_SSUSB_REF bit 9,
 *     CLK_INFRA_SSUSB_BUS bit 18
 *   topckgen (0x10000000) 0xb0 bit 8: ssusb_top_sys_sel
 *     (0 = clk26m, 1 = univpll3_d2)
 */

#include <dm.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <power/pmic.h>

#include "clk-mtk.h"

/* Local clock IDs, matching the <&ssusb_cg N> cells in the control DT. */
#define CLK_SSUSB_SYS		0
#define CLK_SSUSB_REF		1
#define CLK_SSUSB_BUS		2

#define TOPCKGEN_BASE		0x10000000
#define TOP_SSUSB_TOP_SYS_SEL	0xb0
#define SSUSB_TOP_SYS_UNIVPLL3	BIT(8)

/*
 * APMIXEDSYS USB 26 MHz reference clock, enabled by LK in
 * enable_ssusb_xtal_clock() (platform/mt6757/mt_usbphy.c). The T-PHY has no
 * clock of its own; without this the SSUSB PLL never locks.
 */
/*
 * MT6351 PMIC registers (reached over the pwrap PMIC uclass) for the LDO rails
 * that supply the USB U2 PHY. LK's mt_usb_phy_recover()/phy_init_soc() turn
 * these on before touching the PHY, while LK's savecurrent() leaves them off on
 * the way out, so the PHY register bus is unclocked and T-PHY writes are
 * dropped until they are re-enabled. This CG probe runs before the T-PHY, so it
 * is the earliest MT6797-scoped hook. Addresses and bit fields from the stock
 * 3.18 tree drivers/misc/mediatek/include/mt-plat/mt6797/include/mach/upmu_hw.h
 * (VUSB33_CON0:460, RG_VUSB33_EN:6384-6386; VA10_CON0:504, RG_VA10_EN:7128-7130;
 * VA10_ANA_CON0:585, RG_VA10_VOSEL:7992-7994).
 */
#define MT6351_LDO_VUSB33_CON0		0x0a16
#define MT6351_RG_VUSB33_EN		BIT(1)
#define MT6351_LDO_VA10_CON0		0x0a6e
#define MT6351_RG_VA10_EN		BIT(1)
#define MT6351_VA10_ANA_CON0		0x0b10
#define MT6351_RG_VA10_VOSEL		GENMASK(10, 8)	/* 0x1 = 0.95V */

#define APMIXED_BASE			0x1000c000
#define AP_PLL_CON0			0x0
#define AP_PLL_CON2			0x8
#define PLL_CON0_LTECLKSQ_EN		BIT(0)	/* RG_LTECLKSQ_EN */
#define PLL_CON0_LTECLKSQ_LPF_EN	BIT(1)	/* RG_LTECLKSQ_LPF_EN */
#define PLL_CON2_REF2USB_TX_EN		BIT(0)	/* DA_REF2USB_TX_EN */
#define PLL_CON2_REF2USB_TX_LPF_EN	BIT(1)	/* DA_REF2USB_TX_LPF_EN */
#define PLL_CON2_REF2USB_TX_OUT_EN	BIT(2)	/* DA_REF2USB_TX_OUT_EN */

static const struct mtk_gate_regs infra_cg2 = {
	.set_ofs = 0xa8,
	.clr_ofs = 0xac,
	.sta_ofs = 0xb0,
};

/* parent is -1: only clk_enable_bulk() is used, so rate lookup is never run. */
#define GATE_SSUSB(_id, _shift) {			\
		.id = _id,				\
		.parent = -1,				\
		.regs = &infra_cg2,			\
		.shift = _shift,			\
		.flags = CLK_GATE_SETCLR,		\
	}

static const struct mtk_gate ssusb_gates[] = {
	GATE_SSUSB(CLK_SSUSB_SYS, 24),
	GATE_SSUSB(CLK_SSUSB_REF, 9),
	GATE_SSUSB(CLK_SSUSB_BUS, 18),
};

static const struct mtk_clk_tree mt6797_ssusb_cg_tree = {
	.gates_offs = 0,
	.gates = ssusb_gates,
	.num_gates = ARRAY_SIZE(ssusb_gates),
};

static int mt6797_ssusb_cg_probe(struct udevice *dev)
{
	struct udevice *pmic;
	int ret;

	ret = mtk_clk_probe(dev);
	if (ret)
		return ret;

	/*
	 * Enable the USB 26 MHz reference clock in APMIXEDSYS, mirroring LK's
	 * enable_ssusb_xtal_clock(): AP_PLL_CON0 RG_LTECLKSQ_EN then (after the
	 * PLL settles) LPF_EN, then AP_PLL_CON2 DA_REF2USB_TX_EN/_LPF_EN/_OUT_EN.
	 * This clock gate is probed when the MTU3 glue enables its clocks, which
	 * is before the T-PHY touches any SSUSB MMIO.
	 */
	setbits_le32((void __iomem *)(uintptr_t)(APMIXED_BASE + AP_PLL_CON0),
		     PLL_CON0_LTECLKSQ_EN);
	udelay(100);
	setbits_le32((void __iomem *)(uintptr_t)(APMIXED_BASE + AP_PLL_CON0),
		     PLL_CON0_LTECLKSQ_LPF_EN);
	setbits_le32((void __iomem *)(uintptr_t)(APMIXED_BASE + AP_PLL_CON2),
		     PLL_CON2_REF2USB_TX_EN);
	udelay(100);
	setbits_le32((void __iomem *)(uintptr_t)(APMIXED_BASE + AP_PLL_CON2),
		     PLL_CON2_REF2USB_TX_LPF_EN);
	setbits_le32((void __iomem *)(uintptr_t)(APMIXED_BASE + AP_PLL_CON2),
		     PLL_CON2_REF2USB_TX_OUT_EN);

	/*
	 * Reparent ssusb_top_sys_sel to univpll3_d2, mirroring the Linux
	 * assigned-clock-parents for this board; clk26m alone does not clock
	 * the SSUSB system domain fast enough for enumeration. topckgen is a
	 * separate register block from this driver's infracfg base, so poke it
	 * directly rather than modelling the full mux tree.
	 * TODO(hw-verify): assumes LK leaves UNIVPLL (hence univpll3_d2)
	 * enabled; confirm the mux lands on univpll3_d2 on real hardware.
	 */
	setbits_le32((void __iomem *)(uintptr_t)(TOPCKGEN_BASE +
			TOP_SSUSB_TOP_SYS_SEL), SSUSB_TOP_SYS_UNIVPLL3);

	/*
	 * Enable the MT6351 USB PHY rails (VUSB33, VA10 at 0.95V) and wait 50us
	 * for them to settle, mirroring LK phy_init_soc()/mt_usb_phy_recover()
	 * (platform/mt6797/mt_usbphy.c:507-534, :900-927). Without this the PHY
	 * is held powered down and T-PHY register writes are dropped.
	 */
	ret = uclass_first_device_err(UCLASS_PMIC, &pmic);
	if (ret) {
		printf("BC cg: pmic not found (%d), USB PHY rails left off\n", ret);
	} else {
		pmic_clrsetbits(pmic, MT6351_LDO_VUSB33_CON0, 0, MT6351_RG_VUSB33_EN);
		pmic_clrsetbits(pmic, MT6351_LDO_VA10_CON0, 0, MT6351_RG_VA10_EN);
		pmic_clrsetbits(pmic, MT6351_VA10_ANA_CON0, MT6351_RG_VA10_VOSEL,
				0x1 << 8);
		udelay(50);
		printf("BC cg: pmic USB PHY rails on\n");
	}

	return 0;
}

static const struct udevice_id mt6797_ssusb_cg_ids[] = {
	{
		.compatible = "mediatek,mt6797-ssusb-cg",
		.data = (ulong)&mt6797_ssusb_cg_tree,
	},
	{ }
};

U_BOOT_DRIVER(mt6797_ssusb_cg) = {
	.name = "mt6797-ssusb-cg",
	.id = UCLASS_CLK,
	.of_match = mt6797_ssusb_cg_ids,
	.probe = mt6797_ssusb_cg_probe,
	.priv_auto = sizeof(struct mtk_clk_priv),
	.ops = &mtk_clk_infrasys_ops,
	.flags = DM_FLAG_PRE_RELOC,
};
