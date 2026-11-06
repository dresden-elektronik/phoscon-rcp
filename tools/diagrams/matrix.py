#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""What tools/rcp_matrix.py runs, owned by docs/testing.md section 3.

The method, not a result: every ordered pair of nodes - a sender row, a receiver column - runs two
cells, a burst and an Ack, while every other node's radio is off. Six nodes make thirty pairs and
sixty cells. One pair is the subject, in amber, and the panel
beside the matrix says what its two cells assert.

    python3 tools/diagrams/matrix.py            # docs/images/matrix.png
    python3 tools/diagrams/matrix.py --check
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import figure  # noqa: E402
from figure import MX, S2, S4  # noqa: E402

W = 900
NODES = ['ConBee II', 'ConBee II', 'ConBee III', 'ConBee III', 'RaspBee II', 'ConBee I']
CELL = 48
PICK = (0, 4)                  # the pair the panel explains: sender row, receiver column
PATHS = ['tools/rcp_matrix.py']


def build():
    f = figure.Fig(W, 560, 'EVERY BOARD AGAINST EVERY OTHER',
                   'each ordered pair runs a burst and an Ack, with every other radio off')
    n = len(NODES)
    gx = MX + 112
    gy = f.top + 110
    f.text(gx + n * CELL / 2.0, gy - 92, 'RECEIVER', 'f-cap', 'middle')
    f.text(MX, gy - 12, 'SENDER', 'f-cap')
    for j, name in enumerate(NODES):
        f.text(gx + j * CELL + CELL / 2.0 + 4, gy - 8, name, 'f-sub', 'start', rotate=-90)
    for i, name in enumerate(NODES):
        f.text(gx - S2, gy + i * CELL + CELL / 2.0 + 4, name, 'f-sub', 'end')
        for j in range(n):
            x, y = gx + j * CELL, gy + i * CELL
            if i == j:
                f.rect(x + 3, y + 3, CELL - 6, CELL - 6, 'f-none', rx=2)
                continue
            kind = 'f-amb' if (i, j) == PICK else 'f-blk'
            half = (CELL - 9) / 2.0
            f.rect(x + 3, y + 3, half, CELL - 6, kind, rx=2)
            f.rect(x + 6 + half, y + 3, half, CELL - 6, kind, rx=2)
            f.text(x + 3 + half / 2.0, y + CELL / 2.0 + 4, 'B', 'f-axl', 'middle')
            f.text(x + 6 + half * 1.5, y + CELL / 2.0 + 4, 'A', 'f-axl', 'middle')

    # the panel: what the amber pair's two cells assert
    px = gx + n * CELL + 56
    pw = W - MX - px
    py = gy
    si, rj = PICK
    f.text(px, py + 4, '%s  →  %s' % (NODES[si], NODES[rj]), 'f-lab')
    f.text(px, py + 20, 'one ordered pair, two cells', 'f-sub')
    rows = [('B  burst', 'the receiver is promiscuous; passes when 90 % of',
             'the burst arrives byte-identical'),
            ('A  Ack', 'the receiver is a MAC node; passes on LAST_STATUS 0',
             'within three attempts'),
            ('off', 'every other node\'s radio: a bystander on the same PAN',
             'and address would acknowledge in the receiver\'s place')]
    y = py + 44
    for name, l1, l2 in rows:
        f.rect(px, y, pw, 62, 'f-blk' if name != 'off' else 'f-tag')
        f.text(px + 12, y + 20, name, 'f-msg')
        f.text(px + 12, y + 37, l1, 'f-sub')
        f.text(px + 12, y + 51, l2, 'f-sub')
        f.fits(l1, 'f-sub', pw - 24, 'the panel')
        f.fits(l2, 'f-sub', pw - 24, 'the panel')
        y += 70

    ly = f.legend(max(gy + n * CELL, y) + 34,
                  [('amb', 'the pair the panel explains'), ('blk', 'a cell: B burst, A Ack'),
                   ('none', 'no cell: a node does not test itself')])
    f.h = ly + S4
    return f


if __name__ == '__main__':
    sys.exit(figure.main('matrix', build, paths=PATHS))
