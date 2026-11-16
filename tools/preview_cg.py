# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Rev. J. Money
"""Render a .cg grid to PNG with the VGA 8x16 font, to check conversions on the PC.

usage: python -I tools/preview_cg.py include/rotatocam_data.h file.cg out.png [--cyd]
       --cyd renders at the CYD's 4x8 cell size (2x2 averaged), like the firmware does
"""
import re
import sys

from PIL import Image

PAL = [0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
       0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF]


def load_font(header):
    txt = open(header).read()
    m = re.search(r"vga_font8x16\[4096\] = \{(.*?)\};", txt, re.S)
    return [int(v) for v in m.group(1).replace("\n", "").split(",") if v.strip()]


def main(header, cg, out, cyd=False):
    font = load_font(header)
    d = open(cg, "rb").read()
    assert d[:3] == b"CG1"
    w, h = d[3], d[4] | d[5] << 8
    print("title:", d[8:44].rstrip(b"\0").decode("cp437"), "| author:", d[44:64].rstrip(b"\0").decode("cp437"),
          "| group:", d[64:84].rstrip(b"\0").decode("cp437"), f"| {w}x{h}")
    img = Image.new("RGB", (w * 8, h * 16))
    px = img.load()
    for y in range(h):
        for x in range(w):
            o = 84 + (y * w + x) * 2
            ch, at = d[o], d[o + 1]
            fg, bg = PAL[at & 15], PAL[at >> 4]
            for r in range(16):
                bits = font[ch * 16 + r]
                for c in range(8):
                    col = fg if bits & (0x80 >> c) else bg
                    px[x * 8 + c, y * 16 + r] = (col >> 16, (col >> 8) & 255, col & 255)
    if cyd:
        img = img.resize((w * 4, h * 8), Image.BOX)
    img.save(out)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3], "--cyd" in sys.argv)
