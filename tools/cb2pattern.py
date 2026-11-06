#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""Measure the bootloader-window cycle of a running CB2 image.

An image that resets itself - deliberately (SCB.AIRCR SYSRESETREQ) or through
the watchdog - makes the GCF bootloader window reappear again and again without
anyone touching the stick. The pattern is then an output channel: the window is
the readable part, the gap is the part the application controls, so a bring-up
that stamps a different hold per stage reports the stage it reached as a gap
duration. No SWD, no flash read-back, no working USB required.

Identity is matched on vendor/product id, not on the product string: a device
that attaches but never finishes enumerating has no product string, and that is
exactly the case we must not mistake for "absent".

Usage: cb2pattern.py [-t SECONDS] [-s SERIAL] [--vid VID] [--pid PID]
       (default 60 s, 0x1CF1:0x0030 = the GCF bootloader window)
"""
import argparse
import re
import subprocess
import sys
import time

VENDOR_DRESDEN = 0x1CF1
PID_BOOTLOADER = 0x0030


def snapshot(vid: int, pid: int, serial: str | None):
    """Return [(serial, product)] for attached USB device nodes matching vid:pid.

    Line-based, because `ioreg -p IOUSB -l` prints one node line carrying
    "<class IOUSBDevice, id ...>" and its quoted properties on the deeper lines
    below it until the next node line. (Splitting on a `) class` shape - or on a
    lookahead, which re.split then treats as a captured separator - produced
    blocks with no properties in them and the tool reported an absent device even
    while it was enumerated.)
    """
    out = subprocess.run(
        ["ioreg", "-p", "IOUSB", "-l", "-w0"],
        capture_output=True, text=True,
    ).stdout
    blocks, cur = [], None
    for line in out.splitlines():
        # macOS names the node class IOUSBHostDevice in current IOUSBFamily and
        # IOUSBDevice in older ones; accept both or the tool sees nothing at all.
        if "<class IOUSBHostDevice" in line or "<class IOUSBDevice" in line:
            cur = [line]
            blocks.append(cur)
        elif cur is not None:
            cur.append(line)
    hits = []
    for blk in blocks:
        text = "\n".join(blk)
        m_vid = re.search(r'"idVendor" = (\d+)', text)
        if not m_vid or int(m_vid.group(1)) != vid:
            continue
        m_pid = re.search(r'"idProduct" = (\d+)', text)
        if m_pid and int(m_pid.group(1)) != pid:
            continue
        m_ser = re.search(r'"USB Serial Number" = "([^"]*)"', text)
        m_pro = re.search(r'"USB Product Name" = "([^"]*)"', text)
        m_bcd = re.search(r'"bcdDevice" = (\d+)', text)
        ser = m_ser.group(1) if m_ser else ""
        if serial and ser != serial:
            continue
        prod = m_pro.group(1) if m_pro else "<no product string>"
        # Since then our app answers with the bootloader's own name, so the
        # product string no longer says *which* firmware is on the bus: bcdDevice
        # does (0x0100 vendor bootloader and vendor app, 0x0101 ours).
        name = "%s#%04x" % (prod, int(m_bcd.group(1))) if m_bcd else prod
        hits.append((ser, name))
    return hits


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("-t", "--seconds", type=float, default=60.0)
    ap.add_argument("-s", "--serial", default=None,
                    help="require this USB serial number")
    ap.add_argument("--vid", type=lambda x: int(x, 16), default=VENDOR_DRESDEN)
    ap.add_argument("--pid", type=lambda x: int(x, 16), default=PID_BOOTLOADER)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    t0 = time.time()
    prev = False
    edges = []          # (t, True if appeared)
    print(f"watching {args.vid:04x}:{args.pid:04x}"
          + (f" serial {args.serial}" if args.serial else "")
          + f" for {args.seconds:.0f} s")
    while True:
        t = time.time() - t0
        if t >= args.seconds:
            break
        hits = snapshot(args.vid, args.pid, args.serial)
        up = bool(hits)
        if up != prev:
            edges.append((t, up))
            state = "WINDOW OPEN " if up else "window closed"
            names = ", ".join(f"{s} {n!r}" for s, n in hits) or "-"
            print(f"[{t:7.2f}] {state}  {names}", flush=True)
            prev = up
        time.sleep(0.2)

    if not edges:
        print("no transitions: the device never appeared and never left")
        return 1
    on_s, off_s = [], []
    for i, (t, up) in enumerate(edges):
        if i + 1 < len(edges):
            d = edges[i + 1][0] - t
            (on_s if up else off_s).append(d)
    print(f"--- {len(edges)} edges, {len(off_s)} gaps")
    for label, vals in (("window open ", on_s), ("gap (app)  ", off_s)):
        if vals:
            print(f"  {label}: " + " ".join(f"{v:5.2f}" for v in vals[:12])
                  + f"   mean {sum(vals)/len(vals):.2f} s")
    if off_s:
        duty = 0.0
        print("  stage read-out: the gap is what the application controls")
    return 0


if __name__ == "__main__":
    sys.exit(main())
