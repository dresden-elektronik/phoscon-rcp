#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""Check that every section reference in the doc set points at a section that exists.

The docs cite each other by frozen section number ("docs/hardware_mg21.md §10.7", "§9.1"). A number that no
longer names a section is invisible until someone follows the citation and lands somewhere else, so
this lints the ways that breaks.

  ERROR  a reference that names its document: `docs/x.md §N`, `README.md §N`, `AGENTS.md §N`, or
         "docs/x.md section N". The document has to exist and to have a section numbered N. These are
         the references a reader follows blindly.
  WARN   a bare `§N` that neither the current document nor any document named on the same line can
         resolve. Lines citing a datasheet, a standard or a table are skipped: those numbers belong to
         another corpus. `--strict` makes warnings fatal.

  ERROR  in code: a comment in a tracked file under src/, tools/, test/ or cmake/ that names a
         document and a section (`docs/x.md §N`) the doc set does not have. Source comments are
         where most citations live, and nothing else notices when one goes stale.

Usage: docrefs.py [--strict] [paths...]
       (default: README.md, AGENTS.md, CONTRIBUTING.md, docs/*.md, and the code citations)
Exit 0 when nothing is ERROR (and nothing WARN with --strict).
"""
import glob
import os
import re
import subprocess
import sys

HEADING = re.compile(r"^#{2,5}\s+([0-9]+(?:\.[0-9]+)*)[.\s]")
DOC = r"(?:docs/[a-z0-9_\-]+\.md|README\.md|AGENTS\.md|CONTRIBUTING\.md)"
CROSS = re.compile("(" + DOC + ")`?\\s*(?:§|section\\s+)([0-9]+(?:\\.[0-9]+)*)")
ON_DOC_LINE = re.compile(DOC + "`?\\s*§")
LOCAL = re.compile(r"(?<![\w.])§([0-9]+(?:\.[0-9]+)*)")
EXTERNAL = re.compile(r"datasheet|IEEE|802\.15\.4|errata|Table\s+\d", re.I)
DOC_IN_HEADING = re.compile("`(" + DOC + ")`")
ANY_DOC = re.compile(DOC)


def sections(path):
    out = set()
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = HEADING.match(line)
            if m:
                out.add(m.group(1))
    return out


CODE_DIRS = ("src", "tools", "test", "cmake")


def check_code(heads):
    """Explicit `docs/x.md §N` citations in tracked source files; returns the error count."""
    try:
        files = subprocess.run(["git", "ls-files", "--", *CODE_DIRS], capture_output=True,
                               text=True, check=True).stdout.split()
    except (OSError, subprocess.CalledProcessError):
        return 0
    errors = 0
    for path in files:
        try:
            with open(path, encoding="utf-8") as f:
                lines = f.readlines()
        except (OSError, UnicodeDecodeError):
            continue
        for number, line in enumerate(lines, 1):
            for target, ref in CROSS.findall(line):
                key = os.path.normpath(target)
                if key not in heads:
                    print(f"{path}:{number}: {target} §{ref} - no such document")
                    errors += 1
                elif ref not in heads[key]:
                    print(f"{path}:{number}: {target} §{ref} - {target} has no section {ref}")
                    errors += 1
    return errors


def main() -> int:
    strict = "--strict" in sys.argv
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    paths = args or sorted(glob.glob("docs/*.md")) + ["README.md", "AGENTS.md", "CONTRIBUTING.md"]
    paths = [p for p in paths if os.path.isfile(p)]
    heads = {os.path.normpath(p): sections(p) for p in paths}

    errors = 0
    warns = 0
    for path in sorted(heads):
        # A heading can name the document its bare "§N" belongs to - AGENTS.md does that for every
        # rule ("Hard constraints (full text: docs/coding-rules.md)"). Until the next heading, bare
        # numbers in that part of the file resolve against the document the heading named.
        scope = path
        with open(path, encoding="utf-8") as f:
            for number, line in enumerate(f, 1):
                if HEADING.match(line) or re.match(r"^#{2,5}\s", line):
                    named = DOC_IN_HEADING.search(line)
                    scope = os.path.normpath(named.group(1)) if named else path

                for target, ref in CROSS.findall(line):
                    key = os.path.normpath(target)
                    if key not in heads:
                        print(f"{path}:{number}: {target} §{ref} - no such document")
                        errors += 1
                    elif ref not in heads[key]:
                        print(f"{path}:{number}: {target} §{ref} - {target} has no section {ref}")
                        errors += 1

                if EXTERNAL.search(line) or ON_DOC_LINE.search(line):
                    continue
                for ref in LOCAL.findall(line):
                    if ref in heads.get(scope, ()):
                        continue
                    named = [os.path.normpath(d) for d in ANY_DOC.findall(line)]
                    if any(ref in heads.get(d, ()) for d in named):
                        continue
                    print(f"{path}:{number}: §{ref} - no section {ref} in "
                          f"{scope if scope != path else path}")
                    warns += 1

    if not args:
        errors += check_code(heads)

    if errors + warns:
        print(f"docrefs: {errors} error(s), {warns} warning(s)")
        return 1 if errors or strict else 0
    print(f"docrefs: OK - {len(heads)} documents, every section reference resolves")
    return 0


if __name__ == "__main__":
    sys.exit(main())
