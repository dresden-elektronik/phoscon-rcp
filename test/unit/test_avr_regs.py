#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""test_avr_regs.py - the ATmega256RFR2 register map against avr-libc, and the vector table's shape.

src/hal/avr/avr_regs.h is this tree's own transcription (docs/hardware_avr.md §11). A wrong address
names a different register and a wrong bit number writes a neighbouring field, and neither shows in an
arithmetic suite, so the transcription is compared with avr-libc's iom256rfr2.h, which avr-gcc carries.

The comparison is by name: every AVR_<name> the header defines, other than this tree's own AVR_OWN_*
and the C helpers, must equal avr-libc's <name>. Both headers are read by the assembler: in assembly
avr-libc's register macros expand to plain data-space addresses, and the transcription is written to be
included there too, so one generated file of .if/.error pairs does the whole comparison and an unknown
name fails as loudly as a wrong value. A canary with one wrong address proves the gate can fail.

The vector table in src/startup/startup_avr.S is checked against the same header: one jmp per slot,
slot n jumping to __vector_n, as many slots as avr-libc's _VECTORS_SIZE says the part has.

Needs avr-gcc (docs/build-and-flash.md §11); without it the suite is skipped (exit 77). Run by ctest as
`avr_regs`.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HEADER = os.path.join(REPO, "src", "hal", "avr", "avr_regs.h")
STARTUP = os.path.join(REPO, "src", "startup", "startup_avr.S")
SKIP = 77

HINTS = ["/opt/homebrew/opt/avr-gcc@14/bin", "/usr/local/opt/avr-gcc@14/bin"]
NOT_COMPARED = {"AVR_REGS_H", "AVR_REG8", "AVR_BIT", "AVR_ISR"}


def find_gcc():
    gcc = os.environ.get("AVR_GCC") or shutil.which("avr-gcc")
    if gcc:
        return gcc
    for d in HINTS:
        p = os.path.join(d, "avr-gcc")
        if os.access(p, os.X_OK):
            return p
    return None


def names(text):
    out = []
    for m in re.finditer(r"^#define\s+(AVR_\w+)\s+\S", text, re.M):
        name = m.group(1)
        if name in NOT_COMPARED or name.startswith("AVR_OWN_"):
            continue
        out.append(name)
    return out


def assemble(gcc, body, tmp):
    src = os.path.join(tmp, "check.S")
    with open(src, "w") as f:
        f.write(body)
    r = subprocess.run([gcc, "-mmcu=atmega256rfr2", "-c", "-x", "assembler-with-cpp",
                        "-I", os.path.dirname(HEADER), src, "-o", os.path.join(tmp, "check.o")],
                       capture_output=True, text=True)
    return r.returncode, r.stderr


def compare_body(pairs):
    lines = ["#include <avr/io.h>", '#include "avr_regs.h"']
    for ours, theirs in pairs:
        lines += [".if (%s) != (%s)" % (ours, theirs), '.error "%s differs from avr-libc %s"' % (ours, theirs),
                  ".endif"]
    return "\n".join(lines) + "\n"


def vectors_ok(slots, failures):
    text = open(STARTUP).read()
    table = re.findall(r"^\s*vec\s+(\d+)\s*$", text, re.M)
    if not re.search(r"^\s*jmp\s+__init\s*$", text, re.M):
        failures.append("startup_avr.S: slot 0 does not jump to __init")
    want = [str(n) for n in range(1, slots)]
    if table != want:
        failures.append("startup_avr.S: vector slots %s, want 1..%d in order" %
                        (",".join(table[:4]) + ",...", slots - 1))


def main():
    gcc = find_gcc()
    if gcc is None:
        print("avr_regs: SKIP - no avr-gcc (docs/build-and-flash.md §11)")
        return SKIP

    ours = names(open(HEADER).read())
    pairs = [(n, n[len("AVR_"):]) for n in ours]
    failures = []

    with tempfile.TemporaryDirectory() as tmp:
        rc, err = assemble(gcc, compare_body([("0xC5", "UDR0")]), tmp)
        if rc == 0:
            print("avr_regs: FAIL - the canary (0xC5 against UDR0) passed: the gate is blind")
            return 1

        rc, err = assemble(gcc, compare_body(pairs), tmp)
        if rc != 0:
            failures += [l for l in err.splitlines() if "Error" in l or "error" in l]

        # The slot count, from avr-libc, and the table laid out against it.
        rc, err = assemble(gcc, "#include <avr/io.h>\n.if (_VECTORS_SIZE) != 308\n.error \"size\"\n.endif\n", tmp)
        if rc != 0:
            failures.append("avr-libc _VECTORS_SIZE is not 308: the part's table changed")

    vectors_ok(308 // 4, failures)

    for f in failures:
        print("avr_regs: " + f)
    if failures:
        print("avr_regs: FAIL - %d" % len(failures))
        return 1

    print("avr_regs: %d names match avr-libc, 77 vector slots in order: OK" % len(pairs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
