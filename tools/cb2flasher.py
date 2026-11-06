#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""cb2flasher.py - the GCFFlasher binary the flash wrappers run.

GCFFlasher must be 4.12.6 or newer: builds before 4.12.2 list no devices and report a failed
bootloader query, which looks exactly like a firmware fault, and earlier builds can freeze when their
output is a pipe, as it is here (docs/build-and-flash.md §1). Order:
$CB2_GCFFLASHER, then GCFFlasher on PATH, then a clear failure.
"""
import os
import shutil
import sys


def resolve():
    if "-h" in sys.argv or "--help" in sys.argv:
        print(sys.modules["__main__"].__doc__)
        raise SystemExit(0)
    env = os.environ.get("CB2_GCFFLASHER")
    if env:
        return env
    path = shutil.which("GCFFlasher")
    if path:
        return path
    raise SystemExit("no GCFFlasher: put a build of 4.12.6 or newer on PATH or set CB2_GCFFLASHER")
