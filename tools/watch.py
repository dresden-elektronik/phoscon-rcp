#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""Passive post-flash watcher: which USB product the CB2 enumerates as, with
timestamps, plus its CDC traffic decoded - records from our app (8-byte binary,
see tools/dbg.py) and text from the vendor bootloader.

Usage: watch.py [seconds=90] [interval=0.25]      port: $CB2_PORT
  ConBee II     = bootloader (window / eternal window) or deCONZ
  phoscon-rcp = our app
  <none>        = no USB device (app alive at 8 MHz, USB not up)
"""
import cb2port
import os
import sys
import time

import serial
import serial.tools.list_ports

import cdcview

PORT = cb2port.resolve()
SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 90.0
STEP = float(sys.argv[2]) if len(sys.argv) > 2 else 0.25


def prod():
    for p in serial.tools.list_ports.comports():
        if p.device == PORT:
            return p.product
    return None


def main():
    t0 = time.time()
    last = None
    s = None
    view = cdcview.CdcView("cdc")
    while True:
        now = time.time() - t0
        if now >= SECS:
            break
        p = prod()
        if p != last:
            print(f"[{now:8.3f}] product: {p}", flush=True)
            # re-open the CDC port on a transition so we catch fresh data
            if s is not None:
                try:
                    s.close()
                except Exception:
                    pass
                s = None
            if p is not None:
                try:
                    s = serial.Serial(PORT, 115200, timeout=0.02)
                    s.reset_input_buffer()
                except Exception:
                    s = None
            last = p
        if s is not None:
            try:
                d = s.read(256)
                if d:
                    for line in view.feed(d):
                        print(f"[{now:8.3f}]   {line}", flush=True)
            except Exception:
                pass
        time.sleep(STEP)
    if s is not None:
        try:
            s.close()
        except Exception:
            pass
    for line in view.flush():
        print(f"[{SECS:8.3f}]   {line}", flush=True)
    print(f"[{SECS:8.3f}] done; final product: {prod()}; {view.stats()}")


if __name__ == "__main__":
    main()
