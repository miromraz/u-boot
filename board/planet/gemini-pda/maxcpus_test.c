// SPDX-License-Identifier: GPL-2.0
/*
 * Host-side self-check for the Gemini PDA "maxcpus=" token detection in
 * arch/arm/mach-mediatek/mt6797/maxcpus.h. It does NOT build into U-Boot (not
 * listed in any Makefile); it is a standalone host program.
 *
 * Build and run:
 *   cc -I arch/arm/mach-mediatek/mt6797 \
 *      board/planet/gemini-pda/maxcpus_test.c -o /tmp/maxcpus_test
 *   /tmp/maxcpus_test
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include "maxcpus.h"

int main(void)
{
	/* Empty / NULL bootargs: no token -> U-Boot would append maxcpus=8. */
	assert(!gemini_bootargs_has_maxcpus(""));
	assert(!gemini_bootargs_has_maxcpus(NULL));

	/* Real bootargs without maxcpus -> append. */
	assert(!gemini_bootargs_has_maxcpus("console=ttyS0,921600n8 earlycon"));

	/* User already set maxcpus (first, middle, last token) -> leave alone. */
	assert(gemini_bootargs_has_maxcpus("maxcpus=5"));
	assert(gemini_bootargs_has_maxcpus("console=ttyS0 maxcpus=5 ro"));
	assert(gemini_bootargs_has_maxcpus("console=ttyS0 ro maxcpus=5"));
	assert(gemini_bootargs_has_maxcpus("console=ttyS0\tmaxcpus=5"));

	/* A token that merely ends in "maxcpus=" must not count. */
	assert(!gemini_bootargs_has_maxcpus("nomaxcpus=1"));
	assert(!gemini_bootargs_has_maxcpus("console=ttyS0 nomaxcpus=1 ro"));

	printf("maxcpus self-check: all cases passed\n");
	return 0;
}
