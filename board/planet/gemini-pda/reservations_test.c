// SPDX-License-Identifier: GPL-2.0
/*
 * Host-side self-check for the Gemini PDA reservation copy in
 * arch/arm/mach-mediatek/mt6797/reservations.h. It does NOT build into U-Boot
 * (not listed in any Makefile); it is a standalone host program compiled
 * against U-Boot's bundled libfdt.
 *
 * Build and run (needs dtc on PATH):
 *   dtc -I dts -O dtb -o /tmp/lk.dtb board/planet/gemini-pda/reservations_lk.dts
 *   dtc -I dts -O dtb -o /tmp/os.dtb board/planet/gemini-pda/reservations_os.dts
 *   cc -I scripts/dtc/libfdt -I arch/arm/mach-mediatek/mt6797 \
 *      board/planet/gemini-pda/reservations_test.c scripts/dtc/libfdt/fdt*.c \
 *      -o /tmp/reservations_test
 *   /tmp/reservations_test /tmp/lk.dtb /tmp/os.dtb
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <libfdt.h>
#include "reservations.h"

static void *load(const char *path)
{
	FILE *f = fopen(path, "rb");
	void *buf;
	long sz;

	assert(f);
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc(sz);
	assert(fread(buf, 1, sz, f) == (size_t)sz);
	fclose(f);
	assert(fdt_check_header(buf) == 0);
	return buf;
}

/* Count /reserved-memory children whose first reg window == (start, size). */
static int count_node(const void *fdt, uint64_t start, uint64_t size)
{
	int parent = fdt_subnode_offset(fdt, 0, "reserved-memory");
	int node, hits = 0;

	if (parent < 0)
		return 0;
	fdt_for_each_subnode(node, fdt, parent) {
		const fdt32_t *reg;
		int len;
		uint64_t a, s;

		reg = fdt_getprop(fdt, node, "reg", &len);
		if (!reg || len < 4 * (int)sizeof(fdt32_t))
			continue;
		a = ((uint64_t)fdt32_to_cpu(reg[0]) << 32) | fdt32_to_cpu(reg[1]);
		s = ((uint64_t)fdt32_to_cpu(reg[2]) << 32) | fdt32_to_cpu(reg[3]);
		if (a == start && s == size)
			hits++;
	}
	return hits;
}

static bool node_has_nomap(const void *fdt, uint64_t start)
{
	int parent = fdt_subnode_offset(fdt, 0, "reserved-memory");
	int node;

	fdt_for_each_subnode(node, fdt, parent) {
		const fdt32_t *reg = fdt_getprop(fdt, node, "reg", NULL);

		if (reg && (((uint64_t)fdt32_to_cpu(reg[0]) << 32) |
			    fdt32_to_cpu(reg[1])) == start)
			return fdt_getprop(fdt, node, "no-map", NULL) != NULL;
	}
	return false;
}

static int count_memrsv(const void *fdt, uint64_t start, uint64_t size)
{
	int i, total = fdt_num_mem_rsv(fdt), hits = 0;

	for (i = 0; i < total; i++) {
		uint64_t a, s;

		if (fdt_get_mem_rsv(fdt, i, &a, &s) == 0 && a == start &&
		    s == size)
			hits++;
	}
	return hits;
}

int main(int argc, char **argv)
{
	static char osbuf[65536];
	void *lk, *os;

	assert(argc == 3);
	lk = load(argv[1]);
	os = load(argv[2]);
	assert(fdt_open_into(os, osbuf, sizeof(osbuf)) == 0);

	assert(gemini_copy_reservations(osbuf, lk) == 0);

	/* nodes copied from LK, exactly once each, no-map preserved */
	assert(count_node(osbuf, 0xbe000000ULL, 0xc00000ULL) == 1);
	assert(node_has_nomap(osbuf, 0xbe000000ULL));
	assert(count_node(osbuf, 0x7ff40000ULL, 0x40000ULL) == 1);	/* atf-log */
	assert(count_node(osbuf, 0x7dfb0000ULL, 0x1f90000ULL) == 1);	/* fb */
	assert(count_node(osbuf, 0xbfa00000ULL, 0x100000ULL) == 1);	/* mblock */

	/* compatible string carried over */
	{
		int n = fdt_subnode_offset(osbuf, 0, "reserved-memory");
		int t = fdt_subnode_offset(osbuf, n, "trusty@be000000");
		assert(t >= 0);
		assert(fdt_getprop(osbuf, t, "compatible", NULL) != NULL);
	}

	/* OS node left intact, still exactly once */
	assert(count_node(osbuf, 0x46000000ULL, 0x400000ULL) == 1);
	/* LK node overlapping the OS one was skipped: no dup@46000000 */
	assert(fdt_subnode_offset(osbuf,
		fdt_subnode_offset(osbuf, 0, "reserved-memory"),
		"dup@46000000") < 0);

	/* memreserve: LK's new entry added, the one already in OS not doubled */
	assert(count_memrsv(osbuf, 0xb4000000ULL, 0xa000000ULL) == 1);
	assert(count_memrsv(osbuf, 0x7ff80000ULL, 0x80000ULL) == 1);

	/* idempotent: a second copy adds nothing */
	assert(gemini_copy_reservations(osbuf, lk) == 0);
	assert(count_node(osbuf, 0x7dfb0000ULL, 0x1f90000ULL) == 1);
	assert(count_memrsv(osbuf, 0xb4000000ULL, 0xa000000ULL) == 1);

	printf("reservations self-check: all cases passed\n");
	return 0;
}
