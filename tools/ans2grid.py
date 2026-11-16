# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Rev. J. Money
"""Convert classic .ANS (CP437 + ANSI.SYS escapes) files into the compact cell
grid the firmware plays back.

Output format (.cg), little endian:
  0  'CG1'  magic (3 bytes)
  3  u8     width (always 80)
  4  u16    height in rows
  6  u8     flags (bit0 = iCE colors)
  7  u8     reserved
  8  char[36] title   (from SAUCE, NUL padded)
  44 char[20] author
  64 char[20] group
  84 cells: height*80 * 2 bytes (CP437 char, attr = fg | bg << 4)

usage: python -I tools/ans2grid.py OUT_DIR file1.ans [file2.ans ...]
       options: --max-rows N (default 200, longer pieces are cut)
"""
import os
import re
import sys

W = 80


def parse_sauce(data):
    """Return (body, info dict). SAUCE is the last 128 bytes, magic 'SAUCE00'."""
    info = {"title": "", "author": "", "group": "", "ice": False, "width": W}
    if len(data) >= 128 and data[-128:-121] == b"SAUCE00":
        s = data[-128:]
        info["title"] = s[7:42].decode("cp437").strip(" \0")
        info["author"] = s[42:62].decode("cp437").strip(" \0")
        info["group"] = s[62:82].decode("cp437").strip(" \0")
        datatype, filetype = s[94], s[95]
        tinfo1 = s[96] | s[97] << 8
        comments = s[104]
        flags = s[105]
        if datatype == 1 and filetype in (0, 1, 2) and tinfo1:
            info["width"] = tinfo1
        info["ice"] = bool(flags & 1)
        cut = len(data) - 128
        if comments:
            cut -= 5 + 64 * comments
        data = data[:max(cut, 0)]
    eof = data.find(b"\x1a")
    if eof >= 0:
        data = data[:eof]
    return data, info


def render(data, width, ice):
    rows = []
    x = y = 0
    fg, bg, bold, blink = 7, 0, False, False
    saved = (0, 0)

    def ensure(row):
        while len(rows) <= row:
            rows.append([(32, 7)] * width)

    def attr():
        f = fg | (8 if bold else 0)
        b = bg | (8 if (blink and ice) else 0)
        return f | (b << 4)

    i, n = 0, len(data)
    csi = re.compile(rb"\x1b\[([0-9;?=]*)([A-Za-z])")
    while i < n:
        ch = data[i]
        if ch == 0x1B:
            m = csi.match(data, i)
            if not m:
                i += 1
                continue
            i = m.end()
            raw = m.group(1).decode("ascii", "replace").lstrip("?=")
            args = [int(a) if a.isdigit() else 0 for a in raw.split(";")] if raw else []
            cmd = m.group(2).decode()
            a0 = args[0] if args else 0
            if cmd == "m":
                for a in args or [0]:
                    if a == 0:
                        fg, bg, bold, blink = 7, 0, False, False
                    elif a == 1:
                        bold = True
                    elif a == 5:
                        blink = True
                    elif a in (22,):
                        bold = False
                    elif a in (25,):
                        blink = False
                    elif 30 <= a <= 37:
                        fg = [0, 4, 2, 6, 1, 5, 3, 7][a - 30]
                    elif 40 <= a <= 47:
                        bg = [0, 4, 2, 6, 1, 5, 3, 7][a - 40]
                    elif 90 <= a <= 97:
                        fg = [0, 4, 2, 6, 1, 5, 3, 7][a - 90]
                        bold = True
            elif cmd in ("H", "f"):
                y = (args[0] - 1 if len(args) > 0 and args[0] else 0)
                x = (args[1] - 1 if len(args) > 1 and args[1] else 0)
            elif cmd == "A":
                y = max(0, y - (a0 or 1))
            elif cmd == "B":
                y += a0 or 1
            elif cmd == "C":
                x = min(width - 1, x + (a0 or 1))
            elif cmd == "D":
                x = max(0, x - (a0 or 1))
            elif cmd == "J":
                if a0 == 2:
                    rows.clear()
                    x = y = 0
            elif cmd == "K":
                ensure(y)
                for xx in range(x, width):
                    rows[y][xx] = (32, attr())
            elif cmd == "s":
                saved = (x, y)
            elif cmd == "u":
                x, y = saved
            continue
        i += 1
        if ch == 0x0D:
            x = 0
        elif ch == 0x0A:
            y += 1
            x = 0  # most .ANS files assume CRLF, bare LF should still return
        elif ch == 0x09:
            x = min(width - 1, (x // 8 + 1) * 8)
        else:
            if x >= width:
                x = 0
                y += 1
            ensure(y)
            rows[y] = rows[y][:]  # rows start as shared lists
            rows[y][x] = (ch, attr())
            x += 1
    # trim trailing blank rows
    while rows and all(c == 32 and (a >> 4) == 0 for c, a in rows[-1]):
        rows.pop()
    return rows


def convert(path, out_dir, max_rows):
    data, info = parse_sauce(open(path, "rb").read())
    if info["width"] != W:
        return None, f"skip {os.path.basename(path)}: width {info['width']}"
    rows = render(data, W, info["ice"])[:max_rows]
    if len(rows) < 5:
        return None, f"skip {os.path.basename(path)}: only {len(rows)} rows"
    name = re.sub(r"[^a-z0-9_-]", "_", os.path.splitext(os.path.basename(path))[0].lower())[:24]
    out = bytearray(b"CG1")
    out += bytes([W]) + len(rows).to_bytes(2, "little") + bytes([1 if info["ice"] else 0, 0])
    for key, size in (("title", 36), ("author", 20), ("group", 20)):
        out += info[key].encode("cp437", "replace")[:size].ljust(size, b"\0")
    for r in rows:
        for c, a in r:
            out += bytes([c, a])
    dst = os.path.join(out_dir, name + ".cg")
    open(dst, "wb").write(out)
    return dst, f"{os.path.basename(path)} -> {name}.cg ({len(rows)} rows, {len(out)} B)"


def main(argv):
    max_rows = 200
    if "--max-rows" in argv:
        k = argv.index("--max-rows")
        max_rows = int(argv[k + 1])
        del argv[k:k + 2]
    out_dir, files = argv[0], argv[1:]
    os.makedirs(out_dir, exist_ok=True)
    for f in files:
        try:
            _, msg = convert(f, out_dir, max_rows)
        except Exception as e:  # one bad file should not stop a whole pack
            msg = f"error {f}: {e}"
        print(msg)


if __name__ == "__main__":
    main(sys.argv[1:])
