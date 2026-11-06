#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""test_gcf_guard.py - the pre-flash guard (tools/gcf_check.py) must refuse what would brick a board.

The guard is the last host-side check before GCFFlasher writes: magic, the board's container type, the
app origin, the region the image must not touch, both checksum layers of the ConBee III's nested
container, and freshness against the binary it was packaged from. Each of those is a rule a bug in the
guard can break, and a guard that fails open looks exactly like one that passes - so every rule gets a
container that satisfies it and one that does not, and the verdict (exit code plus the reason in the
message) is asserted.

The containers are built here, not copied from a board or a vendor artifact: the guard checks structure
and checksums and never looks inside the payload, so a synthetic container exercises every rule. Run by
ctest as `gcf_guard` (docs/testing.md §2).
"""
import os
import struct
import subprocess
import sys
import tempfile
import time
import zlib

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GUARD = os.path.join(REPO, "tools", "gcf_check.py")

GCF_MAGIC = 0xCAFEFEED
CB3_ORIGIN = 0x4000
CB3_APP_SECTOR = 0x78000
CB3_APPINFO = 16
CB3_BOUND = CB3_ORIGIN + CB3_APP_SECTOR - CB3_APPINFO   # 0x7BFF0
CB3_BUDGET = CB3_BOUND - CB3_ORIGIN                      # 491504
AVR_BOUND = 0x3E000                                      # the RaspBee I boot section

failures = 0


def crc8(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x31) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def wrap(payload, type_byte, target, crc=None, magic=GCF_MAGIC):
    """The 14-byte GCF header, then the payload as stored."""
    head = struct.pack("<IBII", magic, type_byte, target, len(payload))
    return head + bytes([crc8(payload) if crc is None else crc]) + payload


def cb3(outer_type=60, outer_target=CB3_ORIGIN, inner_target=CB3_ORIGIN,
        plain_size=4096, image_size=None, image_type=61, product=0xDEC0DE03,
        outer_crc=None, inner_crc=None, inner_total=None, magic=GCF_MAGIC):
    """A structurally valid ConBee III container: outer header over the nested inner container."""
    data_len = (plain_size + 15) // 16 * 16
    if image_size is None:
        image_size = 20 + data_len
    # The container size always describes the bytes actually stored; only the declared image size
    # may be a lie, which is what the guard's image-size consistency rule is for.
    stored = 8 + 20 + data_len + 4
    inner = struct.pack("<II", product, stored if inner_total is None else inner_total)
    inner += struct.pack("<IIII", image_size, image_type, inner_target, plain_size)
    inner += struct.pack("<I", zlib.crc32(b"\xFF" * ((plain_size + 3) // 4 * 4)) & 0xFFFFFFFF)
    inner += b"\x5A" * data_len   # the guard never looks inside the payload
    inner += struct.pack("<I", zlib.crc32(inner) & 0xFFFFFFFF if inner_crc is None else inner_crc)
    return wrap(inner, outer_type, outer_target, outer_crc, magic)


def classic(payload_len=1024, type_byte=1, target=0x5000, size=None, crc=None):
    return wrap(b"\x11" * payload_len, type_byte, target, crc) if size is None else \
        struct.pack("<IBII", GCF_MAGIC, type_byte, target, size) + bytes([crc8(b"\x11" * payload_len) if crc is None else crc]) + b"\x11" * payload_len


def avr_image(length=1026):
    """A plain image with a JMP at word 0, the shape the RaspBee I bootloader starts."""
    body = bytes((i * 7 + 3) & 0xFF for i in range(length - 4))
    return b"\x0C\x94\x00\x01" + body


def avr(image=None, type_byte=1, target=0, crc=None, plain=False, tail=None, size=None):
    """A ConBee I / RaspBee I container over `image`: header CRC-8 over the plain image, every whole
    8-byte block changed (the guard holds no key, so any change stands in for XTEA), the trailing
    partial block stored in plain."""
    image = avr_image() if image is None else image
    body = len(image) - len(image) % 8
    payload = image if plain else bytes(b ^ 0xA5 for b in image[:body]) + image[body:]
    if tail is not None:
        payload = payload[:body] + tail
    head = struct.pack("<IBII", GCF_MAGIC, type_byte, target, len(payload) if size is None else size)
    return head + bytes([crc8(image) if crc is None else crc]) + payload


def verdict(board, gcf, work, tag, image=None):
    """Run the guard on a container and a fresh image; return (exit code, message)."""
    gcf_path = os.path.join(work, tag + ".GCF")
    bin_path = os.path.join(work, tag + ".bin")
    with open(gcf_path, "wb") as f:
        f.write(gcf)
    with open(bin_path, "wb") as f:
        f.write(b"\x00" * 1024 if image is None else image)
    now = time.time()
    os.utime(gcf_path, (now, now))
    os.utime(bin_path, (now - 60, now - 60))
    r = subprocess.run([sys.executable, GUARD, "--board", board, gcf_path, bin_path],
                       capture_output=True, text=True)
    return r.returncode, (r.stdout + r.stderr).strip()


def expect(name, board, gcf, want_rc, *words, image=None):
    """Assert the verdict: the exit code, and every wanted word in the guard's own message."""
    global failures
    work = tempfile.mkdtemp(prefix="guard_")
    rc, msg = verdict(board, gcf, work, "c", image)
    line = msg.splitlines()[-1] if msg else ""
    ok = rc == want_rc and all(w in msg for w in words)
    print(("  PASS  " if ok else "* FAIL* ") + f"{name}: rc={rc} want={want_rc} :: {line}")
    if not ok:
        failures += 1


def main() -> int:
    global failures
    if not os.path.isfile(GUARD):
        raise SystemExit(f"no guard at {GUARD}")

    print("== cb3: the nested container ==")
    expect("valid container", "cb3", cb3(), 0, "OK", "board=cb3", "491504")
    expect("outer CRC-8 wrong", "cb3", cb3(outer_crc=0x11), 1, "CRC-8")
    expect("inner CRC-32 tail wrong", "cb3", cb3(inner_crc=0xDEADBEEF), 1, "CRC-32")
    expect("no GCF magic", "cb3", cb3(magic=0x12345678), 1, "MAGIC")
    expect("plaintext type refused", "cb3", cb3(outer_type=0), 1, "type 0 not 60..60")
    expect("a cb2 container refused for cb3", "cb3", cb3(outer_type=1), 1, "not 60..60")
    expect("target 0 would overwrite the bootloader", "cb3", cb3(outer_target=0), 1, "target", "bootloader")
    expect("inner target differs from outer", "cb3", cb3(inner_target=0x8000), 1, "inner target")
    expect("inner size field disagrees with the payload", "cb3", cb3(inner_total=100), 1, "inner size")
    expect("wrong product magic", "cb3", cb3(product=0x11223344), 1, "not ConBee III/Hive")
    expect("Hive magic accepted on the cb3 guard", "cb3", cb3(product=0xDEC0DE02), 0, "OK")
    expect("app past the AppSection budget", "cb3", cb3(plain_size=CB3_BUDGET + 1), 1, "AppSection", "budget")
    expect("app at exactly the budget passes", "cb3", cb3(plain_size=CB3_BUDGET), 0, "OK")
    expect("plain image type 60 refused", "cb3", cb3(image_type=60), 1, "61..65")
    expect("compressed image type refused", "cb3", cb3(image_type=71), 1, "61..65")
    expect("bootloader image type refused", "cb3", cb3(image_type=81), 1, "61..65")
    expect("image size inconsistent with the plain size", "cb3", cb3(image_size=100), 1, "inconsistent")

    print("== cb2 and rb2: the classic container ==")
    expect("valid cb2 container", "cb2", classic(), 0, "OK", "board=cb2", "0x3EF00")
    expect("plaintext type refused", "cb2", classic(type_byte=0), 1, "type 0")
    expect("an rb2 container refused for cb2", "cb2", classic(type_byte=31), 1, "not 1..10")
    expect("valid rb2 container", "rb2", classic(type_byte=31), 0, "OK", "board=rb2")
    expect("a cb2 container refused for rb2", "rb2", classic(type_byte=1), 1, "not 31..39")
    expect("target 0 would overwrite the bootloader", "cb2", classic(target=0), 1, "target", "bootloader")
    expect("image reaching NVRAM refused", "cb2", classic(payload_len=0x3A000), 1, "NVRAM")
    expect("size field not matching the file", "cb2", classic(size=999), 1, "size field")

    print("== cbrb1: the RaspBee I bootloader's container ==")
    img = avr_image()
    expect("valid container", "cbrb1", avr(img), 0, "OK", "board=cbrb1", "0x3E000", image=img)
    expect("highest XTEA type accepted", "cbrb1", avr(img, type_byte=10), 0, "OK", "type=10", image=img)
    expect("plaintext type refused", "cbrb1", avr(img, type_byte=0, plain=True), 1, "type 0", image=img)
    expect("an rb2 container refused", "cbrb1", avr(img, type_byte=31), 1, "not 1..10", image=img)
    expect("target not 0 refused", "cbrb1", avr(img, target=0x5000), 1, "target", image=img)
    expect("size field not matching the file", "cbrb1", avr(img, size=999), 1, "size field", image=img)
    expect("container from another image", "cbrb1", avr(img), 1, "!= image", image=img + b"\x00\x00")
    big = avr_image(AVR_BOUND + 2)
    expect("image reaching the bootloader refused", "cbrb1", avr(big), 1, "bootloader", image=big)
    odd = avr_image(1025)
    expect("odd size refused", "cbrb1", avr(odd), 1, "odd", image=odd)
    full = avr_image(1024)
    expect("whole-page size refused", "cbrb1", avr(full), 1, "multiple of 256", image=full)
    novec = b"\xFF\xFF" + img[2:]
    expect("no JMP at word 0 refused", "cbrb1", avr(novec), 1, "JMP", image=novec)
    expect("header CRC-8 wrong", "cbrb1", avr(img, crc=crc8(img) ^ 1), 1, "CRC-8", image=img)
    expect("plain tail altered", "cbrb1", avr(img, tail=b"\x00\x00"), 1, "plain", image=img)
    expect("plain payload under an XTEA type", "cbrb1", avr(img, plain=True), 1, "XTEA", image=img)

    print("== freshness ==")
    work = tempfile.mkdtemp(prefix="guard_")
    gcf_path = os.path.join(work, "c.GCF")
    bin_path = os.path.join(work, "c.bin")
    with open(gcf_path, "wb") as f:
        f.write(classic())
    with open(bin_path, "wb") as f:
        f.write(b"\x00" * 1024)
    old = time.time() - 120
    os.utime(gcf_path, (old, old))
    os.utime(bin_path, None)
    r = subprocess.run([sys.executable, GUARD, "--board", "cb2", gcf_path, bin_path],
                       capture_output=True, text=True)
    msg = (r.stdout + r.stderr).strip()
    ok = r.returncode == 1 and "STALE" in msg
    print(("  PASS  " if ok else "* FAIL* ") + f"container older than its image: rc={r.returncode} :: {msg.splitlines()[-1] if msg else ''}")
    if not ok:
        failures += 1

    print("== --released: a container without its .bin ==")
    for name, gcf, want in (("released, valid", classic(), 0),
                            ("released, aimed at the bootloader", classic(target=0), 1),
                            ("released, cb3, valid", cb3(), 0),
                            ("released, AVR, odd size", avr(avr_image(1025)), 1)):
        board = "cb3" if "cb3" in name else "cbrb1" if "AVR" in name else "cb2"
        path = os.path.join(tempfile.mkdtemp(prefix="guard_"), "released.GCF")
        with open(path, "wb") as f:
            f.write(gcf)
        r = subprocess.run([sys.executable, GUARD, path, "--released", "--board", board],
                           capture_output=True, text=True)
        ok = r.returncode == want
        print(("  PASS  " if ok else "* FAIL* ") + f"{name}: rc={r.returncode} want={want}")
        if not ok:
            failures += 1

    if failures:
        print(f"gcf_guard: {failures} failure(s)")
        return 1
    print("gcf_guard: every rule the guard exists for is refused, and every valid container passes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
