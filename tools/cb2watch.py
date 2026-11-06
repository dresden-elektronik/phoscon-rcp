#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""Post-flash watch for the ConBee II: product name + decoded CDC traffic.

killprobe.py and waitflash.py both polled the board the same way: every ~0.4 s
open the CDC port, flush its input, send the ZM "ID" frame, close. That measures
the bootloader window well, and it silently threw away the one thing our own app
emits - the 8-byte debug records on CDC bulk IN. reset_input_buffer() is not a
detail here: on a real board it is the reason a flash session has no record trail.

This watcher keeps ONE handle open for the whole run and reuses it for the ZM
probe (the bootloader answers on the same IN pipe), so the timeline measurement
survives and the records survive too. Bytes go through tools/cdcview.py, which
decodes records with tools/dbg.py and shows the vendor text as text.

Usage:
  cb2watch.py [seconds=35] [--no-probe]     port: $CB2_PORT
  cb2watch.py --feed FILE [seconds]         replay a captured byte stream instead
                                            of opening a port (dry run / tests)
"""

import os
import re
import subprocess
import sys
import time

import cb2port
import serial

import cdcview



# ZM CMD_READ_PARAMETER-style "ID" request the flasher and the BTL speak (C0
# frame, payload, 16-bit little-endian checksum, C0) - kept identical to
# killprobe.py / zm.py so the answers stay comparable across tools.
ZM_ID_FRAME = bytes([0xC0]) + b"ID" + bytes([((~sum(b"ID") + 1) & 0xFFFF) & 0xFF,
                                             ((~sum(b"ID") + 1) >> 8) & 0xFF, 0xC0])


def product():
    out = subprocess.run(
        ["ioreg", "-p", "IOUSB", "-l", "-w0"], capture_output=True, text=True
    ).stdout
    for p in re.findall(r'"USB Product Name" = "([^"]*)"', out):
        if p in ("ConBee II", "phoscon-rcp"):
            return p
    return "-"


class Repeater:
    """Print each distinct line once and count the repeats: a ZM probe answered the
    same way 80 times is one finding, not 80 lines burying the records."""

    def __init__(self):
        self.last = None
        self.count = 0

    def emit(self, text):
        if text == self.last:
            self.count += 1
            return
        self.close()
        self.last = text
        self.count = 1

    def close(self):
        if self.last is not None:
            print(self.last + ("" if self.count == 1 else "   (x%d)" % self.count),
                  flush=True)
            self.last = None
            self.count = 0


def watch(seconds, probe=True, feed_file=None, show_product=True):
    view = cdcview.CdcView("cdc")
    rep = Repeater()
    t0 = time.time()
    last_state = None

    if feed_file:
        data = open(feed_file, "rb").read()
        step = 64
        for off in range(0, len(data), step):
            for line in view.feed(data[off:off + step]):
                rep.emit("[%7.3f] %s" % (time.time() - t0, line))
        for line in view.flush():
            rep.emit("[%7.3f] %s" % (time.time() - t0, line))
        rep.close()
        print("[%7.3f] replayed %d byte(s) from %s: %s"
              % (time.time() - t0, len(data), feed_file, view.stats()), flush=True)
        return view

    ser = None
    while time.time() - t0 < seconds:
        t = time.time() - t0
        if ser is None:
            try:
                ser = serial.Serial(cb2port.resolve(), 115200, timeout=0)
                rep.emit("[%7.3f] CDC open %s" % (t, cb2port.resolve()))
            except Exception:
                rep.emit("[%7.3f] CDC closed (%s unreachable)" % (t, cb2port.resolve()))
                ser = None
        if ser is not None:
            try:
                n = ser.in_waiting
                if n:
                    for line in view.feed(ser.read(n)):
                        rep.emit("[%7.3f] %s" % (t, line))
                if probe:
                    ser.write(ZM_ID_FRAME)
                    ser.flush()
                    time.sleep(0.25)
                    n = ser.in_waiting
                    if n:
                        for line in view.feed(ser.read(n)):
                            rep.emit("[%7.3f] %s" % (t, line))
            except Exception:
                rep.emit("[%7.3f] CDC lost (%s)" % (t, cb2port.resolve()))
                try:
                    ser.close()
                except Exception:
                    pass
                ser = None
        if show_product:
            state = product()
            if state != last_state:
                rep.emit("[%7.3f] product: %s" % (t, state))
                last_state = state
        time.sleep(0.1)

    for line in view.flush():
        rep.emit("[%7.3f] %s" % (time.time() - t0, line))
    rep.close()
    print("[%7.3f] done: %s; product %s"
          % (time.time() - t0, view.stats(), product()), flush=True)
    return view


def main():
    args = [a for a in sys.argv[1:]]
    seconds = 35.0
    probe = "--no-probe" not in args
    feed_file = None
    if "--feed" in args:
        i = args.index("--feed")
        feed_file = args[i + 1]
        del args[i:i + 2]
    args = [a for a in args if not a.startswith("--")]
    if args:
        seconds = float(args[0])
    watch(seconds, probe=probe, feed_file=feed_file)
    return 0


if __name__ == "__main__":
    sys.exit(main())
