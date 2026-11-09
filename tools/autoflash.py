#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""Flash a CB2 by catching a bootloader window - no power cycle needed if the
image on the board is self-returning.

Two facts make this possible, both measured on the board
(docs/build-and-flash.md §5):

 1. A user reset - SCB.AIRCR SYSRESETREQ from the app, or a watchdog time-out -
    re-runs the GCF bootloader, and its window enumerates as 0x1CF1:0x0030
    "ConBee II". A canary image that reset itself produced 14 windows in a row
    (3.70 s present, 2.65-3.18 s absent) without anyone touching the stick.
 2. GCFFlasher only needs to be *connecting* during such a window. Blind retry
    loops ("failed to connect serial com" every second) hit a window by luck;
    watching the bus and firing the moment one opens hits it every time.

So the recipe is: get a self-returning image on the board once (that first one
does need one power cycle), and from then on windows come by themselves and this
tool flashes into them unattended.

The pre-flash guard (gcf_check.py) is mandatory here exactly as in every other
flash path of this project: magic, type 1..10 (never plaintext), target 0x5000
(a 0x0-offset GCF would clobber the vendor bootloader), the image must end
before the NVRAM at 0x3EF00, and the .GCF must be the .bin's (its size and
CRC, and not older). A build failure must never end up flashing yesterday's image.

The stick is named by its USB serial: several ConBee IIs share the port prefix,
and a tool that took the first one would flash whichever enumerated first.

Usage: autoflash.py PATH.GCF [PATH.bin] -s SERIAL [-t WAIT_S] [-n ATTEMPTS]
                        [--gcftmo SEC] [--dry-run]
"""
import cb2flasher
import gcftool
import argparse
import glob
import os
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GCFFLASHER = cb2flasher.resolve()


def windows(serial: str):
    """CDC ports currently present whose name carries this USB serial number."""
    return [p for p in sorted(glob.glob("/dev/cu.usbmodem*")) if serial in p]


def wait_for_window(wait_s: float, serial: str):
    t0 = time.time()
    while time.time() - t0 < wait_s:
        ports = windows(serial)
        if ports:
            return ports[0], time.time() - t0
        time.sleep(0.2)
    return None, wait_s


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("gcf")
    ap.add_argument("bin", nargs="?", default=None,
                    help=".bin the .GCF must match (default: the GCF's path minus .GCF)")
    ap.add_argument("-s", "--serial", required=True,
                    help="the stick's USB serial number: only a port whose name carries it is used")
    ap.add_argument("-t", "--wait", type=float, default=90.0,
                    help="seconds to wait for a window per attempt")
    ap.add_argument("-n", "--attempts", type=int, default=1)
    ap.add_argument("--gcftmo", type=int, default=25, help="GCFFlasher -t")
    ap.add_argument("--dry-run", action="store_true",
                    help="guard and report the window, do not flash")
    args = ap.parse_args()

    gcf = os.path.abspath(args.gcf)
    if args.bin:
        binf = os.path.abspath(args.bin)
    elif gcf.lower().endswith(".gcf"):
        binf = gcf[:-4]
    else:
        print(f"autoflash: {gcf} does not end in .GCF: name the .bin it was made from")
        return 2
    if not os.path.isfile(gcf):
        print(f"autoflash: no such GCF: {gcf}")
        return 2
    if not os.path.isfile(binf):
        print(f"autoflash: no such image to compare against: {binf}")
        return 2

    # Mandatory guard, before GCFFlasher is even allowed to try.
    gcftool.guard(gcf, binf, "cb2")

    for attempt in range(1, args.attempts + 1):
        port, waited = wait_for_window(args.wait, args.serial)
        stamp = time.strftime("%H:%M:%S")
        if port is None:
            print(f"[{stamp}] attempt {attempt}: no window for {waited:.0f} s "
                  f"(board silent: it needs a power cycle, or the image on it "
                  f"neither resets itself nor is rescued by the watchdog)")
            continue
        print(f"[{stamp}] attempt {attempt}: window on {port} after {waited:.2f} s",
                  flush=True)
        if args.dry_run:
            print("  --dry-run: not flashing", flush=True)
            return 0
        # Stream, do not capture. A captured GCFFlasher is invisible while it is
        # stuck, and a stuck flasher is exactly the case where the operator needs
        # the live progress lines (a six-minute blind wait today, with the flasher
        # sitting in the handshake). Popen + line-by-line echo, then the verdict.
        proc = subprocess.Popen([GCFFLASHER, "-d", port, "-t", str(args.gcftmo),
                                 "-f", gcf], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True,
                                bufsize=1, universal_newlines=True)
        out = []
        for line in proc.stdout:
            line = line.strip()
            if line:
                out.append(line)
                print("  " + line[-120:], flush=True)
        proc.wait()
        if any("successful written" in l for l in out):
            print(f"  FLASH OK (attempt {attempt})", flush=True)
            return 0
        print("  flash failed")
    return 1


if __name__ == "__main__":
    sys.exit(main())
