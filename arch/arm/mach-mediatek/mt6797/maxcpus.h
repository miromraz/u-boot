/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Pure bootargs "maxcpus=" detection for the Planet Gemini PDA, split into
 * this header so a host-side self-check can exercise it without the U-Boot/FDT
 * plumbing. See board/planet/gemini-pda/maxcpus_test.c.
 *
 * The caller must have a string header in scope (strstr): <linux/string.h> in
 * U-Boot, <string.h> on the host.
 */

#ifndef __MT6797_MAXCPUS_H
#define __MT6797_MAXCPUS_H

/*
 * Return true if @args carries a "maxcpus=" token on a token boundary, i.e.
 * at the start of the string or right after whitespace. This way a substring
 * match such as "nomaxcpus=1" does not count as a user maxcpus setting.
 */
static inline bool gemini_bootargs_has_maxcpus(const char *args)
{
	const char *p = args;

	if (!p)
		return false;

	while ((p = strstr(p, "maxcpus=")) != NULL) {
		if (p == args || p[-1] == ' ' || p[-1] == '\t')
			return true;
		p += 8;
	}

	return false;
}

#endif /* __MT6797_MAXCPUS_H */
