#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""Wait for the CB2 to enumerate, then flash and monitor the boot.

The device can be left as a silent 8 MHz zombie (our app alive but USB
down) that the flasher cannot reach. A power cycle brings the GCF
bootloader up (product "ConBee II"). This script polls ioreg and, as
soon as the device appears, packages + runs the flasher (which connects,
resets into the window and uploads) and then monitors the boot exactly
like killprobe (ZM ID + product name + the decoded CDC record stream).

Usage: waitflash.py [wait_seconds]   (default wait 240 s)
"""
import cb2flasher
import gcftool
import cb2port
import os
import re
import subprocess
import sys
import time

import cb2watch

PORT = cb2port.resolve()
WATCH_S = 35.0


def device_up() -> str:
    out = subprocess.run(
        ["ioreg", "-p", "IOUSB", "-l", "-w0"],
        capture_output=True, text=True,
    ).stdout
    for p in re.findall(r'"USB Product Name" = "([^"]*)"', out):
        if p in ("ConBee II", "phoscon-rcp"):
            return p
    return ""


def product() -> str:
    return device_up() or "-"


def main() -> None:
    wait_limit = float(sys.argv[1]) if len(sys.argv) > 1 else 240.0

    print(f"Waiting up to {wait_limit:.0f} s for the CB2 to enumerate "
          "(power-cycle the device now)...", flush=True)
    t_wait = time.time()
    up = ""
    while time.time() - t_wait < wait_limit:
        up = device_up()
        if up:
            break
        time.sleep(0.15)
    if not up:
        print("Timed out waiting for the device.", flush=True)
        sys.exit(1)
    print(f"Device up ({up}) after {time.time() - t_wait:.1f} s; flashing now.",
          flush=True)

    # Build, package and guard (board cb2) in one step; a failed build stops before packaging.
    gcf = gcftool.build_package_guard("build/fw", "cb2")
    subprocess.run([cb2flasher.resolve(), "-d", PORT, "-t", "60", "-f", gcf], check=True)

    cb2watch.watch(WATCH_S)


if __name__ == "__main__":
    main()
