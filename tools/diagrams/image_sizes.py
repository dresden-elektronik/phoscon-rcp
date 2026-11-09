#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""What each image weighs against its board's budget, owned by docs/build-and-flash.md section 3.

Two figures, the rcp images and the probe images, with one row per image, all to one byte scale, so a
row compares with every other in either figure: the bar is the image, in amber, and the hatched rest
of the row is the room left in what its board's guard allows, read from tools/gcf_check.py. A third
figure, to a scale of its own, is the RAM each rcp image allocates at link time against the RAM its
part has, read from the board's linker script; the hatched rest is where the stack grows down from the
top. The sizes are measured from a build of an export, where the version string is the release's, and
are updated in RCP, PROBE and RCP_RAM when they change.

    python3 tools/diagrams/image_sizes.py            # docs/images/image-sizes-{rcp,probe,ram}.png
    python3 tools/diagrams/image_sizes.py --check
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import figure  # noqa: E402
from figure import MX, S2, S4  # noqa: E402

W = 900
X0, X1 = 212, W - MX           # as in flash_maps.py: the labels left, the bars right
BAR, ROW = 30, 60

# one row per image: board, its name, .bin size in bytes, its build directory (a convention), and
# whether a release builds it
RCP = (
    ('cb2', 'ConBee II', 52232, 'build/rcp', True),
    ('rb2', 'RaspBee II', 47428, 'build/rb2-rcp', True),
    ('cb3', 'ConBee III', 62376, 'build/fw-cb3-rcp', True),
    ('cb3', 'ConBee III, own', 25120, 'build/fw-cb3-own', False),
    ('cbrb1', 'ConBee I, RaspBee I', 45112, 'build/fw-cbrb1-rcp', True),
)
PROBE = (
    ('cb2', 'ConBee II', 31688, 'build/fw', True),
    ('rb2', 'RaspBee II', 26880, 'build/rb2', True),
    ('cb3', 'ConBee III', 7456, 'build/fw-cb3', True),
    ('cbrb1', 'ConBee I, RaspBee I', 6854, 'build/fw-cbrb1', True),
)
# the same rows for RAM: every byte the linker places there - .data, .bss and .noinit, on cb3 also the
# bootloader's 32-byte reset record; on cbrb1 .data holds .rodata as well, which avr-gcc reads from RAM
RCP_RAM = (
    ('cb2', 'ConBee II', 9424, 'build/rcp', True),
    ('rb2', 'RaspBee II', 12272, 'build/rb2-rcp', True),
    ('cb3', 'ConBee III', 14272, 'build/fw-cb3-rcp', True),
    ('cb3', 'ConBee III, own', 13568, 'build/fw-cb3-own', False),
    ('cbrb1', 'ConBee I, RaspBee I', 12024, 'build/fw-cbrb1-rcp', True),
)


def budgets():
    """The room each board's guard allows, read from the guard itself."""
    with open(os.path.join(figure.ROOT, 'tools', 'gcf_check.py')) as f:
        src = f.read()

    def const(name):
        return int(re.search(r'^%s\s*=\s*(0x[0-9A-Fa-f]+|\d+)' % name, src, re.M).group(1), 0)
    samr21 = const('NVRAM_BASE') - const('APP_ORIGIN')
    return dict(cb2=samr21, rb2=samr21, cb3=const('CB3_APP_SECTOR_SIZE') - const('CB3_APP_INFO_SIZE'),
                cbrb1=const('AVR_BOOT_BASE'))


def rams():
    """The RAM each part has, read from the board's linker script: the regions the linker may place
    data in, and on cb3 the reset record below them."""
    def length(script, region):
        with open(os.path.join(figure.ROOT, 'src', 'targets', script)) as f:
            src = f.read()
        expr = re.search(r'^\s*%s\s*\([^)]*\)\s*:\s*ORIGIN\s*=\s*[^,]+,\s*LENGTH\s*=\s*(.+)$' % region,
                         src, re.M).group(1)
        terms = re.findall(r'([+-]?)\s*(0x[0-9A-Fa-f]+|\d+)', expr)
        return sum(int(n, 0) * (-1 if sign == '-' else 1) for sign, n in terms)
    samr21 = length('cb2/atsamr21.ld', 'RAM')
    if length('rb2/atsamr21.ld', 'RAM') != samr21:
        raise SystemExit('image_sizes: cb2 and rb2 no longer have the same RAM')
    return dict(cb2=samr21, rb2=samr21,
                cb3=length('cb3/efr32mg21.ld', 'RECORD') + length('cb3/efr32mg21.ld', 'RAM'),
                cbrb1=length('cbrb1/atmega256rfr2.ld', 'data'))


def spaced(n):
    return '{:,}'.format(n).replace(',', ' ') + ' B'


def rows(f, images, room, legend):
    scale = (X1 - X0) / float(max(room.values()))
    y = f.top + 22
    for board, name, size, directory, selected in images:
        if size >= room[board]:
            raise SystemExit('image_sizes: %s %s is over its budget' % (board, name))
        end = X0 + room[board] * scale
        width = max(size * scale, 3)
        f.text(MX, y + 13, name, 'f-lab')
        f.fits(directory, 'f-sub', X0 - MX - S2, 'the label column')
        f.text(MX, y + 27, directory, 'f-sub')
        f.line([(end, y - 4), (end, y)], 'f-tickm')
        f.text(end, y - 8, spaced(room[board]), 'f-bit', 'end')
        f.rect(X0, y, end - X0, BAR, 'f-keep', rx=0)
        if selected:
            f.rect(X0, y, width, BAR, 'f-amb', rx=0)
        else:
            f.rect(X0, y, width, BAR, 'f-blk', rx=0)
        # a label on a hatch sits on a white ground of its own
        label = spaced(size)
        f.rect(X0 + width + S2, y + 6, figure.textW(label, 'f-num') + 12, BAR - 12, 'f-blk', rx=1)
        f.text(X0 + width + S2 + 6, y + 20, label, 'f-num')
        y += ROW
    ly = f.legend(y, legend)
    f.h = ly + S4
    return f


def build_rcp():
    f = figure.Fig(W, 600, 'RCP IMAGE SIZES', 'each image against the room its board allows, all to one scale')
    return rows(f, RCP, budgets(), [('amb', 'the image'), ('blk', 'the image with the unselected back-end'),
                         ('keep', 'the room left')])


def build_probe():
    f = figure.Fig(W, 600, 'PROBE IMAGE SIZES', 'each image against the room its board allows, to the scale of the RCP figure')
    return rows(f, PROBE, budgets(), [('amb', 'the image'), ('keep', 'the room left')])


def build_ram():
    f = figure.Fig(W, 600, 'RCP IMAGE RAM', 'what each image allocates at link time against the RAM its part has, to a scale of its own')
    return rows(f, RCP_RAM, rams(), [('amb', 'allocated at link time'), ('blk', 'the same with the unselected back-end'),
                                     ('keep', 'the stack and the room left')])


if __name__ == '__main__':
    sys.exit(figure.main('image-sizes-rcp', build_rcp) | figure.main('image-sizes-probe', build_probe)
             | figure.main('image-sizes-ram', build_ram))
