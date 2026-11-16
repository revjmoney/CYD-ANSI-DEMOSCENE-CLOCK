# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Rev. J. Money
"""Build sd/ansi.pak: the big ANSI gallery for the SD card. Picks packs from
the classic groups on 16colo.rs until each group has about 300 pieces,
spread over its active years
(the biggest releases of each year first), converts up to 30 pieces of 80
column ANSI from each (by content: the old groups used their own extensions,
.ice .cia .drk .lgo ...) and joins the results into one file, so the board finds any piece
instantly (a FAT folder of thousands of files takes seconds per lookup).

Copy it to the card's root as /ansi.pak (card reader, or tools/sd_ansi.py).
The art is fetched at build time and never committed: it belongs to its artists.

Pack format, little endian:
  0  'ANSIPAK1'
  8  u32 count
  12 count x {u32 offset, u32 size, char name[24]}   offsets from file start
  .. the .cg files (see ans2grid.py)

usage: python -I tools/make_ansi_pack.py [--per-group 300] [--per-pack 30] [--max-rows 150]
"""
import collections
import os
import re
import struct
import sys
import tempfile
import time
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import ans2grid  # noqa: E402

GROUPS = ["acid", "ice", "fire", "cia", "mistigris", "remorse", "dark", "impure", "blocktronics"]
UA = {"User-Agent": "cyd-demoscene/1.0"}
SKIP = {".exe", ".com", ".jpg", ".gif", ".png", ".zip", ".s3m", ".mod", ".xm", ".it", ".rip", ".xb", ".bin", ".adf", ".class", ".lit", ".txt", ".nfo", ".diz"}  # .lit: literature, mostly plain text


def is_ansi(name, data):
    """colour ANSI, whatever the group called it"""
    return os.path.splitext(name)[1].lower() not in SKIP and data.count(b"[") >= 20


def get(url):
    for attempt in range(3):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=120) as r:
                return r.read()
        except OSError:
            if attempt == 2:
                raise
            time.sleep(5)


def releases(group, cache):
    """[(year, pack, entries)] from the group's timeline page"""
    path = os.path.join(cache, f"group_{group}.html")
    if not os.path.exists(path):
        open(path, "wb").write(get(f"https://16colo.rs/group/{group}"))
    html = open(path, encoding="utf-8", errors="replace").read()
    found = re.findall(r'<h3 class="h3">\s*(?:\w+ )?(\d{4}) - <a href="/pack/([^"/]+)">[^<]*</a></h3>\s*entries: (\d+)', html)
    return [(int(y), p, int(n)) for y, p, n in found]


def pick(rel, n):
    """n packs spread over the years: every year gets its share (at least one
    while there is room), biggest releases first"""
    by_year = collections.defaultdict(list)
    for r in rel:
        by_year[r[0]].append(r)
    for y in by_year:
        by_year[y].sort(key=lambda r: -r[2])
    chosen = []
    while len(chosen) < n and any(by_year.values()):
        for y in sorted(by_year):  # one per year per round, oldest first
            if by_year[y] and len(chosen) < n:
                chosen.append(by_year[y].pop(0))
    return chosen


def main(argv):
    per_group = int(argv[argv.index("--per-group") + 1]) if "--per-group" in argv else 300
    per_pack = int(argv[argv.index("--per-pack") + 1]) if "--per-pack" in argv else 30
    max_rows = int(argv[argv.index("--max-rows") + 1]) if "--max-rows" in argv else 150
    cache = os.path.join(ROOT, "ansi_src", "pak")
    os.makedirs(cache, exist_ok=True)
    pieces, seen = [], set()
    for group in GROUPS:
        before, used = len(pieces), []
        for year, pack, _ in pick(releases(group, cache), 40):  # at most 40 packs a group
            if len(pieces) - before >= per_group:
                break
            used.append(f"{pack} ({year})")
            zpath = os.path.join(cache, pack + ".zip")
            for y in (year, year - 1, year + 1):  # the archive folder is sometimes a year off
                if os.path.exists(zpath):
                    break
                try:
                    open(zpath, "wb").write(get(f"https://16colo.rs/archive/{y}/{pack}.zip"))
                except OSError:
                    pass
            if not os.path.exists(zpath):
                print(f"  {pack}: download failed")
                continue
            try:
                z = zipfile.ZipFile(zpath)
            except zipfile.BadZipFile:
                print(f"  {pack}: not a zip")
                continue
            took = 0
            with tempfile.TemporaryDirectory() as tmp:
                for info in sorted(z.infolist(), key=lambda i: i.filename.lower()):
                    base = os.path.basename(info.filename)
                    if took >= per_pack or not base or info.file_size > 400_000:
                        continue
                    raw = z.read(info)
                    if not is_ansi(base, raw):
                        continue
                    src = os.path.join(tmp, "in.ans")
                    open(src, "wb").write(raw)
                    try:
                        dst, _ = ans2grid.convert(src, tmp, max_rows)
                    except Exception:  # a few files in the archive are broken
                        continue
                    if not dst:
                        continue
                    data = open(dst, "rb").read()
                    if data[84:] in seen:  # the same piece re-released in another pack
                        continue
                    seen.add(data[84:])
                    took += 1
                    stem = re.sub(r"[^a-z0-9_.-]", "_", base.lower())
                    pieces.append((f"{pack[:11]}/{stem}"[:23], data))
        print(f"{group}: {len(pieces) - before} pieces from {', '.join(used)}")
    out_dir = os.path.join(ROOT, "sd")
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, "ansi.pak")
    with open(out, "wb") as f:
        f.write(b"ANSIPAK1" + struct.pack("<I", len(pieces)))
        off = 12 + 32 * len(pieces)
        for name, data in pieces:
            f.write(struct.pack("<II", off, len(data)) + name.encode()[:24].ljust(24, b"\0"))
            off += len(data)
        for _, data in pieces:
            f.write(data)
    print(f"\n{len(pieces)} pieces, {os.path.getsize(out) // 1024 // 1024} MB in {out}")


if __name__ == "__main__":
    main(sys.argv[1:])
