// SPDX-License-Identifier: GPL-2.0+
/*
 * Fastboot flash/erase partition allowlist for the Planet Gemini PDA.
 *
 * U-Boot is chainloaded from the vendor LK and shares the eMMC with the
 * preloader, LK, TEE and the modem/IMEI (nvram) partitions. Overwriting any
 * of those can hard-brick the device or destroy the IMEI, so restrict
 * "fastboot flash" and "fastboot erase" to the partitions it is safe to
 * rewrite for mainline experiments. Everything else is refused.
 *
 * "fastboot oem format" (GPT rewrite) and whole-eMMC erase are disabled at
 * the Kconfig level by not enabling FASTBOOT_CMD_OEM_FORMAT /
 * FASTBOOT_MMC_USER_SUPPORT in the defconfig.
 *
 * gemini_fastboot_part_allowed() is a pure predicate so it can be exercised
 * by a host-side unit test; define FASTBOOT_ALLOWLIST_HOST_TEST to compile it
 * without the U-Boot environment.
 */

#ifdef FASTBOOT_ALLOWLIST_HOST_TEST
#include <string.h>
#include <stdbool.h>
#else
#include <fastboot.h>
#include <linux/errno.h>
#include <linux/string.h>
#endif

/* NULL-terminated so no ARRAY_SIZE dependency in the host test. */
static const char * const gemini_fastboot_allowlist[] = {
	"boot",	/* U-Boot itself; recoverable via BROM/mtkclient */
	"boot2",
	"boot3",
	"linux",
	"userdata",
	NULL,
};

bool gemini_fastboot_part_allowed(const char *part)
{
	int i;

	if (!part)
		return false;

	for (i = 0; gemini_fastboot_allowlist[i]; i++)
		if (!strcmp(part, gemini_fastboot_allowlist[i]))
			return true;

	return false;
}

#ifndef FASTBOOT_ALLOWLIST_HOST_TEST
int fastboot_board_flash_allowed(const char *partition, char *response)
{
	if (gemini_fastboot_part_allowed(partition))
		return 0;

	fastboot_fail("partition is not in the Gemini flash allowlist",
		      response);
	return -EPERM;
}
#endif
