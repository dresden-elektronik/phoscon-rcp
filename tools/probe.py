#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""Raw CDC probe: send raw bytes, dump whatever comes back (hex + ascii), and
decode the same bytes as debug records - a probe against our own app lands in
the record stream, not in text.  Records are reported after the raw dump so the
raw bytes stay the primary evidence (docs/coding-rules.md §5).

Usage: probe.py <port> [sendhex ...]
  no args after port: open, read 1 s, dump
  sendhex: one or more space-separated hex strings, each sent with a 0.5 s pause
"""
import sys
import time

import serial

import cdcview

if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
    sys.exit(__doc__)
PORT = sys.argv[1]
SEND = sys.argv[2:]


def dump(data, label="recv"):
    if not data:
        print(f"{label}: <no data>")
        return
    hexs = " ".join(f"{b:02X}" for b in data)
    asc = "".join(chr(b) if 32 <= b < 127 else "." for b in data)
    print(f"{label}: ({len(data)}) {hexs}")
    print(f"         {asc}")


VIEW = cdcview.CdcView("probe", hexdump=True)


def dump_decode(data, label):
    dump(data, label)
    for line in VIEW.feed(data or b""):
        print(f"         {line}")


def main():
    s = serial.Serial(PORT, 115200, timeout=1)
    s.reset_input_buffer()
    time.sleep(0.2)
    dump_decode(s.read(512), "idle")
    for i, hx in enumerate(SEND):
        raw = bytes.fromhex("".join(hx.split()))
        s.write(raw)
        s.flush()
        print(f"send[{i}]: ({len(raw)}) {' '.join(f'{b:02X}' for b in raw)}")
        time.sleep(0.7)
        dump_decode(s.read(512), f"recv[{i}]")
    s.close()
    for line in VIEW.flush():
        print(f"         {line}")
    if VIEW.records:
        print(f"         {VIEW.stats()}")


if __name__ == "__main__":
    main()
