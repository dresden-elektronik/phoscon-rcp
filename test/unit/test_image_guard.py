#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""test_image_guard.py — the AppSection size guard, checked on both sides of its bound.

`src/targets/cb3/image_guard.cmake` refuses a flat image that would reach the 16-byte AppInfo
trailer the bootloader writes at the end of the AppSection: `file(SIZE)` the bin and
`FATAL_ERROR` unless it is <= MAX_BYTES, where MAX_BYTES is derived by the build as
DS_IMAGE_BOUND - APP_ORIGIN (0x7BFF0 - 0x4000 = 491504).

The guard's comparison has never been exercised in the real build: the linker's FLASH region ends
at the same address, so an image that big fails the link one byte before the guard would see it,
and the guard has only ever printed its pass line. This test feeds the script a bin at the bound
(must pass, it is `LESS_EQUAL`), one byte over (must FATAL_ERROR), and confirms the number it is
handed is the documented 491504 - so the failure path is proven, not assumed (docs/testing.md §5).
"""
import os
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GUARD = os.path.join(REPO, "src", "targets", "cb3", "image_guard.cmake")

# The bound the build derives for cb3: the AppSection end minus the app origin.
IMAGE_BOUND = 0x7BFF0
APP_ORIGIN = 0x4000
MAX_BYTES = IMAGE_BOUND - APP_ORIGIN  # 491504

failures = 0


def check(cond, desc, detail=""):
    global failures
    if cond:
        print(f"  ok    {desc}")
    else:
        failures += 1
        print(f"  FAIL  {desc}" + (f"  [{detail}]" if detail else ""))


def main():
    cmake = shutil.which("cmake")
    check(cmake is not None, "a cmake is available to run the guard script", "cmake not on PATH")
    check(os.path.isfile(GUARD), "the cb3 guard script exists", GUARD)
    if cmake is None or not os.path.isfile(GUARD):
        print(f"image_guard: {failures} failure(s)")
        return 1

    check(MAX_BYTES == 491504, f"the derived bound is the documented 491504 (0x{IMAGE_BOUND:X}-0x{APP_ORIGIN:X})",
          f"got {MAX_BYTES}")

    tmp = tempfile.mkdtemp(prefix="img_guard_")
    try:
        def make_bin(name, size):
            path = os.path.join(tmp, name)
            with open(path, "wb") as f:
                f.write(b"\x00" * size)
            return path

        cases = [
            ("at the bound", MAX_BYTES, 0),       # LESS_EQUAL: passes
            ("one under", MAX_BYTES - 1, 0),      # passes
            ("one over", MAX_BYTES + 1, 1),       # FATAL_ERROR
        ]
        for desc, size, want_rc in cases:
            path = make_bin(f"{desc.replace(' ', '_')}.bin", size)
            r = subprocess.run(
                [cmake, "-D", f"MAX_BYTES={MAX_BYTES}", "-D", f"BIN={path}", "-P", GUARD],
                capture_output=True, text=True)
            ok = (r.returncode == 0) if want_rc == 0 else (r.returncode != 0)
            check(ok, f"a {size}-byte image ({desc}) {'passes' if want_rc == 0 else 'is refused'}",
                  f"rc={r.returncode}; stderr={r.stderr.strip()[:200]}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if failures:
        print(f"image_guard: {failures} failure(s)")
        return 1
    print(f"image_guard: the guard passes at {MAX_BYTES}, refuses at {MAX_BYTES + 1}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
