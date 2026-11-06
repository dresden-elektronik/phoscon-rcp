#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""The ConBee III's two radio back-ends, owned by docs/hardware_mg21.md section 10.4.

Both implement hal/radio.h, the subject in amber: rail on Silicon Labs' library, which the tree
links but does not contain (hatched), and own on this tree's sequencer program and generated
register tables. Both end in the same switch-off before a reset. Every file drawn is checked.

    python3 tools/diagrams/mg21_backends.py            # docs/images/mg21-backends.png
    python3 tools/diagrams/mg21_backends.py --check
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import figure  # noqa: E402
from figure import D, MX, S2, S4  # noqa: E402

W = 900
BH = 50
G = 12
PATHS = ['src/hal/radio.h', 'src/hal/mg21/rail/mg21_rail.c', 'src/hal/mg21/rail/rail_shims.c',
         'src/hal/mg21/own/mg21_radio.c', 'src/hal/mg21/own/mg21_seq_image.c',
         'src/hal/mg21/own/mg21_radio_cfg.c', 'src/hal/mg21/own/mg21_phy.c',
         'src/hal/mg21/mg21_radio_off.c', 'tools/mg21_seq.py', 'tools/mg21_radio_gen.py']


def build():
    f = figure.Fig(W, 600, 'THE CONBEE III RADIO BACK-ENDS',
                   'one contract, two implementations; -DDS_MG21_RADIO picks one')
    y = f.top + 8
    cw = (W - 2 * MX - 32) / 2.0
    lx, rx = MX, MX + cw + 32
    f.block(MX, y, W - 2 * MX, 36, 'hal/radio.h', 'the contract the RCP core calls', 'tag')

    y1 = y + 36 + 28
    f.group(lx, y1 - 8, cw, 'rail/', 'the rcp image\'s')
    f.group(rx, y1 - 8, cw, 'own/', 'no image; -DDS_MG21_RADIO=own')
    y1 += 12
    f.block(lx, y1, cw, BH, 'mg21_rail.c', 'the adapter; the only file with RAIL\'s headers',
            'amb')
    f.block(rx, y1, cw, BH, 'mg21_radio.c', 'polls; installs the program and the tables', 'amb')

    y2 = y1 + BH + G
    hw = (cw - S2) / 2.0
    f.block(lx, y2, hw, BH, 'RAIL library', 'fetched, pinned', 'keep')
    f.block(lx + hw + S2, y2, hw, BH, 'rail_shims.c', 'the symbols RAIL expects', 'blk')
    f.text(lx, y2 + BH + 18, 'RAIL owns the radio blocks %s its sequencer %s calibration %s '
           'the 10 radio IRQs' % (D, D, D), 'f-sub')

    tw = (cw - 2 * S2) / 3.0
    f.block(rx, y2, tw, BH, 'seq_image.c', 'the program', 'gap')
    f.block(rx + tw + S2, y2, tw, BH, 'radio_cfg.c', 'register values', 'gap')
    f.block(rx + 2 * (tw + S2), y2, tw, BH, 'mg21_phy.c', 'channel, record', 'blk')
    y3 = y2 + BH + 34
    f.block(rx, y3, tw, 40, 'mg21_seq.py', None, 'tag')
    f.block(rx + tw + S2, y3, tw, 40, 'radio_gen.py', None, 'tag')
    f.text(rx + 2 * (tw + S2), y3 + 16, 'the generators,', 'f-sub')
    f.text(rx + 2 * (tw + S2), y3 + 30, 'in tools/', 'f-sub')
    for i in (0, 1):
        cx = rx + i * (tw + S2) + tw / 2.0
        f.line([(cx, y3 - 2), (cx, y2 + BH + 4)], 'f-out', 'ink')

    y4 = y3 + 40 + 28
    f.block(MX, y4, W - 2 * MX, BH, 'mg21_radio_off.c',
            'both end here before every reset: receive withdrawn, blocks off, radio clock gated',
            'blk')
    f.block(MX, y4 + BH + G, W - 2 * MX, 40, 'EFR32MG21 radio', None, 'brt')

    ly = f.legend(y4 + BH + G + 40 + 30, [('amb', 'implements hal/radio.h'),
                                         ('blk', 'this tree\'s code'),
                                         ('gap', 'generated: do not edit'),
                                         ('keep', 'linked, not in the tree'),
                                         ('tag', 'a contract or a tool')])
    f.h = ly + S4
    return f


if __name__ == '__main__':
    sys.exit(figure.main('mg21-backends', build, paths=PATHS))
