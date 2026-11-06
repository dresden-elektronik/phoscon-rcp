#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""One energy-scan dwell on a timeline, owned by docs/protocol.md section 10.1.

What the host sends and receives along the top, and what the RCP does along the bottom: retune,
then measurements until MAC_SCAN_PERIOD is used - the subject, in amber - a transmission that
borrows the radio in between, then the host's receive state put back before the result and the
completion go out. Not to scale.

    python3 tools/diagrams/scan_dwell.py            # docs/images/scan-dwell.png
    python3 tools/diagrams/scan_dwell.py --check
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import figure  # noqa: E402
from figure import MX, S2, S4  # noqa: E402

W = 900
LANE = 36
SYMBOLS = ['PROPS_ID_MAC_SCAN_STATE', 'PROPS_ID_MAC_SCAN_PERIOD', 'SCAN_PHASE_SUSPEND',
           'SCAN_PHASE_RESTORE']


def build():
    f = figure.Fig(W, 460, 'ONE ENERGY-SCAN DWELL',
                   'one channel, one period, one level; the host\'s radio state is put back before '
                   'the result goes out')
    x0 = MX + 120
    xe = W - MX
    y_host = f.top + 34
    y_rcp = y_host + 96

    f.text(MX, y_host + 22, 'host', 'f-lab')
    f.text(MX, y_host + 37, 'Spinel frames', 'f-sub')
    f.text(MX, y_rcp + 22, 'RCP', 'f-lab')
    f.text(MX, y_rcp + 37, 'one step a loop pass', 'f-sub')
    f.line([(x0, y_host + LANE / 2.0), (xe, y_host + LANE / 2.0)], 'f-rule')

    # the RCP's lane, left to right
    segs = [('tune', 46, 'blk'), ('m', 34, 'amb'), ('m', 34, 'amb'), ('m', 34, 'amb'),
            ('TX', 74, 'gap'), ('tune', 46, 'blk'), ('m', 34, 'amb'), ('m', 34, 'amb'),
            ('m', 34, 'amb'), ('restore', 84, 'blk'), ('emit', 60, 'blk')]
    x = x0 + 40
    xs = {}
    for i, (name, w, kind) in enumerate(segs):
        f.block(x, y_rcp, w - 4, LANE, name if name != 'm' else '', None, kind, center=True)
        xs[i] = (x, w)
        x += w
    stop = xs[8][0] + xs[8][1] - 4
    f.hbracket(xs[0][0], stop, y_rcp + LANE + 10, 'MAC_SCAN_PERIOD: the board\'s clock ends it',
               below=True, cls='f-sub')
    tx, tw = xs[4]

    # the host's frames, as arrows between the lanes
    def down(x, label, sub=None):
        f.arrow([(x, y_host + LANE / 2.0 + 2), (x, y_rcp - 4)])
        f.text(x + 6, y_host + LANE / 2.0 - 6, label, 'f-msg')
        if sub:
            f.text(x + 6, y_host + LANE / 2.0 + 30, sub, 'f-sub')

    def up(x, label, anchor='end'):
        f.line([(x, y_rcp - 4), (x, y_host + LANE / 2.0 + 4)], 'f-back', 'line')
        f.text(x - 6 if anchor == 'end' else x + 6, y_host + LANE / 2.0 - 6, label, 'f-msg',
               anchor)

    down(xs[0][0] + 20, 'SET 0x30 = 02')
    down(tx + 20, 'SET STREAM_RAW', 'borrows the radio')
    ex, ew = xs[10]
    up(ex + 10, 'VALUE_IS 0x39 ch, dBm')
    up(ex + ew - 10, '0x30 = 00', 'start')
    f.text(xe, y_host + LANE / 2.0 + 18, 'the result, then the completion', 'f-sub', 'end')

    ly = f.legend(y_rcp + LANE + 64, [('amb', 'a measurement: request, confirm, read'),
                                      ('blk', 'a step'), ('gap', 'the radio lent to a transmission'),
                                      ('out', 'host to RCP'), ('back', 'RCP to host')])
    f.text(MX, ly + 22, 'not to scale', 'f-axl')
    f.h = ly + 30
    return f


if __name__ == '__main__':
    sys.exit(figure.main('scan-dwell', build, symbols=SYMBOLS))
