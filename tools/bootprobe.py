#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""Measure the CB2 reset -> bootloader window -> app timeline precisely.

Robust per-read error handling; logs every port transition with a timestamp.
"""
import cb2port
import sys
import time
import serial
import serial.tools.list_ports

PORT = cb2port.resolve()
RESET = bytes.fromhex("C0 0B 03 00 0C 00 05 00 26 02 00 00 00 B9 FF C0".replace(" ", ""))
HORIZON = 20.0


def present():
    return any(p.device == PORT for p in serial.tools.list_ports.comports())


def open_port():
    s = serial.Serial(PORT, 115200, timeout=0.05)
    s.reset_input_buffer()
    return s


def safe_read(s, n=256):
    try:
        return s.read(n)
    except (serial.SerialException, OSError):
        return None


def wait_present(t0, note):
    while time.time() - t0 < HORIZON:
        if present():
            print(f"[{time.time()-t0:8.3f}] {note}: port present")
            return True
        time.sleep(0.02)
    print(f"[{time.time()-t0:8.3f}] {note}: no port before horizon")
    return False


def main():
    t0 = time.time()
    s = open_port()
    print(f"[{time.time()-t0:8.3f}] reset sent")
    s.write(RESET)
    s.flush()

    # 1) wait for app disconnect
    gone1 = None
    while time.time() - t0 < HORIZON:
        d = safe_read(s)
        if d:
            print(f"[{time.time()-t0:8.3f}] RX: {d!r}")
        if not present():
            gone1 = time.time() - t0
            print(f"[{gone1:8.3f}] DISCONNECT 1 (app reset)")
            break
    if gone1 is None:
        return 1

    # 2) bootloader appears
    if not wait_present(t0, "bootloader"):
        return 1
    enc1 = time.time() - t0
    try:
        s.close()
    except Exception:
        pass
    s = open_port()
    banner = safe_read(s, 512)
    print(f"[{time.time()-t0:8.3f}] banner: {banner!r}")

    # 3) answer ID, then poll for window close
    s.write(b"ID")
    s.flush()
    id_t = None
    while time.time() - t0 < HORIZON:
        d = safe_read(s, 512)
        if d:
            print(f"[{time.time()-t0:8.3f}] ID answer: {d!r}")
            if id_t is None:
                id_t = time.time() - t0
            break
    if id_t is None:
        print("no ID answer")
        return 1

    gone2 = None
    while time.time() - t0 < HORIZON:
        d = safe_read(s, 256)
        if d:
            print(f"[{time.time()-t0:8.3f}] RX: {d!r}")
        if not present():
            gone2 = time.time() - t0
            print(f"[{gone2:8.3f}] DISCONNECT 2 (window closed)")
            break
    if gone2 is None:
        print("window did not close before horizon")
        return 0

    # 4) app back
    if wait_present(t0, "app"):
        enc3 = time.time() - t0
        print()
        print(f"window (bootloader visible): {enc1:.3f} -> {gone2:.3f} = {gone2-enc1:.3f} s")
        print(f"ID answered at {id_t:.3f} (enc1+{id_t-enc1:.3f} s)")
        print(f"app back at {enc3:.3f} (gone2+{enc3-gone2:.3f} s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
