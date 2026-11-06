#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""gcftool.py - the GCFTool binary that packages an image as a GCF container.

GCFTool comes from dresden elektronik, separately from this tree, and needs the board keys, which
are handed out on request (docs/build-and-flash.md §1, §4). Order: $GCFTOOL, then GCFTool on PATH,
then a clear failure.
"""
import os
import shutil
import subprocess


def resolve():
    env = os.environ.get("GCFTOOL")
    if env:
        return env
    path = shutil.which("GCFTool")
    if path:
        return path
    raise SystemExit("no GCFTool: put it on PATH or set GCFTOOL (docs/build-and-flash.md §1)")


def package(binfile, board):
    """Write <binfile>.GCF with the board's profile; the caller runs tools/gcf_check.py next."""
    subprocess.run([resolve(), "-b", board, binfile], check=True)
