#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
"""Parse a Gemini PDA U-Boot console dump out of an mtkclient 'cache' image.

gemini_logdump writes a 64-byte header + console bytes into the LAST 1 MiB of
the 'cache' partition. Give this the mtkclient dump of that partition (or the
whole eMMC); it finds the magic, validates the length and prints the log.

Usage: parse_logdump.py <cache-dump-file>
"""
import struct
import sys

MAGIC = b"GEMINI-UBOOT-LOG"
# magic[16] + u32 length + u32 seq + u64 timer + 32 reserved = 64 bytes
HDR = struct.Struct("<16sIIQ32s")


def parse(data: bytes) -> int:
    off = data.rfind(MAGIC)
    if off < 0:
        sys.stderr.write("error: magic %r not found\n" % MAGIC)
        return 1
    if off + HDR.size > len(data):
        sys.stderr.write("error: truncated header\n")
        return 1

    magic, length, seq, timer, _ = HDR.unpack(data[off:off + HDR.size])
    body = data[off + HDR.size:]
    if length > len(body):
        sys.stderr.write("error: length %d exceeds available %d bytes\n"
                         % (length, len(body)))
        return 1

    sys.stderr.write("offset=0x%x seq=%d timer=%dms length=%d bytes\n"
                     % (off, seq, timer, length))
    sys.stdout.write(body[:length].decode("utf-8", "replace"))
    return 0


def main() -> int:
    if len(sys.argv) != 2:
        sys.stderr.write(__doc__)
        return 2
    with open(sys.argv[1], "rb") as f:
        return parse(f.read())


if __name__ == "__main__":
    sys.exit(main())
