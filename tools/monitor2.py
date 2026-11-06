#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""Combined monitor: USB product state (ioreg) + CDC traffic, with timestamps.

The CDC traffic of this board is two different things on one pipe: text when
the vendor bootloader owns it, and 8-byte binary debug records when our app
owns it.  Both are shown, decoded through tools/cdcview.py -> tools/dbg.py,
so a record reads as "DBG_COMP_CLOCK line 816 DBG_CLOCK_RSN_48MHZ_READY" plus
the source line that emitted it instead of as repr() of control bytes.

Usage: monitor2.py [seconds]        port: $CB2_PORT
"""
import cb2port
import os
import re
import subprocess
import sys
import time

import cdcview

try:
    import serial
except ImportError:
    serial = None

PORT_GLOB = "/dev/cu.usbmodem"
PORT = cb2port.resolve()


def product():
    out = subprocess.run(
        ["ioreg", "-p", "IOUSB", "-l", "-w0"], capture_output=True, text=True
    ).stdout
    # macOS names the key "USB Product Name" in `ioreg -p IOUSB` (the older
    # "USB Product String" appears in some IOUSBFamily versions); match either
    # so a transition is never missed. (monitor2 printed None throughout the
    # first real-hardware watch because of exactly this.)
    prods = re.findall(r'"USB Prod(?:uct|uct Name|uct String)"\s*=\s*"([^"]*)"', out)
    # the CB2 device product: prefer "ConBee II" / "phoscon-rcp"
    for p in prods:
        if p in ("ConBee II", "phoscon-rcp"):
            return p
    return prods[0] if prods else "None"


def main():
    dur = float(sys.argv[1]) if len(sys.argv) > 1 else 120.0
    t0 = time.time()
    last_prod = None
    ser = None
    view = cdcview.CdcView("CDC")

    print(f"[0.000] product: {product()}", flush=True)
    while time.time() - t0 < dur:
        time.sleep(0.1)
        t = time.time() - t0
        try:
            prod = product()
        except Exception:
            prod = "ERR"
        if prod != last_prod:
            print(f"[{t:7.3f}] product: {prod}", flush=True)
            last_prod = prod
        if serial is not None:
            if ser is None:
                try:
                    ser = serial.Serial(PORT, 115200, timeout=0)
                    print(f"[{t:7.3f}] CDC open {PORT}", flush=True)
                except Exception:
                    ser = None
            elif ser and ser.is_open:
                try:
                    n = ser.in_waiting
                    if n:
                        for line in view.feed(ser.read(n)):
                            print(f"[{t:7.3f}] {line}", flush=True)
                except Exception:
                    try:
                        ser.close()
                    except Exception:
                        pass
                    ser = None
    for line in view.flush():
        print(f"[{dur:7.3f}] {line}", flush=True)
    print(f"[{dur:7.3f}] done: {view.stats()}", flush=True)


if __name__ == "__main__":
    main()
