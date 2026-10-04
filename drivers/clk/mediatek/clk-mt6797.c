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

#include "clk-mtk.h"

/* Local clock IDs, matching the <&ssusb_cg N> cells in the control DT. */
#define CLK_SSUSB_SYS		0
#define CLK_SSUSB_REF		1
#define CLK_SSUSB_BUS		2

#define TOPCKGEN_BASE		0x10000000
#define TOP_SSUSB_TOP_SYS_SEL	0xb0
#define SSUSB_TOP_SYS_UNIVPLL3	BIT(8)

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
	int ret;

	ret = mtk_clk_probe(dev);
	if (ret)
		return ret;

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
