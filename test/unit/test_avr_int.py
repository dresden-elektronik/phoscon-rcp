#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""test_avr_int.py - the portable code must not depend on a 32-bit int.

The ATmega256RFR2 (cbrb1, docs/hardware_avr.md §11) runs the same rcp/ core, app/ loop, com/uart.c
adapter and hal/dbg.c as the ARM boards, but avr-gcc's int and pointers are 16 bits. A shift of a
promoted byte by 16, a literal above 32767 in an int, or a 32-bit value narrowed into an int is
harmless on the Cortex-M parts and wrong on the AVR - and nothing on the host would show it, because
the host suites run with a 32-bit int.

The host clang carries the AVR target's type model in its front end even without an AVR back end, so
`-fsyntax-only --target=avr` type-checks with the AVR's widths. Every portable source is checked twice
with the same strict warnings, once for the AVR and once for the Cortex-M0+; a warning only the AVR
pass gives is an int-width bug. A canary with five such bugs proves the gate is not blind before its
clean result is trusted. A clang without the AVR target skips the suite (exit 77).

Clang warns about a shift only when its count is a constant, so `1u << channel` type-checks cleanly and
is 0 on the AVR from channel 16 up. The suite therefore also refuses a literal 1 of type int or unsigned
shifted by a count that is not a number: write `1uL << n`.

The class this cannot see - two 16-bit operands multiplied in int and widened afterwards - is read by
hand (docs/testing.md §2). Run by ctest as `avr_int`.
"""
import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CLANG = os.environ.get("CLANG", "clang")
SKIP = 77

AVR = ["--target=avr", "-mmcu=atmega256rfr2"]
ARM = ["--target=thumbv6m-none-eabi"]
WARN = ["-std=c99", "-ffreestanding", "-Wall", "-Wextra", "-Wconversion", "-Wshift-count-overflow",
        "-Wconstant-conversion", "-Wtautological-constant-out-of-range-compare", "-Wno-sign-conversion"]

# The sources every board compiles above its HAL. A board's own directories are not in it: each
# silicon has its own HAL (docs/architecture.md §2) and is checked by its own compiler.
PORTABLE = [
    "src/rcp/hdlc.c", "src/rcp/mac.c", "src/rcp/props.c", "src/rcp/props_debug.c",
    "src/rcp/props_stream.c", "src/rcp/scan.c", "src/rcp/session.c", "src/rcp/spinel.c",
    "src/rcp/srcmatch.c",
    "src/lib/u_bstream/u_bstream.c", "src/lib/u_sstream/u_sstream.c",
    "src/app/main.c", "src/app/main_rcp.c", "src/app/main_probe.c", "src/app/flasher_trap.c",
    "src/app/rcp_app.c", "src/app/mon_proto.c", "src/app/monitor_app.c", "src/app/loop_diag.c",
    "src/com/uart.c", "src/hal/dbg.c", "src/hal/uart_baud.c",
    # The AVR layer's portable arithmetic: host-tested, and compiled for the AVR on the board.
    "src/hal/avr/avr_ident.c", "src/hal/avr/avr_baud.c",
    # What a Thread image adds on top of them.
    "src/rcp/mac_sec.c",
]
THREAD = ["-DRCP_THREAD=1", "-DRCP_TX_TIMING=1", "-DRCP_CSL_ACCURACY_PPM=40", "-DRCP_CSL_UNCERTAINTY_10US=20"]
DEFINES = {"src/app/loop_diag.c": ["-DDS_LOOP_DIAG=1"], "src/rcp/mac_sec.c": THREAD}
# The include set the cbrb1 board compiles these with.
# An image's build writes this header (cmake/version.cmake); this check stands in for it.
VERSION_DIR = tempfile.mkdtemp(prefix="avr_int_")
with open(os.path.join(VERSION_DIR, "phoscon_version.h"), "w") as f:
    f.write('#define PHOSCON_RCP_VERSION "0.0.0"\n')

INCLUDES = ["src/app", "src/rcp", "src/board", "src/hal", "src/usb", "src/com", "src/lib/aes", "src/lib/u_bstream",
            "src/lib/u_sstream", "src/hal/avr", "src/targets/cbrb1"]

CANARY = """#include <stdint.h>
uint32_t a(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24; }
int b(void) { int v = 40000; return v; }
uint32_t d(void) { return 1 << 20; }
int e(uint32_t big) { int n = big; return n; }
uint32_t f(uint8_t n) { return 1u << n; }
"""
CANARY_MIN = 5

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
INT_SHIFT = re.compile(r"(?<![\w.])1[uU]?\s*<<\s*[A-Za-z_(]")


def warnings(target, path, extra=()):
    """The diagnostics clang prints for one file, as (file:line, message) pairs."""
    cmd = [CLANG] + target + WARN + list(extra) + ["-I" + VERSION_DIR] + ["-I" + os.path.join(REPO, i) for i in INCLUDES]
    r = subprocess.run(cmd + ["-fsyntax-only", path], capture_output=True, text=True, cwd=REPO)
    out = []
    for line in r.stderr.splitlines():
        if ": warning: " in line or ": error: " in line:
            where, _, msg = line.partition(": warning: ") if ": warning: " in line else line.partition(": error: ")
            out.append((where, msg))
    return r.returncode, out


def int_shifts(text):
    """The source lines shifting a literal 1 of int width by a variable count, as (line, text) pairs."""
    # A comment is blanked with its newlines kept, so the line numbers stay the file's.
    code = COMMENT.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)
    return [(n, line.strip()) for n, line in enumerate(code.splitlines(), 1) if INT_SHIFT.search(line)]


def avr_model_available():
    probe = "_Static_assert(sizeof(int) == 2, \"int\"); _Static_assert(sizeof(void *) == 2, \"ptr\");\n"
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as f:
        f.write(probe)
    try:
        r = subprocess.run([CLANG] + AVR + ["-ffreestanding", "-fsyntax-only", f.name],
                           capture_output=True, text=True)
    except FileNotFoundError:
        return False
    finally:
        os.unlink(f.name)
    return r.returncode == 0


def main() -> int:
    if not avr_model_available():
        print(f"avr_int: SKIP - {CLANG} has no AVR target")
        return SKIP

    failures = 0
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as f:
        f.write(CANARY)
    try:
        _, avr = warnings(AVR, f.name)
        _, arm = warnings(ARM, f.name)
    finally:
        os.unlink(f.name)
    avr_only = [w for w in avr if w not in arm]
    ok = len(avr_only) >= CANARY_MIN
    print(("  PASS  " if ok else "* FAIL* ") +
          f"canary: {len(avr_only)} AVR-only warnings (want >= {CANARY_MIN}), {len(arm)} on ARM")
    if not ok:
        failures += 1
    ok = len(int_shifts(CANARY)) == 1
    print(("  PASS  " if ok else "* FAIL* ") + "canary: the variable-count shift is seen")
    if not ok:
        failures += 1

    for rel in PORTABLE:
        path = os.path.join(REPO, rel)
        if not os.path.isfile(path):
            print(f"* FAIL* {rel}: listed but missing")
            failures += 1
            continue
        extra = DEFINES.get(rel, [])
        rc_avr, avr = warnings(AVR, path, extra)
        rc_arm, arm = warnings(ARM, path, extra)
        avr_only = [w for w in avr if w not in arm]
        with open(path, encoding="utf-8") as src:
            shifts = int_shifts(src.read())
        ok = rc_avr == 0 and rc_arm == 0 and not avr_only and not shifts
        print(("  PASS  " if ok else "* FAIL* ") + f"{rel}: " +
              ("clean" if ok else f"rc avr={rc_avr} arm={rc_arm}, {len(avr_only)} AVR-only, "
                                  f"{len(shifts)} int-width shift(s)"))
        for where, msg in avr_only:
            print(f"          {where}: {msg}")
        for line, text in shifts:
            print(f"          {rel}:{line}: {text}")
        if not ok:
            failures += 1

    if failures:
        print(f"avr_int: {failures} failure(s)")
        return 1
    print("avr_int: the portable code type-checks the same with a 16-bit int")
    return 0


if __name__ == "__main__":
    sys.exit(main())
