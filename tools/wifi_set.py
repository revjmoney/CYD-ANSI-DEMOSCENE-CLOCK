# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Rev. J. Money
"""Give the CYD clock a WiFi network over USB. Run it yourself: the password
is typed here, hidden, and sent straight to the board.

usage: python tools/wifi_set.py COM5             add a network (prompts)
       python tools/wifi_set.py COM5 --forget    forget all saved networks
       python tools/wifi_set.py COM5 --zip 12345 show the weather for a US ZIP code
       python tools/wifi_set.py COM5 --zip ""    weather off
"""
import getpass
import subprocess
import sys
import time

import serial


def current_ssid():
    """The network this PC is on, offered as the default (Windows only)."""
    try:
        out = subprocess.run(["netsh", "wlan", "show", "interfaces"], capture_output=True, text=True).stdout
        for line in out.splitlines():
            k, _, v = line.partition(":")
            if k.strip() == "SSID":
                return v.strip()
    except OSError:
        pass
    return ""


def main(port, forget=False, zipcode=None):
    s = serial.Serial(port, 921600, timeout=3)
    s.dtr = False
    s.rts = False
    time.sleep(6)  # opening the port can reset the board; let it boot
    s.reset_input_buffer()
    if zipcode is not None:
        if not (zipcode == "" or (zipcode.isdigit() and len(zipcode) == 5)):
            sys.exit("a ZIP code is five digits")
        s.write(f"@ZIP {zipcode}\n".encode())
    elif forget:
        s.write(b"@WIFIFORGET\n")
    else:
        guess = current_ssid()
        ssid = input(f"WiFi name [{guess}]: ").strip() or guess
        pw = getpass.getpass("Password (blank for an open network): ")
        if "\t" in ssid + pw or "\n" in ssid + pw:
            sys.exit("tabs and newlines can't be sent")
        s.write(f"@WIFI {ssid}\t{pw}\n".encode())
    for _ in range(20):
        line = s.readline().decode(errors="replace").strip()
        if line.startswith("@OK"):
            print(line[4:])
            print("Unplug and replug the board (or tap SYNC TIME NOW in the clock settings).")
            return
    sys.exit("no answer from the board: is it running the demos (not the game menu)?")


if __name__ == "__main__":
    z = sys.argv[sys.argv.index("--zip") + 1] if "--zip" in sys.argv[:-1] else None
    main(sys.argv[1], "--forget" in sys.argv, z)
