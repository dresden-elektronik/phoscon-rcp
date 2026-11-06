#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""token_equiv.py - prove that a change touched no code, only comments and whitespace.

Strips comments and all whitespace from each file and compares it with the same file in a git
revision: "the code did not change" becomes a checked claim instead of a review opinion. It is
the check to run after any comment/formatting pass written by hand, and it has already caught
two accidental deletions in this tree (a `#define` and a whole code line hidden behind a
trailing comment), which is why it stays.

    tools/token_equiv.py HEAD~1 src/rcp/*.c
    tools/token_equiv.py HEAD~1 $(git show --name-only --format= <commit>)

It says nothing about whether the *built* code is unchanged - a byte-identical image can only
be checked by building (and line numbers in the debug records move with any comment edit, so
for firmware a full rebuild plus the host suites and the two-node rig is the real gate).
"""

import os
import subprocess
import sys


def code(line, state):
    """The line with comments and string/char literals removed (state['block'] persists)."""
    out = []
    i, n = 0, len(line)
    while i < n:
        c = line[i]
        nxt = line[i + 1] if i + 1 < n else ""
        if state["block"]:
            if c == "*" and nxt == "/":
                state["block"] = False
                i += 2
                continue
            i += 1
            continue
        if c == "/" and nxt == "*":
            state["block"] = True
            i += 2
            continue
        if c == "/" and nxt == "/":
            break
        if c in "\"'":
            quote, i = c, i + 1
            while i < n:
                if line[i] == "\\":
                    i += 2
                    continue
                if line[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def canon(text):
    # A trailing backslash is a line continuation: whitespace, not a token.
    state = {"block": False}
    return "".join("".join(code(line.rstrip().removesuffix("\\"), state).split())
                   for line in text.splitlines())


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    rev, files = sys.argv[1], sys.argv[2:]
    bad = []
    for f in files:
        before = subprocess.run(["git", "show", f"{rev}:{f}"],
                                capture_output=True, text=True).stdout
        if canon(before) != canon(open(f).read()):
            bad.append(f)
    print(f"  {len(files)} file(s) compared against {rev}: {len(bad)} with a code change")
    for f in bad:
        print(f"    CHANGED CODE: {f}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
