#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""release_images.py - build, package and check the rcp image of every board for a release.

Writes dist/<version>/phoscon-rcp-<version>-<board>.GCF and SHA256SUMS. Every build is fresh, under
build/release/, and every container passes tools/gcf_check.py against its .bin. Packaging needs GCFTool
and the board keys (docs/build-and-flash.md §12).

    GCFTOOL=<keyed GCFTool> python3 tools/release_images.py              # HEAD must be tagged v<VERSION>
    GCFTOOL=<keyed GCFTool> python3 tools/release_images.py --snapshot   # any commit, to test a release

Nothing here flashes: each container still goes on a stick before the release is published.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys

import gcftool

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOARDS = (("cb2", "arm-none-eabi.cmake"), ("rb2", "arm-none-eabi.cmake"),
          ("cb3", "arm-none-eabi-m33.cmake"), ("cbrb1", "avr-gcc.cmake"))


def git(*args):
    return subprocess.run(["git", "-C", REPO, *args], check=True, capture_output=True,
                          text=True).stdout.strip()


def build(board, toolchain, version):
    """The container's path, after the build, the packaging and the guard."""
    out = os.path.join(REPO, "build", "release", board)
    shutil.rmtree(out, ignore_errors=True)
    subprocess.run(["cmake", "-B", out, "-S", os.path.join(REPO, "src"), "-G", "Ninja",
                    f"-DDEBOARD={board}", "-DDS_MODE=rcp",
                    f"-DCMAKE_TOOLCHAIN_FILE={os.path.join(REPO, 'cmake', toolchain)}"],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["cmake", "--build", out], check=True, stdout=subprocess.DEVNULL)
    binp = os.path.join(out, "phoscon-rcp.bin")
    with open(binp, "rb") as f:
        if f"phoscon-rcp {version}\0".encode() not in f.read():
            raise SystemExit(f"{board}: the image does not report {version}")
    gcftool.package(binp, board)
    gcf = binp + ".GCF"
    subprocess.run([sys.executable, os.path.join(REPO, "tools", "gcf_check.py"), gcf, binp,
                    "--board", board], check=True)
    return gcf


def main():
    ap = argparse.ArgumentParser(description="build, package and check every board's rcp image")
    ap.add_argument("--snapshot", action="store_true", help="any commit; the version names it")
    args = ap.parse_args()

    with open(os.path.join(REPO, "VERSION")) as f:
        version = f.read().strip()
    dirty = git("status", "--porcelain", "--untracked-files=no")
    tag = git("tag", "--points-at", "HEAD", "--list", f"v{version}")
    if args.snapshot:
        if dirty or not tag:
            version += f"+g{git('rev-parse', '--short=8', 'HEAD')}"
        if dirty:
            version += ".dirty"
    elif dirty or not tag:
        raise SystemExit(f"a release needs a clean tree at the tag v{version}; --snapshot tests one")

    dist = os.path.join(REPO, "dist", version)
    shutil.rmtree(dist, ignore_errors=True)
    os.makedirs(dist)
    sums = []
    for board, toolchain in BOARDS:
        name = f"phoscon-rcp-{version}-{board}.GCF"
        shutil.copyfile(build(board, toolchain, version), os.path.join(dist, name))
        with open(os.path.join(dist, name), "rb") as f:
            sums.append(f"{hashlib.sha256(f.read()).hexdigest()}  {name}\n")
    with open(os.path.join(dist, "SHA256SUMS"), "w") as f:
        f.writelines(sums)
    print(f"release_images: {len(sums)} containers in {dist}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
