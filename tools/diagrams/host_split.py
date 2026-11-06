#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""What is in this firmware, against what the vendor firmware puts there.

Owned by docs/architecture.md section 1. The claim the figure makes is a claim about this tree -
nwk, aps and zdo are not modules here - and the generator checks it: a block drawn as the host's
must have no source file, so the day the stack lands in `src/rcp/`, this figure stops drawing
instead of lying.

The two panels share one board, and the boundary is at a different height in each: three layers
below it on the left, five on the right. That difference is the whole message, so the stack that
moves across the line is the figure's subject, in amber.

    python3 tools/diagrams/host_split.py            # docs/images/host-split.png
    python3 tools/diagrams/host_split.py --check    # is the committed raster still this figure?
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import figure  # noqa: E402
from figure import D, MX, S2, S3, S4  # noqa: E402

W = 880
PANEL_W = 404
GAP = W - 2 * MX - 2 * PANEL_W
BH, BG = 44, 8                 # a block and the air between two
LABEL = 26                     # a group's caps label and the air under it
RULE = 64                      # a boundary: the air, the rule with its chip, the air

# Each layer is declared once and drawn in both panels with the same words: on the left it is the
# host's, on the right the MCU's.
ZIGBEE = [
    ('ZDO', 'device object %s discovery' % D),
    ('APS', 'binding %s groups' % D),
    ('NWK', 'routes %s relay %s security' % (D, D)),
]
MAC = ('MAC', 'CSMA %s Ack %s address filters' % (D, D))
PHY = ('PHY', 'the board\'s 802.15.4 radio')

PANELS = [
    dict(label='PHOSCON RCP', sub='the stack runs on the host',
         host=[('host application', 'a Zigbee stack %s Matter over Thread %s a sniffer' % (D, D),
                'blk')] + [z + ('amb',) for z in ZIGBEE],
         chip='Spinel', wire='HDLC frames over USB or a UART',
         firmware=[('Spinel', 'properties %s the raw frame stream' % D, 'blk'),
                   MAC + ('blk',), PHY + ('blk',)]),
    dict(label='VENDOR deCONZ FIRMWARE', sub='the stack runs on the MCU',
         host=[('deCONZ', 'the vendor\'s host application', 'blk')],
         chip='deCONZ serial protocol', wire='APS-level commands',
         firmware=[z + ('amb',) for z in ZIGBEE] + [MAC + ('blk',), PHY + ('blk',)]),
]
BOARDS = ['ConBee I', 'ConBee II', 'ConBee III', 'RaspBee I', 'RaspBee II']

# The modules the left panel says are not here.
ABSENT = ['!src/rcp/nwk.c', '!src/rcp/aps.c', '!src/rcp/zdo.c']


def stack(f, x, y, blocks):
    for name, sub, kind in blocks:
        f.block(x, y, PANEL_W, BH, name, sub, kind)
        y += BH + BG
    return y - BG


def boundary(f, x, y, chip, wire):
    """A boundary: a rule across the panel, broken by the chip that names the protocol."""
    mid = y + RULE / 2.0
    w = f.chip(x, mid - 12, chip)
    f.line([(x + w, mid), (x + PANEL_W, mid)], 'f-out')
    f.text(x + PANEL_W, mid - S2, wire, 'f-sub', 'end')
    return y + RULE


def build():
    most = max(len(p['host']) + len(p['firmware']) for p in PANELS)
    body = 2 * LABEL + most * (BH + BG) + RULE
    h = 70 + body + S4 + 52 + 44
    f = figure.Fig(W, h)
    top = f.top
    y_end = top + 40 + body
    for i, p in enumerate(PANELS):
        x = MX + i * (PANEL_W + GAP)
        f.text(x, top + 8, p['label'], 'f-title')
        f.text(x, top + 26, p['sub'], 'f-lead')
        y = top + 40
        f.text(x, y + 14, 'HOST', 'f-cap')
        stack(f, x, y + LABEL, p['host'])
        # the firmware is stacked up from the board, so both radios sit on the same line and the
        # boundary lands where the layer count puts it
        fw_top = y_end - len(p['firmware']) * (BH + BG) + BG
        f.text(x, fw_top - LABEL + 14, 'FIRMWARE', 'f-cap')
        stack(f, x, fw_top, p['firmware'])
        boundary(f, x, fw_top - LABEL - RULE, p['chip'], p['wire'])
    by = y_end + S4
    f.block(MX, by, W - 2 * MX, 52, ('  %s  ' % D).join(BOARDS),
            'one 802.15.4 radio each: the same hardware under both', 'brt')
    f.legend(by + 52 + 30, [('amb', 'the Zigbee stack: what moves across the line'),
                            ('blk', 'a layer'), ('brd', 'the board')])
    return f


if __name__ == '__main__':
    sys.exit(figure.main('host-split', build, paths=ABSENT, calls=True))
