/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Pure capping logic for board_get_usable_ram_top() on the Planet Gemini PDA,
 * split into this header so a host-side self-check can exercise it without the
 * U-Boot/FDT plumbing. See board/planet/gemini-pda/ram_top_test.c.
 */

#ifndef __MT6797_RAM_TOP_H
#define __MT6797_RAM_TOP_H

/* Usable RAM must stay above the static pre-LK carve-outs (they end here). */
#define GEMINI_RAM_FLOOR	0x46400000ULL
/*
 * ...and leave at least 64 MiB above the top of the load-address window in
 * include/configs/gemini-pda.h (ramdisk_addr_r = 0x50000000).
 */
#define GEMINI_RAM_MIN_TOP	0x54000000ULL
/* The MSDC (eMMC) DMA is 32-bit, so never hand out memory at/above 4 GiB. */
#define GEMINI_RAM_4GIB		0x100000000ULL
/* Safe cap, below the observed LK framebuffer, when the FDT gives us nothing. */
#define GEMINI_RAM_FALLBACK	0x7c000000ULL

/*
 * Lower @top below the start of every reserved region (the LK retained
 * framebuffer and each /reserved-memory entry) that begins above
 * GEMINI_RAM_FLOOR and below the running top, iterating until none remain.
 * @starts/@n list the region base addresses.
 *
 * Returns the capped top. Falls back to GEMINI_RAM_FALLBACK when there is no
 * usable FDT data (@n <= 0) or the cap would drop below GEMINI_RAM_MIN_TOP;
 * *@ok (if non-NULL) reports whether the returned top came out sane.
 */
static inline unsigned long long
gemini_cap_ram_top(unsigned long long top, const unsigned long long *starts,
		   int n, int *ok)
{
	int changed, i;

	if (n <= 0) {
		if (ok)
			*ok = 0;
		return GEMINI_RAM_FALLBACK;
	}

	if (top > GEMINI_RAM_4GIB)
		top = GEMINI_RAM_4GIB;

	do {
		changed = 0;
		for (i = 0; i < n; i++) {
			if (starts[i] > GEMINI_RAM_FLOOR && starts[i] < top) {
				top = starts[i];
				changed = 1;
			}
		}
	} while (changed);

	if (top < GEMINI_RAM_MIN_TOP) {
		if (ok)
			*ok = 0;
		return GEMINI_RAM_FALLBACK;
	}

	if (ok)
		*ok = 1;
	return top;
}

#endif /* __MT6797_RAM_TOP_H */
