#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""test_flash_paths.py - every tracked tool that flashes a board runs the pre-flash guard for that board.

A flash path is a tool that runs GCFFlasher (`cb2flasher.resolve()`, a `--flasher` path) or uploads
pages itself. Each must reach tools/gcf_check.py with the board named, through gcftool.guard(),
gcftool.build_package_guard(), or a gcf_check call with `--board` (docs/build-and-flash.md §6): the
guard's default board is cb2, and a path that leans on it checks another board's container against
the wrong rules. Run by ctest as `flash_paths`.
"""
import glob
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FLASHES = (r"cb2flasher\.resolve\(", r"args\.flasher\b", r"\bdef upload\(")
GUARDS = (r"gcftool\.guard\(\s*[^)]*,\s*\"(cb2|rb2|cb3|cbrb1)\"", r"gcftool\.build_package_guard\(",
          r"gcf_check\.py[\s\S]*\"--board\"")
NOT_PATHS = {"cb2flasher.py", "gcftool.py", "gcf_check.py"}


def main() -> int:
    failures = 0
    paths = 0
    for path in sorted(glob.glob(os.path.join(REPO, "tools", "*.py"))):
        name = os.path.basename(path)
        if name in NOT_PATHS:
            continue
        with open(path, encoding="utf-8") as f:
            text = f.read()
        if not any(re.search(p, text) for p in FLASHES):
            continue
        paths += 1
        guarded = any(re.search(p, text) for p in GUARDS)
        print(("  PASS  " if guarded else "* FAIL* ") + f"tools/{name}"
              + ("" if guarded else ": flashes without the guard for a named board"))
        failures += 0 if guarded else 1
    if paths == 0:
        print("flash_paths: no flash path found - the patterns no longer match the tools")
        return 1
    print(f"flash_paths: {paths} flash path(s), {failures} without the guard")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
