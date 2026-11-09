#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""Pre-flash guard for a board's GCF container: a container aimed at the wrong address can
overwrite the bootloader, so nothing is flashed that this guard has not passed.

GCF header: u32le magic 0xCAFEFEED (bytes ed fe fe ca), u8 type, u32le target_address, u32le
size, u8 CRC. The type byte is the container type the board's bootloader accepts: 1..10 on a
ConBee II, 31..39 on a RaspBee II; 0 = plaintext, which the UART upload path refuses to flash.
The ranges match GCFTool's board profiles (docs/build-and-flash.md §4) and the vendor's
release for both boards.

A released container (docs/build-and-flash.md §12) comes without its .bin: --released checks everything
that can be read from the container alone. Its .bin was checked when the release was built.

Checks (all must pass, exit 0; else non-zero):
  1. magic + size field consistent with the file length
  2. type inside the board's range (never plaintext 0)
  3. target_address at the board's app origin (0x5000 for cb2/rb2, 0x4000 for
     cb3; a 0x0 container would clobber the bootloader — the scenario this guard
     exists for)
  4. the image fits the region it must not touch: for cb2/rb2 the NVRAM at
     0x3EF00 must survive, for cb3 the image must stay inside the AppSection
     minus the AppInfo trailer
  5. for cb3, both checksum layers and the inner container's shape
  6. the GCF is the .bin's: its size and its plain-image checksum match the .bin (the header CRC-8
     for cb2, rb2 and cbrb1, the inner CRC-32 of the plain image for cb3), and it is not older than
     the .bin. On rb2 the CRC-8 covers the image with the bootloader's fields written in, as GCFTool
     writes them, and the .bin must leave their place empty. The checksum catches a container left over from another build, which a newer file
     date does not; the date still backs the CRC-8, which one changed image in 256 would pass.

Board: --board cb2 (default) | --board rb2 | --board cb3 | --board cbrb1. cb2 and
rb2 containers target 0x5000 and share the NVRAM bound; what differs is the key, so the type range
is the board's. cb3 carries the extended (ConBee III / Hive) container, whose payload is itself an
AES inner container; the guard checks both layers.

cbrb1 (ConBee I and RaspBee I) is one board to the guard: both run one image under one bootloader (docs/hardware_avr.md §11.2,
§11.10): type 1..10 (XTEA; plaintext 0 is accepted by the bootloader but refused here like
everywhere else), target 0, and the rules the bootloader and GCFFlasher impose on the size, read from
both sources. The bootloader writes pages from address 0 whatever the target says, so the bound is the
boot section at 0x3E000; it fills pages by 16-bit words, so an odd size loses its last byte; an exact
multiple of 256 makes it request a page past the end, which GCFFlasher refuses. The header CRC-8 is over
the plain image and a trailing partial XTEA block is stored in plain, so both are checked against the
.bin without any key.
"""
import os
import struct
import sys

GCF = "build/fw/phoscon-rcp.bin.GCF"
BIN = "build/fw/phoscon-rcp.bin"
APP_ORIGIN = 0x5000
NVRAM_BASE = 0x3EF00

BOARDS = {
    "cb2": (1, 10),   # GCFTool -b cb2 writes type 1
    "rb2": (31, 39),  # GCFTool -b rb2 writes type 31
    "cb3": (60, 60),  # extended container, inner AES image type 61..65
    "cbrb1": (1, 10), # GCFTool -b cbrb1 writes type 1
}

# rb2: the RaspBee II bootloader checks three u32 fields at image offset 0x4000, in the gap the image
# leaves zeroed below its code (src/targets/rb2/atsamr21.ld): the CRC-32 of the vector table, the CRC-32
# of the image from 0x4100, and the image size. GCFTool -b rb2 writes them into an image of at least
# 0x4100 bytes before it takes the header CRC-8.
RB2_FIELDS = 0x4000
RB2_FIELDS_LEN = 12
RB2_VECTORS_LEN = 0x100
RB2_APP_START = 0x4100

AVR_BOARDS = ("cbrb1",)
AVR_BOOT_BASE = 0x3E000   # 8 KiB boot section; the application is 0..0x3DFFF
AVR_PAGE = 256

# cb3: bootloader layout (deSiLabs bootloader, MG21). The AppSection is
# appSectorSize bytes long starting at the app origin; the bootloader writes its
# 16-byte AppInfo trailer at the very end of it, so the app must leave that
# trailer's room. Same arithmetic, same constants as src/targets/cb3/flash_layout.cmake,
# which derives them from the part's size and page size and is checked by the cb3_layout host test.
CB3_APP_SECTOR_SIZE = 0x78000   # 0x4000..0x7BFFF
CB3_APP_INFO_SIZE = 16
CB3_MAGIC_CONBEE3 = 0xDEC0DE03
CB3_MAGIC_HIVE = 0xDEC0DE02


def crc32(data: bytes) -> int:
    import zlib
    return zlib.crc32(data) & 0xFFFFFFFF


def stale(gcf: str, binp: str) -> bool:
    """1 when the container is older than the image: a rebuild that was not repackaged."""
    if os.path.getmtime(gcf) + 1 < os.path.getmtime(binp):
        print(f"gcf_check: {gcf} STALE — repackage from {binp} first")
        return True
    return False


def check_cb3(gcf: str, board: str, addr: int, size: int, fsize: int,
              binp: str) -> int:
    """Verify the nested ConBee III container: outer CRC-8 over the stored inner
    container, inner CRC-32 tail, product magic, and the AppSection bound that
    leaves room for the 16-byte AppInfo trailer the bootloader writes at 0x7BFF0."""
    with open(gcf, "rb") as f:
        whole = f.read()
    inner = whole[14:]
    magic = struct.unpack_from("<I", inner, 0)[0]
    if magic not in (CB3_MAGIC_CONBEE3, CB3_MAGIC_HIVE):
        print(f"gcf_check: inner magic 0x{magic:08X} is not ConBee III/Hive")
        return 1
    inner_size = struct.unpack_from("<I", inner, 4)[0]
    if inner_size != size:
        print(f"gcf_check: inner size {inner_size} != outer payload {size}")
        return 1
    outer_crc = whole[13]
    if outer_crc != crc8(inner):
        print(f"gcf_check: outer CRC-8 0x{outer_crc:02X} != computed "
              f"0x{crc8(inner):02X} (over the stored inner container)")
        return 1
    if struct.unpack_from("<I", inner, inner_size - 4)[0] != crc32(inner[:-4]):
        print("gcf_check: inner CRC-32 tail does not match")
        return 1
    image_size = struct.unpack_from("<I", inner, 8)[0]
    image_type = struct.unpack_from("<I", inner, 12)[0]
    target = struct.unpack_from("<I", inner, 16)[0]
    plain_size = struct.unpack_from("<I", inner, 20)[0]
    if target != addr:
        print(f"gcf_check: inner target 0x{target:X} != outer 0x{addr:X}")
        return 1
    if not (61 <= image_type <= 65):
        print(f"gcf_check: inner image type {image_type} not 61..65 (the "
              "bootloader dropped the plain type 60: a plain app is refused)")
        return 1
    limit = CB3_APP_SECTOR_SIZE - CB3_APP_INFO_SIZE
    if plain_size > limit:
        print(f"gcf_check: plain image {plain_size} B exceeds the AppSection "
              f"budget {limit} B ({CB3_APP_SECTOR_SIZE} B AppSection minus the "
              f"{CB3_APP_INFO_SIZE}-byte AppInfo trailer the bootloader writes "
              "at its end)")
        return 1
    if image_size != 20 + ((plain_size + 15) // 16 * 16):
        print(f"gcf_check: image size {image_size} inconsistent with plain "
            f"size {plain_size} (0xFF stuffing to the AES block size)")
        return 1
    if binp:
        # The inner header carries the plain image's size and CRC-32 (+20, +24), which is what the
        # bootloader checks after it decrypts: a container from another build cannot pass for this one.
        with open(binp, "rb") as f:
            image = f.read()
        if plain_size != len(image):
            print(f"gcf_check: plain size {plain_size} != image {len(image)} B in {binp}: "
                  "the container is from another build")
            return 1
        plain_crc = struct.unpack_from("<I", inner, 24)[0]
        if plain_crc != crc32(image):
            print(f"gcf_check: plain CRC-32 0x{plain_crc:08X} != 0x{crc32(image):08X} of {binp}: "
                  "the container is from another build")
            return 1
        if stale(gcf, binp):
            return 1
    print(f"gcf_check: OK board={board} type={whole[4]} target=0x{addr:X} "
          f"inner={size} image type={image_type} plain={plain_size} "
          f"(AppSection budget {limit} B)")
    return 0


def check_avr(gcf: str, board: str, addr: int, size: int, fsize: int,
              binp: str) -> int:
    """Verify a ConBee I / RaspBee I container against the plain image it was packed from."""
    if addr != 0:
        print(f"gcf_check: target 0x{addr:X} != 0x0 (the bootloader writes from 0 "
              "whatever the target says; it only bounds the CRC read-back)")
        return 1
    if size != fsize - 14:
        print(f"gcf_check: size field {size} != file payload {fsize - 14}")
        return 1
    with open(gcf, "rb") as f:
        whole = f.read()
    image = None
    if binp:
        with open(binp, "rb") as f:
            image = f.read()
    payload = whole[14:]
    if image is not None and size != len(image):
        print(f"gcf_check: size field {size} != image {len(image)} B in {binp}")
        return 1
    if size >= AVR_BOOT_BASE:
        print(f"gcf_check: image 0x0..0x{size:X} reaches the bootloader at "
              f"0x{AVR_BOOT_BASE:X} (the write loop is bounded by the size, not the app area)")
        return 1
    if size % 2:
        print(f"gcf_check: size {size} is odd (the bootloader fills pages by words "
              "and drops the last byte, so its CRC fails)")
        return 1
    if size % AVR_PAGE == 0:
        print(f"gcf_check: size {size} is a multiple of {AVR_PAGE} (the bootloader "
              "requests a page past the end, which GCFFlasher refuses)")
        return 1
    if image is None:
        print(f"gcf_check: OK board={board} type={whole[4]} target=0x0 size={size} "
              f"(ends 0x{size:X} < 0x{AVR_BOOT_BASE:X}), released container")
        return 0
    reset = image[0] | image[1] << 8
    if reset & 0xFE0E != 0x940C:
        print(f"gcf_check: image word 0 is 0x{reset:04X}, not a JMP (no reset vector at 0)")
        return 1
    if whole[13] != crc8(image):
        print(f"gcf_check: header CRC-8 0x{whole[13]:02X} != computed 0x{crc8(image):02X} "
              f"over the plain image {binp}")
        return 1
    tail = size % 8
    body = size - tail
    if payload[body:] != image[body:]:
        print(f"gcf_check: last {tail} bytes differ from the image (a trailing partial "
              "XTEA block is stored in plain)")
        return 1
    if body and payload[:body] == image[:body]:
        print("gcf_check: payload equals the plain image, but the type says XTEA")
        return 1
    if stale(gcf, binp):
        return 1
    print(f"gcf_check: OK board={board} type={whole[4]} target=0x0 size={size} "
          f"(ends 0x{size:X} < 0x{AVR_BOOT_BASE:X}) crc=0x{whole[13]:02X}")
    return 0


def rb2_plain(image: bytes) -> bytes:
    """The plain image a RaspBee II container's header CRC-8 covers: the .bin with the bootloader's
    fields written in, when it is long enough to carry them."""
    if len(image) < RB2_APP_START:
        return image
    plain = bytearray(image)
    struct.pack_into("<III", plain, RB2_FIELDS, crc32(image[:RB2_VECTORS_LEN]),
                     crc32(image[RB2_APP_START:]), len(image))
    return bytes(plain)


def crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x31) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def main() -> int:
    args = sys.argv[1:]
    if "-h" in args or "--help" in args:
        print(__doc__)
        return 0
    board = "cb2"
    if "--board" in args:
        i = args.index("--board")
        if i + 1 >= len(args) or args[i + 1] not in BOARDS:
            print(f"gcf_check: --board needs one of {', '.join(sorted(BOARDS))}")
            return 1
        board = args[i + 1]
        del args[i:i + 2]
    released = "--released" in args
    if released:
        args.remove("--released")
    lo, hi = BOARDS[board]
    # Positional, but each defaults independently: passing only the GCF must
    # not crash (sys.argv[1:3] unpacking a 1-element slice did).
    gcf = args[0] if len(args) > 0 else GCF
    binp = None if released else (args[1] if len(args) > 1 else BIN)
    if not (os.path.isfile(gcf) and (released or os.path.isfile(binp))):
        print(f"gcf_check: missing {gcf} or {binp}")
        return 1
    with open(gcf, "rb") as f:
        h = f.read(14)
    fsize = os.path.getsize(gcf)
    if len(h) < 14 or struct.unpack("<I", h[0:4])[0] != 0xCAFEFEED:
        print(f"gcf_check: BAD MAGIC {h[:4].hex()}")
        return 1
    ftype, addr, size = struct.unpack_from("<BII", h, 4)
    if ftype < lo or ftype > hi:
        print(f"gcf_check: type {ftype} not {lo}..{hi} for board {board} "
              f"(0=plaintext, unflashed)")
        return 1
    if board in AVR_BOARDS:
        return check_avr(gcf, board, addr, size, fsize, binp)
    expected_origin = 0x4000 if board == "cb3" else APP_ORIGIN
    if addr != expected_origin:
        print(f"gcf_check: target 0x{addr:X} != 0x{expected_origin:X} "
              f"(0x0 would overwrite the bootloader)")
        return 1
    if size != fsize - 14:
        print(f"gcf_check: size field {size} != file payload {fsize - 14}")
        return 1
    if board == "cb3":
        return check_cb3(gcf, board, addr, size, fsize, binp)
    if addr + size >= NVRAM_BASE:
        print(f"gcf_check: image 0x{addr:X}..0x{addr + size:X} "
              f"overruns NVRAM @ 0x{NVRAM_BASE:X}")
        return 1
    if binp:
        # The size field and the header CRC-8 describe the plain image (GCFTool, -b cb2 and -b rb2).
        with open(binp, "rb") as f:
            image = f.read()
        if size != len(image):
            print(f"gcf_check: size field {size} != image {len(image)} B in {binp}: "
                  "the container is from another build")
            return 1
        if board == "rb2" and len(image) >= RB2_APP_START:
            if any(image[RB2_FIELDS:RB2_FIELDS + RB2_FIELDS_LEN]):
                print(f"gcf_check: {binp} holds data at 0x{RB2_FIELDS:X}, where GCFTool writes the "
                      "bootloader's fields")
                return 1
            image = rb2_plain(image)
        if h[13] != crc8(image):
            print(f"gcf_check: header CRC-8 0x{h[13]:02X} != 0x{crc8(image):02X} of {binp}: "
                  "the container is from another build")
            return 1
        if stale(gcf, binp):
            return 1
    print(f"gcf_check: OK board={board} type={ftype} target=0x{addr:X} "
          f"size={size} (ends 0x{addr + size:X} < 0x{NVRAM_BASE:X})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
