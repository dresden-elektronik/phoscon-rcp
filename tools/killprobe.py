#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""Measure the app kill time precisely.

After the flasher resets the device into the bootloader window, poll:
  * ZM "ID" probe every ~0.3 s: the bootloader answers, our app stays
    silent. So a run of silences in the middle of the timeline is the
    app's lifetime (jump -> kill), bounded by bootloader answers.
  * ioreg product name: phoscon-rcp appears only if our app's USB
    comes up.
  * the CDC stream itself, decoded (tools/cb2watch.py): our app's 8-byte
    debug records.  The probe and the records share ONE port handle - the
    previous version reopened the port every poll and flushed its input,
    which is what threw the record trail away.

Usage: killprobe.py [--no-flash] [--port <device>]   (or $CB2_PORT)
"""
import cb2flasher
import gcftool
import os
import subprocess
import sys

import cb2port
import cb2watch

def main() -> None:
    port = cb2port.resolve(sys.argv)
    if port is None:
        print("killprobe.py: no port: pass --port <device> or set CB2_PORT", file=sys.stderr)
        raise SystemExit(2)

    if "--no-flash" not in sys.argv:
        gcftool.package("build/fw/phoscon-rcp.bin", "cb2")
        # Pre-flash guard: offset 0x5000, size vs NVRAM, not stale.
        subprocess.run([".venv/bin/python", "tools/gcf_check.py",
                        "build/fw/phoscon-rcp.bin.GCF"],
                       check=True)
        subprocess.run(
            [cb2flasher.resolve(), "-d", port, "-t", "20",
             "-f", "build/fw/phoscon-rcp.bin.GCF"],
            check=True,
        )

    cb2watch.watch(35.0)


if __name__ == "__main__":
    main()
