/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copy every memory reservation LK made in its control FDT into the OS device
 * tree, so Linux never reuses firmware memory (TEE/ATF/modem/LK framebuffer and
 * logs). LK records these as /memreserve/ header entries (Linux honours them
 * via memblock_reserve(), drivers/of/fdt.c early_init_fdt_scan_reserved_mem)
 * and as /reserved-memory child nodes, the no-map ones becoming memblock_nomap
 * (drivers/of/of_reserved_mem.c). U-Boot's generic code only feeds LK's
 * reservations to its own LMB allocator (boot/image-fdt.c
 * boot_fdt_handle_mem_rsv_regions); it never writes them into the OS blob.
 *
 * Split into this header so a host-side self-check can exercise the pure FDT
 * copy without the U-Boot plumbing. See board/planet/gemini-pda/
 * reservations_test.c. Relies on the includer for libfdt, string.h and the
 * uint64_t/bool types (as ram_top.h / maxcpus.h do).
 */

#ifndef __MT6797_RESERVATIONS_H
#define __MT6797_RESERVATIONS_H

/*
 * Is [start, start+size) fully inside the reg of one /reserved-memory child in
 * @blob? Used to skip reservations the OS DT (or an earlier copy) already
 * covers. Only full containment counts: skipping on mere overlap would leave
 * the uncovered part of a larger LK region unreserved, while Linux accepts
 * overlapping reservations (memblock_reserve/memblock_mark_nomap).
 */
static inline bool gemini_rsv_covered(const void *blob, uint64_t start,
				       uint64_t size)
{
	int parent, node, ac, sc, stride;

	if (!size)
		return false;
	parent = fdt_subnode_offset(blob, 0, "reserved-memory");
	if (parent < 0)
		return false;
	ac = fdt_address_cells(blob, parent);
	sc = fdt_size_cells(blob, parent);
	if (ac < 1 || sc < 1)
		return false;
	stride = ac + sc;

	fdt_for_each_subnode(node, blob, parent) {
		const fdt32_t *reg;
		int len, i, c;

		reg = fdt_getprop(blob, node, "reg", &len);
		if (!reg)
			continue;
		len /= sizeof(fdt32_t);
		for (i = 0; i + stride <= len; i += stride) {
			uint64_t a = 0, s = 0;

			for (c = 0; c < ac; c++)
				a = (a << 32) | fdt32_to_cpu(reg[i + c]);
			for (c = 0; c < sc; c++)
				s = (s << 32) | fdt32_to_cpu(reg[i + ac + c]);
			if (s && start >= a && start + size <= a + s)
				return true;
		}
	}
	return false;
}

/*
 * Copy all of @src's reservations into @dst, skipping any already present:
 *   - /reserved-memory children, verbatim (reg, no-map, reusable, compatible),
 *     skipped when @dst has a child of the same name or one that covers it;
 *   - /memreserve/ entries, skipped when @dst already has an identical entry or
 *     a reserved-memory node covering the range.
 * Nodes are copied before /memreserve/ so an LK entry that duplicates a just
 * copied node is dropped. A missing /reserved-memory node or empty
 * /memreserve/ in @src is fine (nothing to copy). Returns 0, or a negative
 * libfdt error only on a real write failure (e.g. no space in @dst).
 */
static inline int gemini_copy_reservations(void *dst, const void *src)
{
	int src_rm, dst_rm, node, i, total;

	src_rm = fdt_subnode_offset(src, 0, "reserved-memory");
	if (src_rm >= 0) {
		int sac = fdt_address_cells(src, src_rm);
		int ssc = fdt_size_cells(src, src_rm);
		int stride = sac + ssc;

		dst_rm = fdt_subnode_offset(dst, 0, "reserved-memory");
		if (dst_rm < 0) {
			dst_rm = fdt_add_subnode(dst, 0, "reserved-memory");
			if (dst_rm < 0)
				return dst_rm;
			fdt_setprop_u32(dst, dst_rm, "#address-cells", 2);
			fdt_setprop_u32(dst, dst_rm, "#size-cells", 2);
			fdt_setprop_empty(dst, dst_rm, "ranges");
		}

		fdt_for_each_subnode(node, src, src_rm) {
			const char *name = fdt_get_name(src, node, NULL);
			const fdt32_t *reg;
			int len, dst_node, prop;
			bool skip = false;

			if (!name)
				continue;
			if (fdt_subnode_offset(dst, dst_rm, name) >= 0)
				continue;

			reg = fdt_getprop(src, node, "reg", &len);
			if (reg && sac >= 1 && stride >= 1) {
				int l = len / sizeof(fdt32_t), k, c;

				for (k = 0; k + stride <= l && !skip;
				     k += stride) {
					uint64_t a = 0, s = 0;

					for (c = 0; c < sac; c++)
						a = (a << 32) |
						    fdt32_to_cpu(reg[k + c]);
					for (c = 0; c < ssc; c++)
						s = (s << 32) |
						    fdt32_to_cpu(reg[k + sac + c]);
					if (gemini_rsv_covered(dst, a, s))
						skip = true;
				}
			}
			if (skip)
				continue;

			dst_node = fdt_add_subnode(dst, dst_rm, name);
			if (dst_node == -FDT_ERR_EXISTS)
				continue;
			if (dst_node < 0)
				return dst_node;

			fdt_for_each_property_offset(prop, src, node) {
				const char *pname;
				const void *val;
				int vlen, err;

				val = fdt_getprop_by_offset(src, prop, &pname,
							    &vlen);
				if (!val || !pname)
					continue;
				/* phandles are node-local; a copy must not
				 * duplicate a value the OS DT already uses.
				 */
				if (!strcmp(pname, "phandle") ||
				    !strcmp(pname, "linux,phandle"))
					continue;
				err = fdt_setprop(dst, dst_node, pname, val,
						  vlen);
				if (err < 0)
					return err;
			}
		}
	}

	total = fdt_num_mem_rsv(src);
	for (i = 0; i < total; i++) {
		uint64_t addr, size;
		int j, jtotal, err;
		bool dup = false;

		if (fdt_get_mem_rsv(src, i, &addr, &size) != 0 || !size)
			continue;

		jtotal = fdt_num_mem_rsv(dst);
		for (j = 0; j < jtotal; j++) {
			uint64_t da, ds;

			if (fdt_get_mem_rsv(dst, j, &da, &ds) == 0 &&
			    da == addr && ds == size) {
				dup = true;
				break;
			}
		}
		if (dup || gemini_rsv_covered(dst, addr, size))
			continue;

		err = fdt_add_mem_rsv(dst, addr, size);
		if (err < 0)
			return err;
	}

	return 0;
}

#endif /* __MT6797_RESERVATIONS_H */
