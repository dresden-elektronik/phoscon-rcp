#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""fetch_rail.py - fetch the Silicon Labs RAIL library for the ConBee III `rail` radio back-end.

The `rail` back-end (the default of `-DDEBOARD=cb3 -DDS_MODE=rcp`, docs/build-and-flash.md section 11) links
Silicon Labs' prebuilt RAIL library for the EFR32xG21. The library and its headers are not part of
this repository: configure calls this tool, which downloads a pinned set of files from one Simplicity
SDK tag, checks every one against a SHA-256 recorded below, and keeps them in a cache outside the
tree. A second run finds the cache intact and does not touch the network.

Where the files come from, and why not the whole SDK or one URL:

  - the headers are ordinary files in the SiliconLabs/simplicity_sdk repository, served by
    raw.githubusercontent.com at the tag;
  - the archive is a Git LFS object in that repository, and GitHub's own LFS store answers 404 for it
    (the SDK's .lfsconfig points elsewhere): the object is content-addressed by its SHA-256, so it is
    fetched from the LFS server the repository names, through the standard LFS batch API, and checked
    against the same hash.

Usage:
    fetch_rail.py [--cache DIR] [--check]

Prints the directory holding the fetched tree as the last line of stdout (`RAIL_ROOT=<dir>`).
--check verifies the cache and never downloads; it exits 1 when a file is missing or wrong.
Exit codes: 0 ready, 1 --check found the cache incomplete, 2 a download or a hash failed.
"""
import argparse
import hashlib
import json
import os
import sys
import tempfile
import urllib.error
import urllib.request

SDK_REPO = "SiliconLabs/simplicity_sdk"
SDK_TAG = "v2025.6.3"
RAW_URL = "https://raw.githubusercontent.com/%s/%s/%%s" % (SDK_REPO, SDK_TAG)
# .lfsconfig of the tag: where the repository's LFS objects live.
LFS_URL = "https://artifacts.silabs.net/artifactory/api/lfs/gsdk"
TIMEOUT_S = 120

RAIL = "platform/radio/rail_lib/"

# (path in the SDK, SHA-256, size, how it is served). Everything the rail adapter compiles against
# and links: the xG21 archive, the RAIL headers the adapter includes (rail.h and what it pulls in,
# the xG2x chip header, the 802.15.4 header), sl_status.h, and the SDK's licence text.
FILES = (
    ("License.txt",
     "e412cf76b7ccbc9459358aa97c2679bad065b5cf0483084a36e3bea9c83b85d7", 1982, "raw"),
    (RAIL + "common/rail.h",
     "dcbe6942527f8ba37361e6844c0ed2da17a95bbfa5f9ad78c18bc9710a78e891", 357253, "raw"),
    (RAIL + "common/rail_types.h",
     "c064bc0daff6016d556d3fe0d7ae469c4c7013ba8360c1546585bc972fb9035c", 259705, "raw"),
    (RAIL + "common/rail_assert_error_codes.h",
     "cce109c00ba88670cde095af389a96f0eee4e8cfe1fc5a6188533061d7ead52c", 30123, "raw"),
    (RAIL + "common/rail_features.h",
     "03172dc4f93e82eec8f6b53ea5742a633b2e7e19189b3abce9b1f7a64eb43ab1", 37708, "raw"),
    (RAIL + "chip/efr32/efr32xg2x/rail_chip_specific.h",
     "68bfa096db58a562b5a0861b735f1e3c77a2e6341513806e578b4e63a8c41687", 29846, "raw"),
    (RAIL + "protocol/ieee802154/rail_ieee802154.h",
     "8990b7ab00964bbc0226cf47215f9f84b1e7d987470e39c3f1b56febca1ebbd5", 82258, "raw"),
    ("platform/common/inc/sl_status.h",
     "3cfc83987b05f3f12aec3180bb1df82dee44bacbafe317aec62b7049b6bdf5bb", 65630, "raw"),
    (RAIL + "autogen/librail_release/librail_efr32xg21_gcc_release.a",
     "526878162077edd2d9eb60fe917ea1206335559f512c1949ca163014f116773a", 1518556, "lfs"),
)

AR_MAGIC = b"!<arch>\n"


def default_cache():
    base = os.environ.get("XDG_CACHE_HOME") or os.path.join(os.path.expanduser("~"), ".cache")
    return os.path.join(base, "phoscon-rcp", "rail")


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def intact(path, digest, size):
    return os.path.isfile(path) and os.path.getsize(path) == size and sha256_of(path) == digest


def http(request):
    # The artifact server answers 403 to urllib's default agent string.
    request.add_header("User-Agent", "phoscon-rcp-fetch-rail/1")
    return urllib.request.urlopen(request, timeout=TIMEOUT_S)


def download_raw(path):
    return http(urllib.request.Request(RAW_URL % path)).read()


def download_lfs(digest, size):
    body = json.dumps({"operation": "download", "transfers": ["basic"],
                       "objects": [{"oid": digest, "size": size}]}).encode()
    request = urllib.request.Request(
        LFS_URL + "/objects/batch", data=body,
        headers={"Accept": "application/vnd.git-lfs+json",
                 "Content-Type": "application/vnd.git-lfs+json"})
    answer = json.load(http(request))
    obj = answer["objects"][0]
    if "error" in obj:
        raise RuntimeError("the LFS server refuses the object: %s" % obj["error"])
    action = obj["actions"]["download"]
    return http(urllib.request.Request(action["href"], headers=action.get("header") or {})).read()


def fetch_one(root, path, digest, size, how):
    data = download_lfs(digest, size) if how == "lfs" else download_raw(path)
    if hashlib.sha256(data).hexdigest() != digest or len(data) != size:
        hint = " (an LFS pointer instead of the file?)" if data.startswith(b"version https://git-lfs") else ""
        raise RuntimeError("%s: downloaded %d bytes, SHA-256 %s, expected %d bytes, %s%s"
                           % (path, len(data), hashlib.sha256(data).hexdigest(), size, digest, hint))
    if path.endswith(".a") and not data.startswith(AR_MAGIC):
        raise RuntimeError("%s: not an ar archive" % path)
    target = os.path.join(root, path)
    os.makedirs(os.path.dirname(target), exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(target))
    with os.fdopen(fd, "wb") as f:
        f.write(data)
    os.replace(tmp, target)


def main():
    ap = argparse.ArgumentParser(description="fetch the pinned RAIL library files")
    ap.add_argument("--cache", default=default_cache(), help="cache directory (default %(default)s)")
    ap.add_argument("--check", action="store_true", help="verify the cache, never download")
    args = ap.parse_args()

    root = os.path.join(os.path.abspath(os.path.expanduser(args.cache)), SDK_TAG)
    missing = [f for f in FILES if not intact(os.path.join(root, f[0]), f[1], f[2])]

    if missing and args.check:
        for f in missing:
            print("missing or wrong: %s" % f[0], file=sys.stderr)
        return 1

    for path, digest, size, how in missing:
        print("fetch_rail: %s (%d bytes)" % (path, size), file=sys.stderr)
        try:
            fetch_one(root, path, digest, size, how)
        except (urllib.error.URLError, OSError, RuntimeError, KeyError, ValueError) as err:
            print("fetch_rail: cannot fetch %s: %s\n"
                  "fetch_rail: the rail back-end needs these files once; connect to the network, or fill\n"
                  "fetch_rail: %s with the tree of %s %s (docs/build-and-flash.md section 11)."
                  % (path, err, root, SDK_REPO, SDK_TAG), file=sys.stderr)
            return 2

    print("RAIL_ROOT=%s" % root)
    return 0


if __name__ == "__main__":
    sys.exit(main())
