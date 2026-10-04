#!/bin/sh
# SPDX-License-Identifier: GPL-2.0+
#
# Package U-Boot into an Android boot image v0 that the Planet Gemini PDA's
# vendor LK bootloader accepts.
#
# LK gunzips the "kernel" to 0x40080000 and then scans for an appended DTB
# (it ignores the header dt_size), so the kernel payload is:
#
#     gzip(u-boot.bin)  ++  u-boot's Gemini DTB
#
# LK selects the 64-bit boot path from the "bootopt=64S3,32N2,64N2" cmdline
# and patches the appended DTB (/memory, CPU clock-frequency, mblock
# reserved-memory) before jumping to U-Boot with x0 = that DTB.
#
# Usage: mkbootimg.sh [BUILD_DIR]   (default BUILD_DIR: ./build-gemini)

set -e

BUILD_DIR="${1:-build-gemini}"
UBOOT_BIN="$BUILD_DIR/u-boot.bin"
DTB="$BUILD_DIR/arch/arm/dts/mt6797-gemini-pda.dtb"
OUT="$BUILD_DIR/u-boot-gemini-boot.img"

MKBOOTIMG="${MKBOOTIMG:-mkbootimg}"

# Android boot image offsets, from the Planet Gemini pmOS deviceinfo.
BASE=0x40080000
KERNEL_OFFSET=0x00000000
RAMDISK_OFFSET=0x04f80000
SECOND_OFFSET=0x00e80000
TAGS_OFFSET=0x03f80000
PAGESIZE=2048
CMDLINE="bootopt=64S3,32N2,64N2"

for f in "$UBOOT_BIN" "$DTB"; do
	if [ ! -f "$f" ]; then
		echo "error: missing $f (build U-Boot first)" >&2
		exit 1
	fi
done

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# kernel payload = gzip(u-boot.bin) ++ appended DTB
gzip -c "$UBOOT_BIN" > "$WORK/u-boot.bin.gz"

# LK's early DTB loader (bldr_load_dtb, app/mt_boot/mt_boot.c) reads the
# last DTB_MAX_SIZE (512 KiB) of the kernel payload at offset
# page_sz + kernel_sz - 512 KiB, unsigned. A payload smaller than that wraps
# around, the read fails and LK boots without a DTB. Zero-pad the gzip stream
# (gunzip ignores trailing bytes) so the payload is always >= 512 KiB.
truncate -s ">524288" "$WORK/u-boot.bin.gz"
cat "$WORK/u-boot.bin.gz" "$DTB" > "$WORK/kernel.gz-dtb"

# LK still expects a ramdisk slot in a v0 image; give it a tiny dummy.
: | gzip -c > "$WORK/ramdisk.gz"

"$MKBOOTIMG" \
	--kernel "$WORK/kernel.gz-dtb" \
	--ramdisk "$WORK/ramdisk.gz" \
	--base "$BASE" \
	--kernel_offset "$KERNEL_OFFSET" \
	--ramdisk_offset "$RAMDISK_OFFSET" \
	--second_offset "$SECOND_OFFSET" \
	--tags_offset "$TAGS_OFFSET" \
	--pagesize "$PAGESIZE" \
	--cmdline "$CMDLINE" \
	--header_version 0 \
	-o "$OUT"

echo "wrote $OUT"
