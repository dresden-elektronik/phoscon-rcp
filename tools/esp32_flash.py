#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""esp32_flash.py - write an app image into an ESP32 board's app slot, guarded (docs/build-and-flash.md §13).

The board keeps its ESP-IDF bootloader and partition table; this writes the app slot and nothing else:

  1. tools/esp32_check.py's rules for the board, on the image and its ELF
  2. the board's partition table read back and compared with the one the guard records for it, so the
     slot the image is written into is the slot the board boots
  3. esptool write-flash at the slot's origin
  4. the slot read back and compared with the image, byte for byte

esptool reaches the ROM's download mode through the USB Serial/JTAG controller's line state with the
running image untouched, and resets into the new image when it is done. Every esptool command this tool
issues is one of ALLOWED; nothing here can erase, write below the slot, or touch an eFuse.

  tools/esp32_flash.py --board xiaoc6 --port <port> \\
      build/fw-xiaoc6/phoscon-rcp.bin --elf build/fw-xiaoc6/phoscon-rcp
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import esp32_check  # noqa: E402

ALLOWED = ("read-flash", "write-flash")
PARTITION_TABLE = (0x8000, 0xC00)


def esptool_path():
    found = os.environ.get("ESPTOOL") or shutil.which("esptool")
    if found:
        return found
    venv = os.path.join(os.path.dirname(HERE), ".venv-tools", "bin", "esptool")
    return venv if os.access(venv, os.X_OK) else None


def esptool(tool, port, verb, *args):
    if verb not in ALLOWED:
        raise SystemExit(f"esp32_flash: refusing esptool {verb}: only {', '.join(ALLOWED)}")
    cmd = [tool, "--port", port, verb] + [str(a) for a in args]
    print("esp32_flash: " + " ".join(cmd))
    r = subprocess.run(cmd)
    if r.returncode != 0:
        raise SystemExit(f"esp32_flash: esptool {verb} failed ({r.returncode})")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("image")
    ap.add_argument("--board", required=True, choices=sorted(esp32_check.BOARDS))
    ap.add_argument("--port", required=True, help="the board's port; always explicit (AGENTS.md)")
    ap.add_argument("--elf", required=True, help="the ELF the image was written from")
    args = ap.parse_args(argv)

    board = esp32_check.BOARDS[args.board]
    origin, slot = board["slot"]
    with open(args.image, "rb") as f:
        data = f.read()
    try:
        esp32_check.check(data, args.board, args.elf, args.image)
    except esp32_check.Refused as e:
        print(f"esp32_flash: REFUSED by the guard: {e}")
        return 1
    if len(data) > slot:
        print("esp32_flash: REFUSED: the image does not fit the slot")
        return 1

    tool = esptool_path()
    if tool is None:
        print("esp32_flash: esptool not found (ESPTOOL, PATH or .venv-tools/bin)")
        return 2

    with tempfile.TemporaryDirectory() as tmp:
        table = os.path.join(tmp, "partition_table.bin")
        esptool(tool, args.port, "read-flash", hex(PARTITION_TABLE[0]), hex(PARTITION_TABLE[1]), table)
        with open(table, "rb") as f:
            seen = hashlib.sha256(f.read()).hexdigest()
        if seen != board["partition_table_sha256"]:
            print(f"esp32_flash: REFUSED: the board's partition table ({seen}) is not the one recorded for "
                  f"{args.board}; the app slot may not be at 0x{origin:x}")
            return 1

        esptool(tool, args.port, "write-flash", hex(origin), args.image)

        back = os.path.join(tmp, "slot.bin")
        esptool(tool, args.port, "read-flash", hex(origin), hex(len(data)), back)
        with open(back, "rb") as f:
            if f.read() != data:
                print("esp32_flash: the slot read back differs from the image")
                return 1

    print(f"esp32_flash: {len(data)} bytes at 0x{origin:x} on {args.port}, read back identical")
    return 0


if __name__ == "__main__":
    sys.exit(main())
