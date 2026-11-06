#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""The debug record and where it goes, owned by docs/architecture.md section 8.

Every diagnostic is one 8-byte record - the subject, in amber - and the figure shows its four
fields, then the path: two producer rings, one queue, and the image's way out, plus the post-mortem
row that survives a flash.

    python3 tools/diagrams/debug_record.py            # docs/images/debug-record.png
    python3 tools/diagrams/debug_record.py --check
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import figure  # noqa: E402
from figure import MX, S2, S4  # noqa: E402

W = 900
FW = W - 2 * MX
SYMBOLS = ['Debug_Emit', 'DBG_COMP_DBGQ', 'PROPS_ID_RCP_LOG_CRASH_DUMP', 'Nvram_StatusFlush']
FIELDS = [('magic', '0x0000', 2, 'amb'), ('component', 'which part spoke', 2, 'amb'),
          ('line', 'a source line or a value', 2, 'amb'), ('reason', 'what happened', 2, 'amb')]


def build():
    f = figure.Fig(W, 520, 'THE DEBUG RECORD',
                   'eight bytes, four little-endian u16 fields; no strings, no header, no checksum')
    y = f.top + 22
    figure.strip(f, MX, y, FIELDS, FW / 8.0, 50)

    # the path a record takes
    y2 = y + 50 + 54
    f.text(MX, y2 - 14, 'THE QUEUE: ONE RING PER PRODUCER', 'f-cap')
    bw, bh = 196, 50
    f.block(MX, y2, bw, bh, 'handler ring', '64 slots; full drops the oldest', 'blk')
    f.block(MX, y2 + bh + 12, bw, bh, 'loop ring', '64 slots; full drops the oldest', 'blk')
    qx = MX + bw + 60
    f.block(qx, y2 + 31, bw, bh, 'the drain', 'batches to the way out', 'blk')
    for yy in (y2 + bh / 2.0, y2 + bh + 12 + bh / 2.0):
        f.arrow([(MX + bw + 2, yy), (MX + bw + 34, yy), (MX + bw + 34, y2 + 56),
                 (qx - 2, y2 + 56)])
    ox = qx + bw + 60
    ow = W - MX - ox
    outs = [('probe image', 'the host link\'s IN stream', 'brt'),
            ('RCP image', 'in-band: RCP_LOG_CRASH_DUMP', 'brt'),
            ('post-mortem row', 'probe image: the last record', 'tag')]
    for i, (name, sub, kind) in enumerate(outs):
        oy = y2 - 24 + i * 62
        f.block(ox, oy, ow, bh, name, sub, kind)
        f.arrow([(qx + bw + 2, y2 + 56), (qx + bw + 34, y2 + 56), (qx + bw + 34, oy + bh / 2.0),
                 (ox - 2, oy + bh / 2.0)])

    ly = f.legend(y2 + 3 * 62 + 10, [('amb', 'the record'), ('blk', 'a fixed ring'),
                                     ('brd', 'the image\'s way out'),
                                     ('tag', 'flash, written from the loop only')])
    f.h = ly + S4
    return f


if __name__ == '__main__':
    sys.exit(figure.main('debug-record', build, symbols=SYMBOLS))
