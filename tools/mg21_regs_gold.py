#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""Regenerate the ConBee III register record that the mg21_regs suite checks against.

`test/data/mg21_regs_ref.h` is the record of one part's register layout: block bases, the offset
of every register inside blocks that are not dense, the field encodings this tree's driver
writes, the interrupt numbers, and the geometry. It is generated, never typed, because the only
reliable reading of a register map is to let a compiler say where the part's own typedef puts
each register — which is what `tools/mg21_regs_gold.c` does when it is compiled against a vendor
description of the same silicon.

    SDK=<simplicity_sdk_2024.12.1> python3 tools/mg21_regs_gold.py

Two tree shapes are accepted: a whole Simplicity SDK (`platform/Device/...`), or a flat copy of
its EFR32MG21 device headers (Silicon Labs, Zlib). Neither is part of this repository, so the
generated record — not the headers — is what the suite needs, and a fresh clone runs `mg21_regs`
without any reference tree present.

Regenerate after a part or SDK change and read the diff: that diff is the review.
"""

import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
GOLD_SRC = REPO / "tools" / "mg21_regs_gold.c"
OUT = REPO / "test" / "data" / "mg21_regs_ref.h"
PART_HEADER = "efr32mg21a010f1024im32.h"

# (device include dir, CMSIS core include dir, common include dir) relative to the SDK root.
SHAPES = [
    ("platform/Device/SiliconLabs/EFR32MG21/Include", "platform/CMSIS/Core/Include", "platform/common/inc"),
    ("Device/Include", "CMSIS/Core/Include", "common/inc"),
]


def find_includes(root: Path):
    for shape in SHAPES:
        dirs = [root / d for d in shape]
        if all(d.is_dir() for d in dirs) and (dirs[0] / PART_HEADER).is_file():
            return dirs
    return None


def main() -> int:
    root = os.environ.get("SDK")
    if not root:
        print("mg21_regs_gold: set SDK=<Simplicity SDK tree, or a flat copy of its MG21 device headers>", file=sys.stderr)
        return 2
    dirs = find_includes(Path(root))
    if dirs is None:
        print(f"mg21_regs_gold: {PART_HEADER} not found under {root}", file=sys.stderr)
        return 2

    cc = os.environ.get("CC", "cc")
    cmd = [cc, "-std=gnu99", "-O0"]
    for d in dirs:
        cmd += ["-I", str(d)]
    cmd += [str(GOLD_SRC), "-o", "/tmp/mg21_regs_gold"]

    build = subprocess.run(cmd, capture_output=True, text=True)
    if build.returncode != 0:
        print(build.stdout + build.stderr, file=sys.stderr)
        print(f"mg21_regs_gold: {cc} could not compile {GOLD_SRC.name} against {root}", file=sys.stderr)
        return 1

    run = subprocess.run(["/tmp/mg21_regs_gold"], capture_output=True, text=True)
    if run.returncode != 0:
        print(run.stdout + run.stderr, file=sys.stderr)
        print("mg21_regs_gold: the record program failed to run", file=sys.stderr)
        return 1

    OUT.write_text(run.stdout)
    entries = run.stdout.count("#define REF_")
    print(f"mg21_regs_gold: wrote {entries} entries to {OUT.relative_to(REPO)} from {root}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
