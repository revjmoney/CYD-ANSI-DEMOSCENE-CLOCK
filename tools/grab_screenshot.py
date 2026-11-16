# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Rev. J. Money
"""Talk to the CYD over serial: send commands and grab panel screenshots.

usage: python -I tools/grab_screenshot.py COM5 out.png [cmds]
  cmds are sent first (e.g. "g5\\n" to jump to demo 5), then 'S' grabs the
  frame the panel is actually showing (read back from display RAM).
"""
import struct
import sys
import time

import serial
from PIL import Image


def main(port, out, cmds=""):
    s = serial.Serial(port, 921600, timeout=5)
    s.dtr = False
    s.rts = False  # don't reset the board on open
    time.sleep(0.2)
    if cmds:
        s.write(cmds.encode().decode("unicode_escape").encode())
        time.sleep(2.5)
    s.reset_input_buffer()
    s.write(b"S")
    buf = b""
    while b"SHOT" not in buf:
        chunk = s.read(1)
        if not chunk:
            raise SystemExit("no SHOT header")
        buf = (buf + chunk)[-4:]
    w, h = struct.unpack("<HH", s.read(4))
    raw = s.read(w * h * 3)
    if len(raw) != w * h * 3:
        raise SystemExit(f"short read {len(raw)}")
    Image.frombytes("RGB", (w, h), raw).save(out)
    s.write(b"i")
    time.sleep(0.2)
    print(s.read(s.in_waiting).decode(errors="replace").strip())


if __name__ == "__main__":
    main(*sys.argv[1:])
