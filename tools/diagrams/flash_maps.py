#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""Where an image may go on each part, owned by docs/build-and-flash.md section 6.

One bar per silicon family, each to the scale of its own flash, address growing to the right. The
region an image may occupy is the subject, in amber; everything a flash must leave alone - the
bootloader, the deCONZ NVRAM, the bootloader's staging area - is hatched. The boundaries are the
ones tools/gcf_check.py enforces, and the generator reads them from there, so the figure and the
guard cannot disagree.

    python3 tools/diagrams/flash_maps.py            # docs/images/flash-maps.png
    python3 tools/diagrams/flash_maps.py --check
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import figure  # noqa: E402
from figure import MX, S2, S4  # noqa: E402

W = 900
X0, X1 = 212, W - MX
BAR = 40
KIB = 1024


def guard():
    """The boundaries the pre-flash guard enforces, read from the guard itself."""
    with open(os.path.join(figure.ROOT, 'tools', 'gcf_check.py')) as f:
        src = f.read()

    def const(name):
        return int(re.search(r'^%s\s*=\s*(0x[0-9A-Fa-f]+|\d+)' % name, src, re.M).group(1), 0)
    return dict(app=const('APP_ORIGIN'), nvram=const('NVRAM_BASE'),
                avr_boot=const('AVR_BOOT_BASE'),
                cb3_end=0x4000 + const('CB3_APP_SECTOR_SIZE'),
                cb3_info=const('CB3_APP_INFO_SIZE'))


def parts():
    g = guard()
    return [
        dict(name='cb2 · rb2', sub='ATSAMR21E18A, 256 KiB', size=256 * KIB, regions=[
            (0, g['app'], 'bootloader', 'and its two flag bytes', 'keep'),
            (g['app'], g['nvram'], 'app image', 'from 0x%X' % g['app'], 'amb'),
            (g['nvram'], 256 * KIB, 'deCONZ NVRAM', 'the MAC address', 'keep'),
        ], marks=[(0, 'start'), (g['app'], 'start'), (g['nvram'], 'end')]),
        dict(name='cb3', sub='EFR32MG21, 1 MiB', size=1024 * KIB, regions=[
            (0, 0x4000, 'bootloader', None, 'keep'),
            (0x4000, g['cb3_end'] - g['cb3_info'], 'app image', 'the AppSection', 'amb'),
            (g['cb3_end'] - g['cb3_info'], g['cb3_end'], 'AppInfo', 'the bootloader writes it',
             'keep'),
            (g['cb3_end'], 0xF8000, 'staging area', 'the bootloader\'s', 'keep'),
            (0xF8000, 1024 * KIB, 'NVM3', None, 'keep'),
        ], marks=[(0x4000, 'start'), (g['cb3_end'] - g['cb3_info'], 'middle'), (0xF8000, 'end')]),
        dict(name='cbrb1', sub='ATmega256RFR2, 256 KiB', size=256 * KIB, regions=[
            (0, g['avr_boot'], 'app image', 'from 0: the reset vector', 'amb'),
            (g['avr_boot'], 256 * KIB, 'bootloader', 'the boot section', 'keep'),
        ], marks=[(0, 'start'), (g['avr_boot'], 'end')]),
    ]


def bar(f, y, p):
    """One part's flash as a bar to its own scale; returns the lowest y it drew at."""
    def x(a):
        return X0 + (X1 - X0) * a / float(p['size'])

    f.text(MX, y + 16, p['name'], 'f-lab')
    f.text(MX, y + 31, p['sub'], 'f-sub')
    below = []
    for a, b, name, sub, kind in p['regions']:
        w = x(b) - x(a)
        cls = {'amb': 'f-amb', 'keep': 'f-keep'}[kind]
        f.rect(x(a), y, w, BAR, cls, rx=0)
        tn = 'f-aid' if kind == 'amb' else 'f-id'
        if figure.textW(name, tn) + 16 < w and (not sub or figure.textW(sub, 'f-sub') + 16 < w):
            f.fits(name, tn, w - 8, 'the region %s' % name)
            if kind == 'keep':
                # a label on a hatch sits on a white ground of its own
                f.rect(x(a) + w / 2.0 - figure.textW(name, tn) / 2.0 - 6, y + 6,
                       figure.textW(name, tn) + 12, 18 if not sub else 30, 'f-blk', rx=1)
            f.text(x(a) + w / 2.0, y + (19 if sub else 24), name, tn, 'middle')
            if sub:
                f.text(x(a) + w / 2.0, y + 32, sub, 'f-asub' if kind == 'amb' else 'f-sub',
                       'middle')
        else:
            below.append(((x(a) + x(b)) / 2.0, name, sub))
    # only the boundaries a reader needs, each anchored away from its neighbour; the part's size
    # is in its subtitle
    for m, anchor in p['marks']:
        f.line([(x(m), y - 4), (x(m), y)], 'f-tickm')
        f.text(x(m), y - 8, '0x%X' % m, 'f-bit', anchor)
    # labels that do not fit their region hang below it on a leader, right-aligned to it
    ly = y + BAR + 18
    last = None
    for cx, name, sub in below:
        if last is not None and cx - last < 150:
            ly += 30
        f.line([(cx, y + BAR), (cx, ly - 12)], 'f-rule')
        anchor = 'end' if cx > (X0 + X1) / 2.0 else 'start'
        f.text(cx, ly, name, 'f-lab', anchor)
        if sub:
            f.text(cx, ly + 14, sub, 'f-sub', anchor)
        last = cx
    return (ly + 20) if below else (y + BAR)


def build():
    f = figure.Fig(W, 600, 'WHERE AN IMAGE MAY GO',
                   'each part\'s flash to its own scale; the guard refuses an image that reaches '
                   'a hatched region')
    y = f.top + 24
    for p in parts():
        y = bar(f, y, p) + 44
    ly = f.legend(y - 8, [('amb', 'the app image\'s room'),
                          ('keep', 'not this firmware\'s: a flash leaves it alone')])
    f.h = ly + S4
    return f


if __name__ == '__main__':
    sys.exit(figure.main('flash-maps', build))
