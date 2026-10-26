#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
"""Build handset slotflip UKIs on a build host, without booting ARM code."""

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import tempfile


def arm64_elf(path):
    data = path.read_bytes()
    if data[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', data, 18)[0] != 183:
        raise ValueError(f'not a little-endian ARM64 ELF: {path}')
    return data


def arm64_pe(path):
    with path.open('rb') as source:
        if source.read(2) != b'MZ':
            raise ValueError(f'not a PE executable: {path}')
        source.seek(0x3c)
        offset = struct.unpack('<I', source.read(4))[0]
        source.seek(offset)
        if source.read(4) != b'PE\0\0' or source.read(2) != b'\x64\xaa':
            raise ValueError(f'not an ARM64 PE executable: {path}')


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def archive(files):
    """Write newc directly, including /dev/console, without mknod/root."""
    result = bytearray()
    entries = {'.': (stat.S_IFDIR | 0o755, b'', 0, 0)}
    for name, mode, data, major, minor in files:
        parent = Path(name).parent
        while str(parent) != '.':
            entries.setdefault(str(parent), (stat.S_IFDIR | 0o755, b'', 0, 0))
            parent = parent.parent
        entries[name] = (mode, data, major, minor)
    entries['TRAILER!!!'] = (0, b'', 0, 0)
    names = sorted(n for n in entries if n != 'TRAILER!!!') + ['TRAILER!!!']
    for inode, name in enumerate(names, 1):
        mode, data, major, minor = entries[name]
        encoded = name.encode() + b'\0'
        fields = (inode, mode, 0, 0, 1, 0, len(data), 0, 0,
                  major, minor, len(encoded), 0)
        result.extend(b'070701' + ''.join(f'{field:08x}' for field in fields).encode())
        result.extend(encoded)
        result.extend(b'\0' * (-len(result) % 4))
        result.extend(data)
        result.extend(b'\0' * (-len(result) % 4))
    return gzip.compress(bytes(result), mtime=0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('kernel', 'dtb', 'stub', 'runtime-root', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--release', required=True)
    parser.add_argument('--ukify', default=shutil.which('ukify') or '/usr/lib/systemd/ukify')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = args.runtime_root.resolve()
    output = args.output.resolve()
    arm64_pe(args.stub)
    arm64_pe(args.kernel)
    dtb = args.dtb.read_bytes()
    if not dtb.startswith(b'\xd0\x0d\xfe\xed') or b'google,sunfish\0' not in dtb:
        raise ValueError('DTB must identify google,sunfish')

    files = [('init', stat.S_IFREG | 0o755, (here / 'init').read_bytes(), 0, 0),
             ('policy.sh', stat.S_IFREG | 0o644, (here / 'policy.sh').read_bytes(), 0, 0),
             ('bin/sh', stat.S_IFLNK | 0o777, b'busybox', 0, 0),
             ('dev/console', stat.S_IFCHR | 0o600, b'', 5, 1)]
    for directory in ('proc', 'sys'):
        files.append((directory, stat.S_IFDIR | 0o755, b'', 0, 0))
    binaries = []
    for name in ('busybox', 'qbootctl'):
        source = next((root / p for p in (f'usr/bin/{name}', f'bin/{name}')
                       if (root / p).is_file()), None)
        if source is None:
            raise ValueError(f'missing ARM64 {name} in runtime root')
        files.append((f'bin/{name}', stat.S_IFREG | 0o755, arm64_elf(source), 0, 0))
        binaries.append(source)

    libraries = {}
    for directory in ('usr/lib/aarch64-linux-gnu', 'lib/aarch64-linux-gnu'):
        for source in sorted((root / directory).glob('*.so*')):
            if not source.is_file() or source.read_bytes()[:4] != b'\x7fELF':
                continue
            libraries[source.name] = source
    for name, source in libraries.items():
        files.append((f'lib/aarch64-linux-gnu/{name}', stat.S_IFREG | 0o755,
                      arm64_elf(source), 0, 0))
    for source in binaries + list(libraries.values()):
        dynamic = subprocess.check_output(['readelf', '-d', str(source)], text=True)
        needed = re.findall(r'\(NEEDED\).*\[(.*?)\]', dynamic)
        missing = set(needed) - libraries.keys()
        if missing:
            raise ValueError(f'missing runtime libraries for {source.name}: {sorted(missing)}')
        headers = subprocess.check_output(['readelf', '-l', str(source)], text=True)
        match = re.search(r'Requesting program interpreter: (.*?)\]', headers)
        if match:
            interpreter = match.group(1).lstrip('/')
            loader = libraries.get(Path(interpreter).name)
            if loader is None:
                raise ValueError(f'missing interpreter {interpreter}')
            files.append((interpreter, stat.S_IFREG | 0o755, arm64_elf(loader), 0, 0))

    output.mkdir(parents=True, exist_ok=True)
    canonical = output / 'EFI/sunfish'
    canonical.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='slotflip-', dir=output) as temporary:
        work = Path(temporary)
        initrd = work / 'initrd.gz'
        initrd.write_bytes(archive(files))
        payloads = []
        for name, mode in (('slotflip.efi', 'b'), ('slotflip-test.efi', 'test')):
            target = work / name
            subprocess.run([args.ukify, 'build', '--stub=' + str(args.stub.resolve()),
                            '--linux=' + str(args.kernel.resolve()),
                            '--devicetree=' + str(args.dtb.resolve()),
                            '--initrd=' + str(initrd), '--uname=' + args.release,
                            '--cmdline=slotflip=' + mode + ' quiet panic=10',
                            '--output=' + str(target)], check=True)
            arm64_pe(target)
            payloads.append((name, target))
        for name, target in payloads:
            os.replace(target, canonical / name)
            # Compatibility for the already-flashed bootloader's old ESP-root path.
            shutil.copyfile(canonical / name, output / name)
    manifest = {
        'release': args.release,
        'kernel_sha256': digest(args.kernel),
        'dtb_sha256': digest(args.dtb),
        'stub_sha256': digest(args.stub),
        'runtime_sha256': {name: hashlib.sha256(data).hexdigest()
                           for name, mode, data, major, minor in files
                           if stat.S_ISREG(mode) and name.startswith(('bin/', 'lib/'))},
        'payloads': {name: digest(output / name) for name in (
            'EFI/sunfish/slotflip.efi', 'EFI/sunfish/slotflip-test.efi',
            'slotflip.efi', 'slotflip-test.efi')},
        'hardware_tested': False,
    }
    (output / 'slotflip-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Built canonical EFI/sunfish payloads and ESP-root compatibility copies.')


if __name__ == '__main__':
    main()
