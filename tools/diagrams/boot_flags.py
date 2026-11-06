#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""The ConBee II bootloader's two flag bytes, owned by docs/hardware_samr21.md section 7.

What the flags hold after a flash and after each boot, and the two ways on from the first boot:
the app clears the update flag and every later reset is safe, or it dies first and the next boot
erases the app's first page. The update flag at 0x4FFF is the subject - the one byte the app must
write - and the generator checks that the tree still clears it.

    python3 tools/diagrams/boot_flags.py            # docs/images/boot-flags.png
    python3 tools/diagrams/boot_flags.py --check
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import figure  # noqa: E402
from figure import MX, S2, S4  # noqa: E402

W = 900
SW, SH = 150, 104
# (key, x, y-row, title, line, update 0x4FFF, reboot 0x4FFE, kind of the box)
STATES = [
    ('flashed', MX, 0, 'after a flash', 'the upload set both', 'A3', 'AD', 'blk'),
    ('boot1', 224, 0, 'clean boot 1', 'reboot flag cleared', 'A3', '00', 'blk'),
    ('safe', 510, -1, 'app running', 'cleared 0x4FFF early', '00', '00', 'blk'),
    ('boot2', 510, 1, 'clean boot 2', 'erases page 0x5000', 'A3', '00', 'blk'),
    ('stuck', W - MX - SW, 1, 'bootloader forever', 'word[0x5000] erased', '--', '--',
     'tag'),
]


def state(f, x, y, title, line, upd, rbt, kind):
    f.rect(x, y, SW, SH, 'f-blk' if kind == 'blk' else 'f-tag')
    f.text(x + 12, y + 22, title, 'f-lab')
    f.text(x + 12, y + 38, line, 'f-sub')
    f.fits(line, 'f-sub', SW - 24, 'the state %r' % title)
    if upd == '--':
        f.text(x + 12, y + 70, 'the start window', 'f-sub')
        f.text(x + 12, y + 84, 'never closes', 'f-sub')
        return
    cw = (SW - 24 - S2) / 2.0
    for i, (addr, val) in enumerate((('0x4FFF', upd), ('0x4FFE', rbt))):
        cx = x + 12 + i * (cw + S2)
        f.text(cx, y + 58, addr, 'f-bit')
        f.rect(cx, y + 64, cw, 28, 'f-amb' if (i == 0 and val != '00') else 'f-blk', rx=1)
        f.text(cx + cw / 2.0, y + 83, '0x' + val, 'f-aid' if (i == 0 and val != '00') else 'f-id',
               'middle')


def build():
    f = figure.Fig(W, 560, 'THE CONBEE II BOOT FLAGS',
                   'the app must clear the update flag at 0x4FFF at every boot, before anything can '
                   'hang')
    mid = f.top + 30 + SH + 40
    ys = {0: mid - SH / 2.0, -1: mid - SH - 40, 1: mid + 40}
    pos = {}
    for key, x, row, title, line, upd, rbt, kind in STATES:
        y = ys[row]
        pos[key] = (x, y)
        state(f, x, y, title, line, upd, rbt, kind)

    def edge(a, b, label, err=False):
        (ax, ay), (bx, by) = pos[a], pos[b]
        y0, y1 = ay + SH / 2.0, by + SH / 2.0
        if abs(y0 - y1) < 1:
            f.arrow([(ax + SW + 2, y0), (bx - 2, y1)])
            f.text((ax + SW + bx) / 2.0, y0 - 8, label, 'f-sub', 'middle')
        else:
            # the branch turns close to its source, so the label has the run into the target
            xm = ax + SW + 20
            f.arrow([(ax + SW + 2, y0), (xm, y0), (xm, y1), (bx - 2, y1)])
            f.text(xm + S2, y1 - 8, label, 'f-sub')
            f.fits(label, 'f-sub', bx - xm - 2 * S2, 'the branch into %s' % b)

    edge('flashed', 'boot1', 'reset')
    edge('boot1', 'safe', 'the app clears it')
    edge('boot1', 'boot2', 'the app dies first')
    edge('boot2', 'stuck', '')
    sx, sy = pos['safe']
    f.text(sx + SW + S4, sy + SH / 2.0 + 4, 'any later reset is safe', 'f-ok')
    bx, by = pos['boot2']
    f.text(bx + SW + 6, by + SH / 2.0 + 18, 'gate fails', 'f-sub')

    ly = f.legend(ys[1] + SH + 36, [('amb', 'the update flag, set: the byte the app must clear'),
                                    ('blk', 'a state of the two flags'),
                                    ('tag', 'stuck: no app to start')])
    f.h = ly + S4
    return f


if __name__ == '__main__':
    # the tree must still clear the flag the figure says it clears
    sys.exit(figure.main('boot-flags', build, paths=['src/hal/samr21/hal_samr21.c']))
