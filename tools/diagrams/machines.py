#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""The two machines that share the radio with something else, owned by docs/architecture.md
section 6.

The radio's transmit machine and the energy-scan dwell, each advanced one step per main-loop pass.
The subject is where each machine yields: the TX machine waits in TO_ARET while the radio vector
holds the transceiver, and the dwell parks in SUSPEND while a transmission borrows it. Every state
drawn is checked against its enum in src/.

    python3 tools/diagrams/machines.py            # docs/images/machines.png
    python3 tools/diagrams/machines.py --check
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import figure  # noqa: E402
from figure import MX, S2, S4  # noqa: E402

W = 900
BH = 46
TX = [('IDLE', 'nothing in flight', 'blk'), ('TO_ARET', 'wait for the radio', 'amb'),
      ('WAIT_ARET', 'the state settles', 'blk'), ('WAIT_END', 'on air, then the Ack', 'blk')]
SCAN = [('IDLE', None, 'blk'), ('TUNE', 'retune, arm', 'blk'), ('SETTLE', 'the settle time', 'blk'),
        ('MEASURE', 'request a level', 'blk'), ('READ', 'take it once', 'blk')]
SCAN2 = [('RESTORE', 'the host\'s RX', 'blk'), ('EMIT', 'the result', 'blk'),
         ('DONE', None, 'blk')]
SYMBOLS = (['RADIO_TX_' + s[0] for s in TX]
           + ['SCAN_PHASE_' + s[0] for s in SCAN + SCAN2] + ['SCAN_PHASE_SUSPEND'])


def row(f, y, states, x0, widths, gap):
    xs = []
    x = x0
    for (name, sub, kind), w in zip(states, widths):
        f.block(x, y, w, BH, name, sub, kind)
        xs.append((x, w))
        x += w + gap
    return xs


def loop_over(f, x0, x1, y, label):
    """An arc above a row from x0 back to x1, its label above it."""
    f.arrow([(x0, y - 2), (x0, y - 22), (x1, y - 22), (x1, y - 2)])
    f.text((x0 + x1) / 2.0, y - 28, label, 'f-sub', 'middle')


def build():
    f = figure.Fig(W, 760, 'THE MACHINES THAT SHARE THE RADIO',
                   'one step per main-loop pass; amber marks where a machine yields the radio to '
                   'another owner')
    # the radio's TX machine
    y = f.top + 22
    f.text(MX, y, 'RADIO TX, samr21_radio.c', 'f-cap')
    ya = y + 62
    gap = 96
    w = (W - 2 * MX - 3 * gap) / 4.0
    xs = row(f, ya, TX, MX, [w] * 4, gap)
    for (ax, aw), (bx, _bw), word in zip(xs, xs[1:], ['Radio_Tx', 'TX_ARET_ON', 'TX_START']):
        f.arrow([(ax + aw + 2, ya + BH / 2.0), (bx - 2, ya + BH / 2.0)])
        f.text((ax + aw + bx) / 2.0, ya + BH / 2.0 - 6, word, 'f-msg', 'middle')
        f.fits(word, 'f-msg', gap - 8, 'the arrow %r' % word)
    tx, tw = xs[1]
    loop_over(f, tx + tw - 24, tx + 24, ya, 'an Ack on air or a frame waiting: next pass')
    ex, ew = xs[3]
    ix, iw = xs[0]
    low = ya + BH + 30
    f.arrow([(ex + ew / 2.0, ya + BH + 2), (ex + ew / 2.0, low), (ix + iw / 2.0, low),
             (ix + iw / 2.0, ya + BH + 4)])
    f.text((ix + iw / 2.0 + ex + ew / 2.0) / 2.0, low + 16,
           'TRX_END: the outcome; a missed phase deadline: TIMEOUT', 'f-sub', 'middle')

    # the energy-scan dwell
    y = low + 64
    f.text(MX, y, 'ENERGY-SCAN DWELL, rcp/scan.c', 'f-cap')
    yb = y + 90
    gap = 36
    wi = 96
    w = (W - 2 * MX - wi - 4 * gap) / 4.0
    xs = row(f, yb, SCAN, MX, [wi] + [w] * 4, gap)
    for (ax, aw), (bx, _bw) in zip(xs, xs[1:]):
        f.arrow([(ax + aw + 2, yb + BH / 2.0), (bx - 2, yb + BH / 2.0)])
    f.text(xs[0][0] + wi + gap / 2.0, yb + BH / 2.0 - 6, 'Start', 'f-sub', 'middle')
    mx, mw = xs[3]
    rx, rw = xs[4]
    loop_over(f, rx + rw / 2.0, mx + mw / 2.0, yb, 'samples left')

    # the way out, back along a second row
    y2 = yb + BH + 50
    xs2 = [(xs[4][0], xs[4][1]), (xs[3][0], xs[3][1]), (xs[2][0], xs[2][1])]
    for (name, sub, kind), (x, ww) in zip(SCAN2, xs2):
        f.block(x, y2, ww, BH, name, sub, kind)
    f.arrow([(rx + rw / 2.0, yb + BH + 2), (rx + rw / 2.0, y2 - 2)])
    f.text(rx + rw / 2.0 + S2, yb + BH + 30, 'none left', 'f-sub')
    for (ax, _aw), (bx, bw) in zip(xs2, xs2[1:]):
        f.arrow([(ax - 2, y2 + BH / 2.0), (bx + bw + 2, y2 + BH / 2.0)])
    dx, dw = xs2[2]
    ix, iw = xs[0]
    f.arrow([(dx - 2, y2 + BH / 2.0), (ix + iw / 2.0, y2 + BH / 2.0), (ix + iw / 2.0, yb + BH + 2)])

    # SUSPEND: a transmission borrows the transceiver; RESUME re-runs TUNE
    tx, tw = xs[1]
    sx, sw = xs[2]
    sy = yb - 76
    f.block(sx, sy, sw, BH - 4, 'SUSPEND', 'a TX borrows it', 'amb')
    f.arrow([(sx - 2, sy + (BH - 4) / 2.0), (tx + tw / 2.0, sy + (BH - 4) / 2.0),
             (tx + tw / 2.0, yb - 2)])
    f.text(tx + tw / 2.0 - S2, sy + (BH - 4) / 2.0 - 6, 'resume', 'f-sub', 'end')
    f.text(sx + sw + S4, sy + 16, 'from any phase', 'f-sub')
    f.text(sx + sw + S4, sy + 30, 'that issues actions', 'f-sub')

    ly = f.legend(y2 + BH + 36, [('amb', 'where the machine yields the radio'),
                                 ('blk', 'a state'), ('out', 'one step, one pass')])
    f.h = ly + S4
    return f


if __name__ == '__main__':
    sys.exit(figure.main('machines', build, symbols=SYMBOLS))
