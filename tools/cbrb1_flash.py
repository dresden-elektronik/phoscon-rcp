#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""Flash a ConBee I that runs the rcp image, over its serial port alone (docs/hardware_avr.md §11.7).

The image listens at 57 600 and the bootloader at 38 400, so a flasher that sends its reset frame at
38 400 goes unheard. GCFFlasher 4.12.4 or newer handles that by itself; this is for an older one. It sends
the reset frame at 57 600, changes rate inside the bootloader's 0.5 s window, and uploads the container
through the dialog of docs/hardware_avr.md §11.2.

  python3 tools/cbrb1_flash.py --port <port> build/fw/phoscon-rcp.bin.GCF build/fw/phoscon-rcp.bin
"""
import argparse
import os
import struct
import subprocess
import sys
import time

try:
    import serial
except ImportError as e:
    sys.exit(f"missing: {e.name}")

GCF_MAGIC = 0xCAFEFEED
GCF_HEADER = 14
PAGE = 256
APP_BAUD, BOOT_BAUD = 57600, 38400
RESET_FRAME = bytes.fromhex("C00B03000C0005002602000000B9FFC0")
RESET_REPLY = bytes.fromhex("C00B0300")
UNLOCK = bytes([0x1A, 0x1C, 0xA9, 0xAE])


def guard(gcf, image):
    """Run the mandatory pre-flash guard; a failure must stop here, not at the board."""
    tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gcf_check.py")
    argv = [sys.executable, tool, gcf, image if image else "--released", "--board", "cbrb1"]
    res = subprocess.run(argv, capture_output=True, text=True)
    print(res.stdout.strip() or res.stderr.strip())
    if res.returncode != 0:
        sys.exit("guard refused the container; nothing was flashed")


def read_until(port, done, seconds):
    """Read until done(buffer) holds or the time is up; the buffer either way."""
    buf = b""
    end = time.time() + seconds
    while time.time() < end and not done(buf):
        buf += port.read(256)
    return buf


def upload(port, data, size):
    """Answer the bootloader's GET requests until the last page has gone. 1 on success."""
    buf = b""
    end = time.time() + 5
    while time.time() < end:
        buf += port.read(64)
        at = buf.find(b"GET")
        if at < 0 or len(buf) < at + 6 or buf[at + 5] != ord(";"):
            continue
        page = buf[at + 3] | buf[at + 4] << 8
        buf = buf[at + 6 :]
        chunk = data[page * PAGE : (page + 1) * PAGE]
        port.write(chunk)
        end = time.time() + 5
        if page * PAGE + len(chunk) >= size:
            return 1
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("gcf")
    ap.add_argument("bin", nargs="?", help="the binary the container was made from, for the guard's freshness check")
    args = ap.parse_args()

    guard(args.gcf, args.bin)
    with open(args.gcf, "rb") as f:
        gcf = f.read()
    magic, ftype, target, size, crc = struct.unpack("<IBIIB", gcf[:GCF_HEADER])
    if magic != GCF_MAGIC or len(gcf) != GCF_HEADER + size:
        sys.exit("not a whole GCF container")

    port = serial.Serial(args.port, APP_BAUD, timeout=0)
    port.reset_input_buffer()
    port.write(RESET_FRAME)
    if RESET_REPLY not in read_until(port, lambda b: RESET_REPLY in b, 1.0):
        print("no reply to the reset frame at 57 600: looking for the bootloader anyway")
    port.baudrate = BOOT_BAUD

    if b"Bootloader" not in read_until(port, lambda b: b"Bootloader" in b and b.endswith(b"\n"), 3.0):
        sys.exit("no bootloader banner; is this stick running the rcp image?")
    port.write(UNLOCK)
    if b"READY" not in read_until(port, lambda b: b"READY" in b, 1.0):
        sys.exit("the bootloader did not take the keys")
    port.write(struct.pack("<IIBB", size, target, ftype, crc))

    if not upload(port, gcf[GCF_HEADER:], size):
        sys.exit("the bootloader stopped asking for pages")
    answer = read_until(port, lambda b: b"VALID CRC" in b, 8.0)
    if b"#VALID CRC" not in answer:
        sys.exit(f"validation failed: {answer!r}")
    print("firmware successful written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
