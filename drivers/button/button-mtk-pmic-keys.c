// SPDX-License-Identifier: GPL-2.0+
/*
 * Button driver for the power/home keys wired to a MediaTek PMIC.
 *
 * On MT6797 the keys hang off the MT6351 PMIC, reached through the PMIC
 * wrapper (mtk-pwrap). Their pressed state is a bit in the PMIC TOPSTATUS
 * register, read over the wrapper's WACS2 interface via the PMIC uclass.
 *
 * Each key is a child node ("power", "home", ...) carrying a label, a
 * linux,code and a mediatek,key-mask (the TOPSTATUS bit). The bits are
 * active-low: a cleared bit means the key is pressed (matching the Linux
 * drivers/input/keyboard/mtk-pmic-keys.c "pressed = !key_deb" logic).
 *
 * Bound by mtk-pwrap for an "mediatek,mt6351" PMIC's "keys" subnode.
 */

#include <button.h>
#include <dm.h>
#include <dm/lists.h>
#include <log.h>
#include <power/pmic.h>

/* MT6351 TOPSTATUS: PWRKEY = bit 0x2, HOMEKEY = bit 0x4 (both active-low). */
#define MT6351_TOPSTATUS	0x220

struct mtk_pmic_key_priv {
	struct udevice *pmic;
	u32 mask;
	int linux_code;
};

/*
 * Pure decode so it can be unit-tested on the host: TOPSTATUS bits are
 * active-low, so a cleared masked bit means the key is pressed.
 */
static enum button_state_t mtk_pmic_key_decode(u32 topstatus, u32 mask)
{
	return (topstatus & mask) ? BUTTON_OFF : BUTTON_ON;
}

static enum button_state_t mtk_pmic_key_get_state(struct udevice *dev)
{
	struct mtk_pmic_key_priv *priv = dev_get_priv(dev);
	int val;

	if (!priv || !priv->pmic)
		return -ENODATA;

	val = pmic_reg_read(priv->pmic, MT6351_TOPSTATUS);
	if (val < 0) {
		/*
		 * enum button_state_t has no error value, so a negative errno
		 * would be misread as a state change and inject a phantom key.
		 * Report the key as released instead.
		 */
		log_debug("PMIC TOPSTATUS read failed: %d\n", val);
		return BUTTON_OFF;
	}

	return mtk_pmic_key_decode(val, priv->mask);
}

static int mtk_pmic_key_get_code(struct udevice *dev)
{
	struct mtk_pmic_key_priv *priv = dev_get_priv(dev);

	if (!priv || !priv->linux_code)
		return -ENODATA;

	return priv->linux_code;
}

static int mtk_pmic_key_probe(struct udevice *dev)
{
	struct button_uc_plat *uc_plat = dev_get_uclass_plat(dev);
	struct mtk_pmic_key_priv *priv = dev_get_priv(dev);
	struct udevice *p;
	int ret;

	/* The top-level "keys" node has no label; nothing to probe. */
	if (!uc_plat->label)
		return 0;

	for (p = dev_get_parent(dev); p; p = dev_get_parent(p)) {
		if (device_get_uclass_id(p) == UCLASS_PMIC) {
			priv->pmic = p;
			break;
		}
	}
	if (!priv->pmic)
		return -ENODEV;

	ret = dev_read_u32(dev, "mediatek,key-mask", &priv->mask);
	if (ret)
		return ret;

	return dev_read_u32(dev, "linux,code", &priv->linux_code);
}

static int mtk_pmic_key_bind(struct udevice *parent)
{
	struct udevice *dev;
	ofnode node;
	int ret;

	dev_for_each_subnode(node, parent) {
		struct button_uc_plat *uc_plat;
		const char *label;

		label = ofnode_read_string(node, "label");
		if (!label) {
			debug("%s: node %s has no label\n", __func__,
			      ofnode_get_name(node));
			return -EINVAL;
		}
		ret = device_bind_driver_to_node(parent, "mtk_pmic_keys",
						 ofnode_get_name(node),
						 node, &dev);
		if (ret)
			return ret;
		uc_plat = dev_get_uclass_plat(dev);
		uc_plat->label = label;
	}

	return 0;
}

static const struct button_ops mtk_pmic_key_ops = {
	.get_state	= mtk_pmic_key_get_state,
	.get_code	= mtk_pmic_key_get_code,
};

U_BOOT_DRIVER(mtk_pmic_keys) = {
	.name		= "mtk_pmic_keys",
	.id		= UCLASS_BUTTON,
	.ops		= &mtk_pmic_key_ops,
	.priv_auto	= sizeof(struct mtk_pmic_key_priv),
	.bind		= mtk_pmic_key_bind,
	.probe		= mtk_pmic_key_probe,
};
