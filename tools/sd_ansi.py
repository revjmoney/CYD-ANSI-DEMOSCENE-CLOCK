# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Rev. J. Money
"""Copy the ANSI art onto the board's SD card over USB: sd/ansi.pak (made by
make_ansi_pack.py) to /ansi.pak, else sd/ansi/*.cg (fetch_ansi.py --sd) into
/ansi. The board must be running the demos. About 30 KB/s, so the big pack
takes a while; copying it in a PC card reader is much faster.

usage: python tools/sd_ansi.py COM5
"""
import os
import sys
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
SD = os.path.join(os.path.dirname(HERE), "sd")
SRC = os.path.join(SD, "ansi")


def expect(s, want, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        line = s.readline().decode(errors="replace").strip()
        if line.startswith("@"):
            if not line.startswith(want):
                raise RuntimeError(line)
            return line
    raise RuntimeError(f"no {want} from the board")


def main(port):
    pak = os.path.join(SD, "ansi.pak")
    if os.path.exists(pak):
        files = [("/ansi.pak", pak)]
    else:
        files = [(f"/ansi/{f}", os.path.join(SRC, f)) for f in sorted(os.listdir(SRC)) if f.endswith(".cg")]
    if not files:
        sys.exit("nothing to copy: run python -I tools/make_ansi_pack.py (or fetch_ansi.py --sd) first")
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 921600, 3
    s.dtr = s.rts = False  # opening the port must not reset the board
    s.open()
    time.sleep(0.5)
    s.reset_input_buffer()
    total = 0
    for i, (name, src) in enumerate(files):
        data = open(src, "rb").read()
        s.write(f"@PUT {name} {len(data)}\n".encode())
        expect(s, "@READY")
        t0 = time.time()
        for off in range(0, len(data), 512):
            s.write(data[off:off + 512])
            expect(s, "@K")
            if off and off % (1 << 20) == 0:
                left = (time.time() - t0) / off * (len(data) - off)
                print(f"  {off >> 20}/{len(data) >> 20} MB, about {left / 60:.0f} min left", flush=True)
        done = expect(s, "@DONE")
        if int(done.split()[1]) != sum(data) & 0xFFFFFFFF:
            sys.exit(f"{name}: checksum mismatch")
        total += len(data)
        print(f"[{i + 1}/{len(files)}] {name}")
    print(f"{len(files)} file(s), {total // 1024} KB copied. Restart the board to use them.")


if __name__ == "__main__":
    main(sys.argv[1])
