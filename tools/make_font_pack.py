# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Rev. J. Money
"""Pack TheDraw (.TDF) fonts into one file for the clock, so it can load any
font with a single seek (opening files inside a folder of thousands of fonts
takes seconds on the SD card).

usage: python -I tools/make_font_pack.py FONT_DIR OUT/clock.pak
       Only block and colour fonts that have the digits 0-9 are packed.
       Copy the result to the root of the SD card as /clock.pak.

format: "TDFPACK1", u32 count, then count x (u32 offset, u32 length, char name[16]),
        then the fonts back to back (each a complete single-font .tdf file)
"""
import glob
import os
import struct
import sys


def has_digits(d):
    if len(d) < 233 or d[41] not in (1, 2):
        return False
    return all((d[45 + (ord(c) - 33) * 2] | d[46 + (ord(c) - 33) * 2] << 8) != 0xFFFF for c in "0123456789")


def main(src, out):
    fonts = []
    for f in sorted(glob.glob(os.path.join(src, "*.tdf")), key=str.lower):
        d = open(f, "rb").read()
        if has_digits(d):
            fonts.append((os.path.splitext(os.path.basename(f))[0], d))
    base = 12 + 24 * len(fonts)
    table, data = b"", b""
    for name, d in fonts:
        table += struct.pack("<II", base + len(data), len(d)) + name.encode("ascii", "replace")[:15].ljust(16, b"\0")
        data += d
    open(out, "wb").write(b"TDFPACK1" + struct.pack("<I", len(fonts)) + table + data)
    print(f"{len(fonts)} fonts, {(base + len(data)) // 1024} KB -> {out}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
