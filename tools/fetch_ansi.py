# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Rev. J. Money
"""Download classic ANSI art packs from 16colo.rs and convert them into .cg
files: a small set in data/ansi/ for the LittleFS image (the fallback when there
is no SD card), or with --sd every piece into sd/ansi/ for the SD card (copy it
with tools/sd_ansi.py, or onto the card's /ansi folder in a card reader).

The art itself is NOT part of this repo (it belongs to the original artists and
groups), which is why it is fetched at build time instead of committed.

usage: python -I tools/fetch_ansi.py [--sd] [--budget-kb 800] [--max-rows 150]
"""
import io
import os
import sys
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import ans2grid  # noqa: E402

PACKS = [
    "1996/acid-50a",
    "1996/fire0896",
    "1997/acid-54",
    "1997/acid-55a",
    "2014/blocktronics_blockalypse",
    "2014/blocktronics_1980",
    "2014/blocktronics_wtf4",
    "2014/impure53",
]


def short_name(pack):
    """acid-50a -> acid50a, blocktronics_wtf4 -> bt_wtf4"""
    return pack.replace("blocktronics_", "bt_").replace("-", "")[:9]


def fetch(pack, cache):
    dst = os.path.join(cache, pack.split("/")[1] + ".zip")
    if not os.path.exists(dst):
        url = f"https://16colo.rs/archive/{pack}.zip"
        print("download", url)
        req = urllib.request.Request(url, headers={"User-Agent": "cyd-demoscene/1.0"})
        with urllib.request.urlopen(req, timeout=60) as r:
            open(dst, "wb").write(r.read())
    return dst


def main(argv):
    sd = "--sd" in argv
    budget = (1 << 30 if sd else 800) * 1024  # LittleFS is 960 KB
    max_rows = 150
    if "--budget-kb" in argv:
        budget = int(argv[argv.index("--budget-kb") + 1]) * 1024
    if "--max-rows" in argv:
        max_rows = int(argv[argv.index("--max-rows") + 1])
    cache = os.path.join(ROOT, "ansi_src")
    out_dir = os.path.join(ROOT, "sd" if sd else "data", "ansi")
    os.makedirs(cache, exist_ok=True)
    os.makedirs(out_dir, exist_ok=True)
    for f in os.listdir(out_dir):
        os.remove(os.path.join(out_dir, f))

    # gather candidates pack by pack, then round-robin so every pack is represented
    per_pack = []
    for pack in PACKS:
        name = pack.split("/")[1]
        extract = os.path.join(cache, name)
        os.makedirs(extract, exist_ok=True)
        with zipfile.ZipFile(fetch(pack, cache)) as z:
            files = []
            for info in z.infolist():
                base = os.path.basename(info.filename)
                if not base.lower().endswith(".ans") or info.file_size > 200_000:
                    continue
                path = os.path.join(extract, base)
                with open(path, "wb") as fh:
                    fh.write(z.read(info))
                files.append(path)
        per_pack.append((name, sorted(files)))

    used, count = 0, 0
    queues = [list(f) for _, f in per_pack]
    names = [n for n, _ in per_pack]
    while any(queues) and used < budget:
        for qi, q in enumerate(queues):
            if not q or used >= budget:
                continue
            src = q.pop(0)
            tmp = os.path.join(cache, "_tmp")
            os.makedirs(tmp, exist_ok=True)
            dst, msg = ans2grid.convert(src, tmp, max_rows)
            if not dst:
                continue
            size = os.path.getsize(dst)
            if used + size > budget:
                os.remove(dst)
                continue
            # LittleFS (mklittlefs) names max out at 31 chars, keep well under
            stem = os.path.basename(dst)[:-3].strip("_")[:18]
            final = os.path.join(out_dir, f"{short_name(names[qi])}_{stem}.cg")
            n = 2
            while os.path.exists(final):
                final = os.path.join(out_dir, f"{short_name(names[qi])}_{stem[:15]}{n}.cg")
                n += 1
            os.replace(dst, final)
            used += size
            count += 1
            print(msg)
    print(f"\n{count} pieces, {used // 1024} KB in {out_dir}")


if __name__ == "__main__":
    main(sys.argv[1:])
