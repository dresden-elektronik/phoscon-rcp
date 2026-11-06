#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""What each image weighs against its board's budget, owned by docs/build-and-flash.md section 3.

Two figures, the rcp images and the probe images, with one row per image, all to one byte scale, so a
row compares with every other in either figure: the bar is the image, in amber, and the hatched rest
of the row is the room left in what its board's guard allows, read from tools/gcf_check.py. The sizes
are measured from a build of an export, where the version string is the release's, and are updated in
SIZES when they change.

    python3 tools/diagrams/image_sizes.py            # docs/images/image-sizes-{rcp,probe}.png
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
    ('cb2', 'ConBee II', 51468, 'build/rcp', True),
    ('rb2', 'RaspBee II', 46672, 'build/rb2-rcp', True),
    ('cb3', 'ConBee III', 61024, 'build/fw-cb3-rcp', True),
    ('cb3', 'ConBee III, own', 24076, 'build/fw-cb3-own', False),
    ('cbrb1', 'ConBee I, RaspBee I', 43572, 'build/fw-cbrb1-rcp', True),
)
PROBE = (
    ('cb2', 'ConBee II', 31516, 'build/fw', True),
    ('rb2', 'RaspBee II', 26716, 'build/rb2', True),
    ('cb3', 'ConBee III', 7264, 'build/fw-cb3', True),
    ('cbrb1', 'ConBee I, RaspBee I', 6624, 'build/fw-cbrb1', True),
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


def spaced(n):
    return '{:,}'.format(n).replace(',', ' ') + ' B'


def rows(f, images, legend):
    room = budgets()
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
    return rows(f, RCP, [('amb', 'the image'), ('blk', 'the image with the unselected back-end'),
                         ('keep', 'the room left')])


def build_probe():
    f = figure.Fig(W, 600, 'PROBE IMAGE SIZES', 'each image against the room its board allows, to the scale of the RCP figure')
    return rows(f, PROBE, [('amb', 'the image'), ('keep', 'the room left')])


if __name__ == '__main__':
    sys.exit(figure.main('image-sizes-rcp', build_rcp) | figure.main('image-sizes-probe', build_probe))
