// SPDX-License-Identifier: GPL-2.0+
/*
 * Planet Gemini PDA built-in QWERTY keyboard.
 *
 * The keys form an 8x7 matrix wired to an AW9523B GPIO expander on I2C5 at
 * address 0x5b: port 0 (pins 0-7) are the eight rows, port 1 bits 0-6 (pins
 * 8-14) are the seven columns. The matrix is polled: each column is driven low
 * in turn (the others high) and the row inputs are read back; a row bit that
 * reads low is a pressed key at that (row, column). Decoding to key codes is
 * done with the generic key_matrix helper from the "linux,keymap" in the DT.
 *
 * LK leaves the AW9523 reset (SoC GPIO58) as an input and leaves the chip
 * running, so U-Boot only configures the expander's GPIO/direction registers
 * and never touches the reset line.
 *
 * TODO(hw-verify): the AW9523 reset is assumed to be held deasserted by LK /
 * the board (GPIO58 is left as an input); if the expander does not respond,
 * GPIO58 may need to be driven to release reset.
 */

#include <dm.h>
#include <i2c.h>
#include <input.h>
#include <keyboard.h>
#include <key_matrix.h>
#include <stdio_dev.h>
#include <linux/bitops.h>
#include <linux/delay.h>

/* AW9523B registers */
#define AW9523_IN_P0		0x00	/* row inputs */
#define AW9523_OUT_P1		0x03	/* column outputs */
#define AW9523_CONF_P0		0x04	/* direction, 1 = input */
#define AW9523_CONF_P1		0x05
#define AW9523_LEDMODE_P0	0x12	/* per-pin, 1 = GPIO, 0 = LED */
#define AW9523_LEDMODE_P1	0x13

#define GEMINI_MAX_KEYS		8	/* simultaneous keys to decode */
#define COL_SETTLE_US		20	/* let the column/row lines settle */

#define KBD_REPEAT_DELAY_MS	250
#define KBD_REPEAT_RATE_MS	40

struct gemini_kbd_priv {
	struct input_config *input;
	struct key_matrix matrix;
};

/*
 * Decode one column's row byte into pressed (row, col) entries. Rows are
 * active-low: a cleared bit is a pressed key. Appends to keys[] starting at
 * nkeys and returns the new count (bounded by max). Pure, so it is unit
 * tested on the host.
 */
static int gemini_scan_col(u8 rowbits, int col, int nrows,
			   struct key_matrix_key *keys, int nkeys, int max)
{
	int row;

	for (row = 0; row < nrows && nkeys < max; row++) {
		if (rowbits & BIT(row))
			continue;	/* high = not pressed */
		keys[nkeys].row = row;
		keys[nkeys].col = col;
		keys[nkeys].valid = 1;
		nkeys++;
	}

	return nkeys;
}

static int gemini_kbd_check(struct input_config *input)
{
	struct udevice *dev = input->dev;
	struct gemini_kbd_priv *priv = dev_get_priv(dev);
	struct key_matrix_key keys[GEMINI_MAX_KEYS];
	int keycodes[GEMINI_MAX_KEYS];
	int nkeys = 0, ncodes, col, ret;

	for (col = 0; col < priv->matrix.num_cols; col++) {
		/* Drive the selected column low, all other columns high. */
		ret = dm_i2c_reg_write(dev, AW9523_OUT_P1, 0xff & ~BIT(col));
		if (ret)
			return 0;
		udelay(COL_SETTLE_US);

		ret = dm_i2c_reg_read(dev, AW9523_IN_P0);
		if (ret < 0)
			return 0;

		nkeys = gemini_scan_col(ret, col, priv->matrix.num_rows,
					keys, nkeys, GEMINI_MAX_KEYS);
	}

	ncodes = key_matrix_decode(&priv->matrix, keys, nkeys, keycodes,
				   GEMINI_MAX_KEYS);
	input_send_keycodes(input, keycodes, ncodes);

	return 1;
}

static int gemini_kbd_init(struct udevice *dev)
{
	int ret;

	/* Take both ports out of LED mode so they act as GPIOs. */
	ret = dm_i2c_reg_write(dev, AW9523_LEDMODE_P0, 0xff);
	if (ret)
		return ret;
	ret = dm_i2c_reg_write(dev, AW9523_LEDMODE_P1, 0xff);
	if (ret)
		return ret;

	/* Rows (port 0) all inputs; columns (port 1 bits 0-6) outputs. */
	ret = dm_i2c_reg_write(dev, AW9523_CONF_P0, 0xff);
	if (ret)
		return ret;
	ret = dm_i2c_reg_write(dev, AW9523_CONF_P1, 0x80);
	if (ret)
		return ret;

	/* Idle with every column driven high. */
	return dm_i2c_reg_write(dev, AW9523_OUT_P1, 0xff);
}

static int gemini_kbd_probe(struct udevice *dev)
{
	struct gemini_kbd_priv *priv = dev_get_priv(dev);
	struct keyboard_priv *uc_priv = dev_get_uclass_priv(dev);
	struct stdio_dev *sdev = &uc_priv->sdev;
	struct input_config *input = &uc_priv->input;
	int rows, cols, ret;

	rows = dev_read_u32_default(dev, "keypad,num-rows", 0);
	cols = dev_read_u32_default(dev, "keypad,num-columns", 0);
	if (!rows || !cols)
		return -EINVAL;

	ret = key_matrix_init(&priv->matrix, rows, cols, false);
	if (ret)
		return ret;
	ret = key_matrix_decode_fdt(dev, &priv->matrix);
	if (ret)
		return ret;

	ret = gemini_kbd_init(dev);
	if (ret)
		return ret;

	priv->input = input;
	input->dev = dev;
	input_set_delays(input, KBD_REPEAT_DELAY_MS, KBD_REPEAT_RATE_MS);
	input_add_tables(input, false);
	input->read_keys = gemini_kbd_check;
	strcpy(sdev->name, "gemini-kbd");

	return input_stdio_register(sdev);
}

static const struct keyboard_ops gemini_kbd_ops = {
};

static const struct udevice_id gemini_kbd_ids[] = {
	{ .compatible = "planet,gemini-keyboard" },
	{ }
};

U_BOOT_DRIVER(gemini_keyboard) = {
	.name		= "gemini_keyboard",
	.id		= UCLASS_KEYBOARD,
	.of_match	= gemini_kbd_ids,
	.ops		= &gemini_kbd_ops,
	.priv_auto	= sizeof(struct gemini_kbd_priv),
	.probe		= gemini_kbd_probe,
};
