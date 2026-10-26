#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
"""Exercise slot policy with mocked boot control; never touch a real device."""

import gzip
import os
from pathlib import Path
import stat
import struct
import subprocess
import tempfile
import unittest

import build


HERE = Path(__file__).resolve().parent


class SlotPolicy(unittest.TestCase):
    def run_policy(self, target='b', current='a', mark=0, select=0,
                   magic=b'ANDROID!', size=8 * 1024 * 1024):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'boot_b').write_bytes(magic + struct.pack('<I', size))
            helper = root / 'qbootctl'
            helper.write_text(
                '#!/bin/sh\nprintf "%s\\n" "$*" >>"$QBOOT_LOG"\n'
                'case "$1" in\n'
                ' -c) printf "Current slot: _%s\\n" "$CURRENT";;\n'
                ' -m) exit "$MARK_RC";;\n'
                ' -s) exit "$SELECT_RC";;\n'
                ' *) exit 2;;\nesac\n')
            helper.chmod(0o755)
            env = dict(os.environ, PATH=str(root) + ':' + os.environ['PATH'],
                       SLOTFLIP_BOOT_DIR=str(root), QBOOT_LOG=str(root / 'calls'),
                       CURRENT=current, MARK_RC=str(mark), SELECT_RC=str(select))
            result = subprocess.run(
                ['sh', '-c', 'say() { :; }; . "$1"; slotflip_apply "$2"',
                 'slotflip-test', str(HERE / 'policy.sh'), target], env=env,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            return result.returncode, (root / 'calls').read_text().splitlines()

    def test_marks_source_before_selecting_android(self):
        self.assertEqual(self.run_policy(), (0, ['-c', '-m', '-s b']))

    def test_mark_failure_prevents_slot_selection(self):
        status, calls = self.run_policy(mark=1)
        self.assertNotEqual(status, 0)
        self.assertEqual(calls, ['-c', '-m'])

    def test_selection_failure_is_reported(self):
        self.assertNotEqual(self.run_policy(select=1)[0], 0)

    def test_probe_never_writes_boot_control(self):
        for current in ('a', 'b'):
            self.assertEqual(self.run_policy(target='test', current=current), (0, ['-c']))

    def test_invalid_inputs_never_write_boot_control(self):
        for options in ({'current': '?'}, {'current': 'b'}, {'target': ''},
                        {'target': 'a'}, {'magic': b'INVALID!'}, {'size': 600000}):
            status, calls = self.run_policy(**options)
            self.assertNotEqual(status, 0, options)
            self.assertEqual(calls, ['-c'], options)

    def test_failed_efi_load_does_not_execute_stale_buffer(self):
        env = (HERE.parent.parent / 'board/qualcomm/sunfish-handset.env').read_text()
        script = next(line.split('=', 1)[1] for line in env.splitlines()
                      if line.startswith('boot_android='))
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / 'executed'
            result = subprocess.run(
                ['sh', '-c', 'load() { return 1; }; pause() { :; }; '
                 'bootefi() { touch "$EXECUTED"; }; ' + script],
                env=dict(os.environ, EXECUTED=str(marker)), capture_output=True)
            self.assertEqual(result.returncode, 0)
            self.assertFalse(marker.exists())


class Initramfs(unittest.TestCase):
    def test_standard_cpio_reads_console_and_shell_link(self):
        data = build.archive([
            ('bin/busybox', stat.S_IFREG | 0o755, b'fixture', 0, 0),
            ('bin/sh', stat.S_IFLNK | 0o777, b'busybox', 0, 0),
            ('dev/console', stat.S_IFCHR | 0o600, b'', 5, 1)])
        result = subprocess.run(['cpio', '-itv', '--quiet'], input=gzip.decompress(data),
                                capture_output=True, check=True)
        listing = result.stdout.decode()
        self.assertIn('bin/sh -> busybox', listing)
        self.assertTrue(any(line.startswith('c') and line.endswith('dev/console')
                            for line in listing.splitlines()))

    def test_wrong_architecture_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            elf = Path(directory) / 'x86'
            elf.write_bytes(b'\x7fELF\x02\x01' + bytes(12) + struct.pack('<H', 62))
            with self.assertRaises(ValueError):
                build.arm64_elf(elf)


if __name__ == '__main__':
    unittest.main()
