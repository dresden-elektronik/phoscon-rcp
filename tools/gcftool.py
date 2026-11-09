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
import sys


def resolve():
    env = os.environ.get("GCFTOOL")
    if env:
        return env
    path = shutil.which("GCFTool")
    if path:
        return path
    raise SystemExit("no GCFTool: put it on PATH or set GCFTOOL (docs/build-and-flash.md §1)")


def package(binfile, board):
    """Write <binfile>.GCF with the board's profile; the caller runs guard() next."""
    subprocess.run([resolve(), "-b", board, binfile], check=True)


def guard(gcf, binfile, board):
    """The mandatory pre-flash guard (tools/gcf_check.py), for the board, against the .bin the container
    was packed from; binfile None means a released container, said explicitly. A refusal stops here."""
    tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gcf_check.py")
    argv = [sys.executable, tool, gcf, binfile if binfile else "--released", "--board", board]
    if subprocess.run(argv).returncode != 0:
        raise SystemExit("guard refused the container; nothing was flashed")


def build_package_guard(builddir, board):
    """Build, package and guard the image of a build directory, in that order: a build that fails stops
    here, so a container is never made from the .bin an earlier build left behind. The GCF's path."""
    if subprocess.run(["cmake", "--build", builddir]).returncode != 0:
        raise SystemExit(f"the build in {builddir} failed; nothing was packaged or flashed")
    binfile = os.path.join(builddir, "phoscon-rcp.bin")
    package(binfile, board)
    guard(binfile + ".GCF", binfile, board)
    return binfile + ".GCF"
