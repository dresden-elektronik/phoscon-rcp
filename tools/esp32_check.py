#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""esp32_check.py - the pre-flash guard for an ESP32 board's app image (docs/build-and-flash.md §13).

An ESP32 board keeps its ESP-IDF bootloader and partition table; this tree writes only the app image into
the board's app slot, with esptool. The guard is the last host-side check before that write, and it reads
the image itself rather than trusting the tool that wrote it:

  magic, chip    magic 0xE9; the chip ID the board's chip has; a chip revision range that admits the part
  checksum, hash the XOR checksum over the segment data, and the appended SHA-256 over the whole image
  descriptor    segment 0 is flash-mapped and begins with the app descriptor (magic 0xABCD5432), whose MMU
                 page size is 64 KiB - without it the bootloader refuses the image (docs/hardware_esp32.md
                 §12.3)
  segments      exactly two flash-mapped segments, each at a file offset congruent with its address modulo
                 the MMU page; every RAM segment inside the RAM an app may use; no other segment but
                 esptool's padding
  slot          the image fits the board's app slot
  freshness     with --elf: the ELF's SHA-256 the descriptor carries is that ELF's, and the image is not
                 older than it

Exit 0 = every check passed; 1 = refused, with the rule named; 2 = usage. tools/esp32_flash.py runs it
before it writes anything, and test/unit/test_esp32_guard.py proves every rule can refuse.

  tools/esp32_check.py --board xiaoc6 build/fw-xiaoc6/phoscon-rcp.bin --elf build/fw-xiaoc6/phoscon-rcp
"""
import argparse
import hashlib
import os
import struct
import sys

IMAGE_MAGIC = 0xE9
APP_DESC_MAGIC = 0xABCD5432
HEADER_LEN = 24
SEGMENT_HEADER_LEN = 8
CHECKSUM_SEED = 0xEF
MMU_PAGE = 0x10000
MMU_PAGE_LOG2 = 16
FLASH_LOW, FLASH_HIGH = 0x42000000, 0x43000000   # the C6's shared instruction and data window
ELF_SHA256_AT = 0x90                              # in the descriptor: esptool writes the ELF's SHA-256 there
MMU_PAGE_SIZE_AT = 0xB4                           # in the descriptor: log2 of the MMU page

# Per board: the chip, the part revision on the bench, the RAM an app may use, the app slot, and the
# SHA-256 of the partition table the slot comes from (esp32_flash.py compares the board's own with it).
BOARDS = {
    "xiaoc6": {
        "chip_id": 13,
        "part_revision": 2,          # v0.2, read with esptool from the bench XIAO
        "ram": (0x40800000, 0x4086E610),
        "slot": (0x10000, 0x200000),
        "partition_table_sha256": "6a88d59601a83a16a19a08114b59d338324b8dec267d8b43e5d61ad56ec92102",
    },
}


class Refused(Exception):
    pass


def parse(data):
    """The image's header fields and its segments as (index, address, file offset of the data, length)."""
    if len(data) < HEADER_LEN:
        raise Refused("size: shorter than an image header")
    magic, count, _mode, _size_freq, entry = struct.unpack_from("<BBBBI", data, 0)
    if magic != IMAGE_MAGIC:
        raise Refused(f"magic: 0x{magic:02x}, not 0x{IMAGE_MAGIC:02x}")
    chip_id, _min_rev, min_full, max_full = struct.unpack_from("<HBHH", data, 12)
    hash_appended = data[23]
    segments = []
    at = HEADER_LEN
    for i in range(count):
        if at + SEGMENT_HEADER_LEN > len(data):
            raise Refused(f"segments: segment {i} header runs past the end")
        addr, length = struct.unpack_from("<II", data, at)
        at += SEGMENT_HEADER_LEN
        if length % 4 or at + length > len(data):
            raise Refused(f"segments: segment {i} length 0x{length:x} is not whole words inside the file")
        segments.append((i, addr, at, length))
        at += length
    return {"entry": entry, "chip_id": chip_id, "min_full": min_full, "max_full": max_full,
            "hash_appended": hash_appended, "segments": segments, "end": at}


def check_sums(data, image):
    checksum = CHECKSUM_SEED
    for _i, _addr, off, length in image["segments"]:
        for b in data[off:off + length]:
            checksum ^= b
    pad_end = (image["end"] + 16) & ~15        # the checksum is the last byte of the 16-byte padding
    if pad_end > len(data) or data[pad_end - 1] != checksum:
        raise Refused(f"checksum: the image's 0x{data[pad_end - 1]:02x} is not the segments' 0x{checksum:02x}"
                      if pad_end <= len(data) else "checksum: the image ends before its checksum")
    if image["hash_appended"] != 1:
        raise Refused("hash: no SHA-256 appended (the bootloader validates it)")
    if pad_end + 32 != len(data):
        raise Refused(f"hash: {len(data) - pad_end} bytes after the checksum, not the 32 of a SHA-256")
    if hashlib.sha256(data[:pad_end]).digest() != data[pad_end:]:
        raise Refused("hash: the appended SHA-256 is not the image's")


def is_flash(addr):
    return FLASH_LOW <= addr < FLASH_HIGH


def check_shape(data, image, board):
    if image["chip_id"] != board["chip_id"]:
        raise Refused(f"chip: ID {image['chip_id']}, the board's chip is {board['chip_id']}")
    if not image["min_full"] <= board["part_revision"] <= image["max_full"]:
        raise Refused(f"chip: revisions {image['min_full']}..{image['max_full']} exclude the part's "
                      f"{board['part_revision']}")

    segments = image["segments"]
    if not segments or not is_flash(segments[0][1]):
        raise Refused("descriptor: segment 0 is not flash-mapped")
    off0 = segments[0][2]
    magic, = struct.unpack_from("<I", data, off0)
    if magic != APP_DESC_MAGIC:
        raise Refused(f"descriptor: segment 0 begins with 0x{magic:08x}, not the app descriptor")
    if data[off0 + MMU_PAGE_SIZE_AT] != MMU_PAGE_LOG2:
        raise Refused(f"descriptor: MMU page size 2^{data[off0 + MMU_PAGE_SIZE_AT]}, not 64 KiB")

    ram_low, ram_end = board["ram"]
    flash = 0
    for i, addr, off, length in segments:
        if is_flash(addr):
            flash += 1
            if off % MMU_PAGE != addr % MMU_PAGE:
                raise Refused(f"segments: flash segment {i} at file offset 0x{off:x} for address 0x{addr:x}")
        elif addr == 0:
            continue                               # esptool's padding: neither loaded nor mapped
        elif not (ram_low <= addr and addr + length <= ram_end):
            raise Refused(f"segments: segment {i} 0x{addr:x}..0x{addr + length:x} is outside the app's RAM "
                          f"0x{ram_low:x}..0x{ram_end:x}")
    if flash != 2:
        raise Refused(f"segments: {flash} flash-mapped segments, the bootloader takes exactly 2")

    if not ram_low <= image["entry"] < ram_end and not is_flash(image["entry"]):
        raise Refused(f"segments: entry 0x{image['entry']:x} is neither in RAM nor in flash")

    _origin, slot = board["slot"]
    if len(data) > slot:
        raise Refused(f"slot: {len(data)} bytes, the app slot holds {slot}")


def check_fresh(data, image, elf_path, image_path):
    with open(elf_path, "rb") as f:
        elf = f.read()
    carried = data[image["segments"][0][2] + ELF_SHA256_AT:][:32]
    if carried != hashlib.sha256(elf).digest():
        raise Refused(f"freshness: the image was not written from {elf_path}")
    if image_path and os.path.getmtime(image_path) < os.path.getmtime(elf_path):
        raise Refused(f"freshness: {image_path} is older than {elf_path}")


def check(data, board_name, elf_path=None, image_path=None):
    board = BOARDS[board_name]
    image = parse(data)
    check_sums(data, image)
    check_shape(data, image, board)
    if elf_path:
        check_fresh(data, image, elf_path, image_path)
    return image


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("image")
    ap.add_argument("--board", required=True, choices=sorted(BOARDS))
    ap.add_argument("--elf", help="the ELF the image was written from")
    args = ap.parse_args(argv)

    with open(args.image, "rb") as f:
        data = f.read()
    try:
        image = check(data, args.board, args.elf, args.image)
    except Refused as e:
        print(f"esp32_check: REFUSED {args.image}: {e}")
        return 1
    flash = [s for s in image["segments"] if is_flash(s[1])]
    print(f"esp32_check: OK {args.image}: {len(data)} bytes, {len(image['segments'])} segments "
          f"({len(flash)} flash-mapped), entry 0x{image['entry']:08x}, for {args.board}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
