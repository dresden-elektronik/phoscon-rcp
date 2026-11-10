#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""fetch_esp_phy.py - fetch Espressif's PHY libraries for an ESP32 board's radio.

The RF bring-up and calibration of Espressif's 802.15.4 parts exist only as the prebuilt libphy.a and
libbtbb.a of esp-phy-lib, which reach routines in the part's mask ROM by address (docs/hardware_esp32.md
§12.5). None of it is part of this repository: configure calls this tool, which downloads a pinned set
of files, checks every one against a SHA-256 recorded below, and keeps them in a cache outside the tree.
A second run finds the cache intact and does not touch the network.

The set is the one ESP-IDF v5.4.1 builds with, so the archives and the ROM address lists agree: the two
archives from the esp-phy-lib commit that tag pins as its submodule, esp-phy-lib's licence, and from the
tag itself the ROM's PHY address list, the ROM's main list (two symbols of it are taken), the chip's
default PHY init table and ESP-IDF's licence. Both are Apache-2.0.

From the fetched files the tool writes, in the cache and nowhere else:
  generated/<chip>_phy_init_data.inc   the init table's 128 bytes, ESP-IDF's expression evaluated with
                                       CONFIG_ESP_PHY_MAX_TX_POWER = 20, its default
  generated/<chip>.rom.extra.ld        ets_update_cpu_frequency and ets_delay_us from the ROM's list

Usage:
    fetch_esp_phy.py [--cache DIR] [--check]

Prints the directory holding the tree as the last line of stdout (`ESP_PHY_ROOT=<dir>`). --check
verifies the cache and never downloads; it exits 1 when a file is missing or wrong. Exit codes: 0
ready, 1 --check found the cache incomplete, 2 a download or a hash failed.
"""
import argparse
import hashlib
import os
import re
import sys
import tempfile
import urllib.error
import urllib.request

CHIP = "esp32c6"
PHY_LIB_COMMIT = "aa6bac1e0a83ea70fa3f03a72041efddaeb7b898"   # ESP-IDF v5.4.1's components/esp_phy/lib
IDF_TAG = "v5.4.1"
PHY_URL = "https://raw.githubusercontent.com/espressif/esp-phy-lib/%s/%%s" % PHY_LIB_COMMIT
IDF_URL = "https://raw.githubusercontent.com/espressif/esp-idf/%s/%%s" % IDF_TAG
TIMEOUT_S = 120
MAX_TX_POWER_DBM = 20

# (where it lands in the cache, URL template, path, SHA-256, size)
FILES = (
    ("esp-phy-lib/esp32c6/libphy.a", PHY_URL, "esp32c6/libphy.a",
     "c8e926c5511adc0fe0179f10e378c78cb5cfd3f77ed679428683c150ace6a4a8", 178304),
    ("esp-phy-lib/esp32c6/libbtbb.a", PHY_URL, "esp32c6/libbtbb.a",
     "b06c61caa701a0c551eef376b4e3575e65993c9b3bbafddb481d4e1d5c24b847", 17412),
    ("esp-phy-lib/LICENSE", PHY_URL, "LICENSE",
     "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30", 11358),
    ("esp-idf/LICENSE", IDF_URL, "LICENSE",
     "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30", 11358),
    ("esp-idf/esp32c6.rom.phy.ld", IDF_URL, "components/esp_rom/esp32c6/ld/esp32c6.rom.phy.ld",
     "7332fd77b2aeb34d5d9881e86e26a7521f2b315b3ad4116082f92eb1b6a19f35", 7435),
    ("esp-idf/esp32c6.rom.ld", IDF_URL, "components/esp_rom/esp32c6/ld/esp32c6.rom.ld",
     "eb2b0865162092344cc2224b8a3c4ea75134f07e6b034267c6ea44ad9ef89fb1", 13590),
    ("esp-idf/phy_init_data.c", IDF_URL, "components/esp_phy/esp32c6/phy_init_data.c",
     "44f329eb42acade10a012ac0b15fea755c9d95111550f0dce4798b34383677c3", 5737),
)

# Of the ROM's main list, only what the PHY glue calls (hal/esp32/phy/esp32_phy.c) and the archive needs.
ROM_EXTRA = ("ets_update_cpu_frequency", "ets_delay_us")
INIT_DATA_LEN = 128
AR_MAGIC = b"!<arch>\n"


def default_cache():
    base = os.environ.get("XDG_CACHE_HOME") or os.path.join(os.path.expanduser("~"), ".cache")
    return os.path.join(base, "phoscon-rcp", "esp-phy")


def sha256_of(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def intact(path, digest, size):
    return os.path.isfile(path) and os.path.getsize(path) == size and sha256_of(path) == digest


def write_atomic(target, data):
    os.makedirs(os.path.dirname(target), exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(target))
    with os.fdopen(fd, "wb") as f:
        f.write(data)
    os.replace(tmp, target)


def fetch_one(root, local, url, path, digest, size):
    request = urllib.request.Request(url % path, headers={"User-Agent": "phoscon-rcp-fetch-esp-phy/1"})
    data = urllib.request.urlopen(request, timeout=TIMEOUT_S).read()
    if hashlib.sha256(data).hexdigest() != digest or len(data) != size:
        raise RuntimeError("%s: downloaded %d bytes, SHA-256 %s, expected %d bytes, %s"
                           % (path, len(data), hashlib.sha256(data).hexdigest(), size, digest))
    if local.endswith(".a") and not data.startswith(AR_MAGIC):
        raise RuntimeError("%s: not an ar archive" % path)
    write_atomic(os.path.join(root, local), data)


def limit(val, low, high):
    return low if val < low else high if val > high else val


def init_table(source):
    """ESP-IDF's initializer of phy_init_data, every element evaluated: numbers, and LIMIT() of the
    maximum TX power, as its header defines it."""
    start = source.index("phy_init_data= { {") + len("phy_init_data= { {")
    body = source[start:source.index("} };", start)]
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    body = re.sub(r"//[^\n]*", "", body)
    items = [x.strip() for x in re.split(r",(?![^(]*\))", body) if x.strip()]
    values = []
    for item in items:
        expr = item.replace("CONFIG_ESP_PHY_MAX_TX_POWER", str(MAX_TX_POWER_DBM)).replace("LIMIT", "limit")
        if re.search(r"[A-Za-z_](?<!limit)\w*", re.sub(r"\blimit\b|0x[0-9A-Fa-f]+", "", expr)):
            raise RuntimeError("phy_init_data.c: an element this tool cannot evaluate: %s" % item)
        values.append(int(eval(expr, {"__builtins__": {}, "limit": limit})))
    if len(values) != INIT_DATA_LEN or not all(0 <= v <= 255 for v in values):
        raise RuntimeError("phy_init_data.c: %d elements, not %d bytes" % (len(values), INIT_DATA_LEN))
    return values


def generate(root):
    with open(os.path.join(root, "esp-idf", "phy_init_data.c")) as f:
        values = init_table(f.read())
    lines = ["/* Generated by tools/fetch_esp_phy.py from ESP-IDF %s components/esp_phy/%s/phy_init_data.c"
             % (IDF_TAG, CHIP),
             " * (Apache-2.0), CONFIG_ESP_PHY_MAX_TX_POWER = %d. Not part of the repository. */" % MAX_TX_POWER_DBM]
    for i in range(0, INIT_DATA_LEN, 16):
        lines.append("    " + ", ".join("0x%02Xu" % v for v in values[i:i + 16]) + ",")
    write_atomic(os.path.join(root, "generated", "%s_phy_init_data.inc" % CHIP), ("\n".join(lines) + "\n").encode())

    with open(os.path.join(root, "esp-idf", "%s.rom.ld" % CHIP)) as f:
        rom = f.read()
    out = ["/* Generated by tools/fetch_esp_phy.py from ESP-IDF %s components/esp_rom/%s/ld/%s.rom.ld"
           % (IDF_TAG, CHIP, CHIP), " * (Apache-2.0): the ROM routines the PHY glue and the archives call. */"]
    for name in ROM_EXTRA:
        m = re.search(r"^\s*%s\s*=\s*(0x[0-9A-Fa-f]+)\s*;" % name, rom, re.M)
        if not m:
            raise RuntimeError("%s.rom.ld: no %s" % (CHIP, name))
        out.append("%s = %s;" % (name, m.group(1)))
    write_atomic(os.path.join(root, "generated", "%s.rom.extra.ld" % CHIP), ("\n".join(out) + "\n").encode())


def main():
    ap = argparse.ArgumentParser(description="fetch the pinned ESP32 PHY library files")
    ap.add_argument("--cache", default=default_cache(), help="cache directory (default %(default)s)")
    ap.add_argument("--check", action="store_true", help="verify the cache, never download")
    args = ap.parse_args()

    root = os.path.join(os.path.abspath(os.path.expanduser(args.cache)), PHY_LIB_COMMIT[:12])
    missing = [f for f in FILES if not intact(os.path.join(root, f[0]), f[3], f[4])]

    if missing and args.check:
        for f in missing:
            print("missing or wrong: %s" % f[0], file=sys.stderr)
        return 1

    for local, url, path, digest, size in missing:
        print("fetch_esp_phy: %s (%d bytes)" % (path, size), file=sys.stderr)
        try:
            fetch_one(root, local, url, path, digest, size)
        except (urllib.error.URLError, OSError, RuntimeError) as err:
            print("fetch_esp_phy: cannot fetch %s: %s\n"
                  "fetch_esp_phy: an ESP32 radio needs these files once; connect to the network, or put them\n"
                  "fetch_esp_phy: into %s by hand (docs/build-and-flash.md section 13)." % (path, err, root),
                  file=sys.stderr)
            return 2

    try:
        generate(root)
    except (OSError, RuntimeError, ValueError, SyntaxError) as err:
        print("fetch_esp_phy: %s" % err, file=sys.stderr)
        return 2

    print("ESP_PHY_ROOT=%s" % root)
    return 0


if __name__ == "__main__":
    sys.exit(main())
