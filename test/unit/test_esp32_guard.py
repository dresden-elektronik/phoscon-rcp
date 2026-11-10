#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""test_esp32_guard.py - the ESP32 pre-flash guard (tools/esp32_check.py) must refuse what the bootloader
would refuse, and what would land outside the app slot.

Each rule gets an image that satisfies it and one that does not, and the verdict - exit code and the rule
named in the message - is asserted. The images are built here, from the documented format, not by esptool:
a guard checked only against the tool it guards would share that tool's mistakes. Run by ctest as
`esp32_guard` (docs/testing.md §2).
"""
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GUARD = os.path.join(REPO, "tools", "esp32_check.py")

failures = 0


def descriptor(elf_sha=b"\0" * 32, magic=0xABCD5432, mmu_log2=16):
    d = struct.pack("<II8x", magic, 0) + b"0.0.0".ljust(32, b"\0") + b"t".ljust(32, b"\0")
    d += b"\0" * 64 + elf_sha + struct.pack("<HHB3x", 0, 99, mmu_log2) + b"\0" * 72
    assert len(d) == 256
    return d


def image(segments, chip_id=13, min_full=0, max_full=0xFFFF, entry=0x40800100, bad_sum=False,
          bad_hash=False, no_hash=False):
    """segments: (address, data) pairs; a flash segment is padded in front so its data lands at a file
    offset congruent with its address, the way esptool lays it out."""
    body = b""
    count = 0
    at = 24
    checksum = 0xEF
    for addr, data in segments:
        if 0x42000000 <= addr < 0x43000000 and (at + 8) % 0x10000 != addr % 0x10000:
            pad = (addr % 0x10000 - (at + 16)) % 0x10000
            body += struct.pack("<II", 0, pad) + b"\0" * pad
            at += 8 + pad
            count += 1
        body += struct.pack("<II", addr, len(data)) + data
        at += 8 + len(data)
        count += 1
        for b in data:
            checksum ^= b
    head = struct.pack("<BBBBI", 0xE9, count, 2, 0x2F, entry)
    head += struct.pack("<B3sHBHH4xB", 0xEE, b"\0\0\0", chip_id, 0, min_full, max_full, 0 if no_hash else 1)
    out = head + body
    out += b"\0" * (15 - len(out) % 16) + bytes([checksum ^ (1 if bad_sum else 0)])
    if not no_hash:
        digest = hashlib.sha256(out).digest()
        out += bytes([digest[0] ^ 1]) + digest[1:] if bad_hash else digest
    return out


def good_segments(elf_sha=b"\0" * 32, **kw):
    return [(0x42000020, descriptor(elf_sha, **kw) + b"\x11" * 16),
            (0x40800000, b"\x22" * 64),
            (0x42100020, b"\x33" * 32)]


def run(name, data, want_ok, want_text=None, elf=None, older=False):
    global failures
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "app.bin")
        with open(path, "wb") as f:
            f.write(data)
        cmd = [sys.executable, GUARD, "--board", "xiaoc6", path]
        if elf is not None:
            elf_path = os.path.join(tmp, "app.elf")
            with open(elf_path, "wb") as f:
                f.write(elf)
            now = time.time()
            image_at = now - 10 if older else now + 10
            os.utime(path, (image_at, image_at))
            os.utime(elf_path, (now, now))
            cmd += ["--elf", elf_path]
        r = subprocess.run(cmd, capture_output=True, text=True)
    ok = r.returncode == 0
    passed = ok == want_ok and (want_text is None or want_text in r.stdout)
    print(("  PASS  " if passed else "* FAIL* ") + name + ("" if passed else ": " + r.stdout.strip()))
    failures += 0 if passed else 1


def main():
    elf = b"\x7fELF this stands for the image's own ELF"
    sha = hashlib.sha256(elf).digest()

    run("a well-formed image passes", image(good_segments()), True)
    run("an image written from its ELF passes freshness", image(good_segments(sha)), True, elf=elf)

    bad_magic = bytearray(image(good_segments()))
    bad_magic[0] = 0xE8
    run("magic: not 0xE9", bytes(bad_magic), False, "magic:")
    run("chip: another chip's ID", image(good_segments(), chip_id=5), False, "chip:")
    run("chip: a revision range without the part", image(good_segments(), min_full=3), False, "chip:")
    run("checksum: one bit off", image(good_segments(), bad_sum=True), False, "checksum:")
    run("hash: one bit off", image(good_segments(), bad_hash=True), False, "hash:")
    run("hash: none appended", image(good_segments(), no_hash=True), False, "hash:")
    run("descriptor: segment 0 in RAM",
        image([good_segments()[1], good_segments()[0], good_segments()[2]]), False, "descriptor:")
    run("descriptor: no magic", image(good_segments(magic=0x12345678)), False, "descriptor:")
    run("descriptor: another MMU page", image(good_segments(mmu_log2=15)), False, "descriptor:")
    run("segments: a third flash segment",
        image(good_segments() + [(0x42200020, b"\x44" * 16)]), False, "segments:")
    run("segments: one flash segment", image(good_segments()[:2]), False, "segments:")
    run("segments: RAM above the app's end",
        image(good_segments() + [(0x4086E600, b"\x55" * 32)]), False, "segments:")
    run("segments: RTC memory", image(good_segments() + [(0x50000000, b"\x55" * 32)]), False, "segments:")
    misplaced = bytearray(image(good_segments()))
    struct.pack_into("<I", misplaced, 24, 0x42000040)   # segment 0's address no longer its offset's
    run("segments: a flash segment off its page offset", bytes(misplaced), False)
    run("slot: larger than the app slot",
        image(good_segments()[:2] + [(0x42100020, b"\x66" * 0x200000)]), False, "slot:")
    run("freshness: written from another ELF", image(good_segments()), False, "freshness:", elf=elf)
    run("freshness: older than its ELF", image(good_segments(sha)), False, "freshness:", elf=elf, older=True)

    print(f"esp32_guard: {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
