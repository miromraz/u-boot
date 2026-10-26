#!/bin/sh
# SPDX-License-Identifier: GPL-2.0+
# Handset policy only: U-Boot in A, Android in B. Sourced by /init and tests.
slotflip_apply() {
	mode=$1
	current=$(qbootctl -c 2>/dev/null | sed -n 's/^Current slot: _\([ab]\)$/\1/p')
	case $current in a|b) ;; *) say 'Unknown current slot; refusing'; return 1 ;; esac
	if [ "$mode" = test ]; then
		say "Probe only: current=$current, no boot-control writes"
		return 0
	fi
	if [ "$mode" != b ] || [ "$current" != a ]; then
		say "Expected current=a and target=b; refusing current=$current target=$mode"
		return 1
	fi
	boot_b=${SLOTFLIP_BOOT_DIR:-/dev/disk/by-partlabel}/boot_b
	[ -b "$boot_b" ] || [ -f "$boot_b" ] || { say 'boot_b missing'; return 1; }
	magic=$(head -c 8 "$boot_b")
	size=$(od -An -tu4 -j8 -N4 "$boot_b" | tr -d ' ')
	case $size in ''|*[!0-9]*) say 'Invalid Android boot header'; return 1 ;; esac
	if [ "$magic" != 'ANDROID!' ] || [ "$size" -le 4194304 ]; then
		say 'boot_b is not a plausible Android kernel; refusing'
		return 1
	fi
	# Mark the known-good source before activating B; never continue on failure.
	qbootctl -m || { say 'Could not mark A successful; refusing'; return 1; }
	qbootctl -s b || { say 'Could not select B'; return 1; }
	say 'Selected Android B; reboot required'
}
