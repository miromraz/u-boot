// SPDX-License-Identifier: GPL-2.0
/*
 * Host-side self-check for the Gemini PDA board_get_usable_ram_top() capping
 * logic in arch/arm/mach-mediatek/mt6797/ram_top.h. It does NOT build into
 * U-Boot (not listed in any Makefile); it is a standalone host program.
 *
 * Build and run:
 *   cc -I arch/arm/mach-mediatek/mt6797 \
 *      board/planet/gemini-pda/ram_top_test.c -o /tmp/ram_top_test
 *   /tmp/ram_top_test
 */

#include <stdio.h>
#include <assert.h>
#include "ram_top.h"

static unsigned long long cap(unsigned long long top,
			      const unsigned long long *starts, int n,
			      int *ok)
{
	return gemini_cap_ram_top(top, starts, n, ok);
}

int main(void)
{
	unsigned long long r;
	int ok;

	/* Representative LK region bases observed/expected on the Gemini. */
	const unsigned long long fb_only[]	= { 0x7dfb0000ULL };
	const unsigned long long fb_mblock[]	= { 0x7dfb0000ULL, 0xbfa00000ULL };
	const unsigned long long straddle[]	= { 0xff000000ULL };
	const unsigned long long low[]		= { 0x50000000ULL };

	/* No regions == no usable FDT data -> safe fallback, flagged not-ok. */
	r = cap(0x140000000ULL, NULL, 0, &ok);
	assert(r == GEMINI_RAM_FALLBACK && !ok);

	/* fb only: cap exactly at the framebuffer base, below 4 GiB. */
	r = cap(0x140000000ULL, fb_only, 1, &ok);
	assert(r == 0x7dfb0000ULL && ok);

	/* fb + a higher mblock: the lower region (fb) wins. */
	r = cap(0x140000000ULL, fb_mblock, 2, &ok);
	assert(r == 0x7dfb0000ULL && ok);

	/* Region straddling 4 GiB: 4 GiB cap first, then down to its base. */
	r = cap(0x140000000ULL, straddle, 1, &ok);
	assert(r == 0xff000000ULL && ok);

	/* A region inside the load window forces the fallback (too low). */
	r = cap(0x140000000ULL, low, 1, &ok);
	assert(r == GEMINI_RAM_FALLBACK && !ok);

	/* DRAM that ends below 4 GiB with no high regions stays put. */
	r = cap(0x80000000ULL, fb_only, 1, &ok);
	assert(r == 0x7dfb0000ULL && ok);

	printf("ram_top self-check: all cases passed\n");
	return 0;
}
