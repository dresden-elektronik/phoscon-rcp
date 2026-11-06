#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""Who may run when, owned by docs/architecture.md section 7.

The execution contexts from the highest priority down: the RaspBee II's UART vector, the radio
vector, the ConBee II's USB vector beside SysTick, and the main loop. Each says what it may do. The
radio vector is the subject - its Ack turnaround is the deadline the board is built around - and the
lock the loop takes against it is drawn as what it is: the loop masking that one line. Names are the
SAMR21's.

    python3 tools/diagrams/contexts.py            # docs/images/contexts.png
    python3 tools/diagrams/contexts.py --check
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import figure  # noqa: E402
from figure import D, MX, S2, S4  # noqa: E402

W = 900
X0 = 214
XR = W - MX - 96             # rows end here; the lock is drawn in the lane beyond
RH = 58
SYMBOLS = ['SERCOM5_Handler', 'EIC_Handler', 'RcpApp_PushFromIrq', 'Rf_Lock', 'HAL_Wfi', 'Nvram_StatusFlush',
           'HAL_InIrqContext']

LEVELS = [
    ('priority 0', 'preempts all below', [
        ('UART vector, RaspBee II', 'octets into the ring before its two-octet buffer overflows', 'blk')]),
    ('priority 1', 'preempts the rest', [
        ('radio vector', 'one frame: read, filter, maybe start the Ack, push one slot', 'amb')]),
    ('priority 2', 'equal: neither preempts', [
        ('USB vector, ConBee II', 'bytes into the ring', 'blk'),
        ('SysTick', 'the millisecond', 'blk')]),
    ('main loop', 'thread context', [
        ('everything else', 'HDLC, the session, TX steps, flash writes; then HAL_Wfi', 'blk')]),
]


def build():
    f = figure.Fig(W, 610, 'WHO MAY RUN WHEN',
                   'handlers do bounded hardware work; the loop does the rest')
    y = f.top + 14
    rows = []
    for name, sub, blocks in LEVELS:
        f.text(MX, y + 24, name, 'f-lab')
        f.text(MX, y + 40, sub, 'f-sub')
        n = len(blocks)
        gap = S2
        widths = [(XR - X0 - gap * (n - 1)) * (0.68 if n == 2 and i == 0 else
                                                    0.32 if n == 2 else 1.0)
                  for i in range(n)]
        x = X0
        for (bn, bs, kind), w in zip(blocks, widths):
            f.block(x, y, w, RH, bn, bs, kind)
            x += w + gap
        rows.append(y)
        y += RH + 30

    # preemption: each level may interrupt the ones below it
    ax = X0 - 22
    f.line([(ax, rows[0] + RH / 2.0), (ax, rows[-1] + RH / 2.0 - 4)], 'f-out', 'ink')
    f.text(ax - 6, (rows[0] + rows[-1] + RH) / 2.0, 'PREEMPTS', 'f-cap', 'middle', rotate=-90)

    # the loop's lock against the radio vector, in the lane right of the rows
    lane = XR + 40
    f.line([(XR + 2, rows[3] + RH / 2.0), (lane, rows[3] + RH / 2.0), (lane, rows[1] + RH / 2.0),
            (XR + 4, rows[1] + RH / 2.0)], 'f-back', 'line')
    f.text(lane + 14, (rows[1] + rows[3] + RH) / 2.0, 'Rf_Lock: the loop masks it', 'f-sub',
           'middle', rotate=90)

    ny = rows[3] + RH + 26
    f.text(X0, ny, 'never in a handler:', 'f-lab')
    f.text(X0 + figure.textW('never in a handler:', 'f-lab') + S2, ny,
           'flash programming %s encoding %s a spin %s a record from the radio vector' % (D, D, D),
           'f-sub')

    ly = f.legend(ny + 30, [('amb', "the Ack's deadline"),
                            ('blk', 'a context'), ('out', 'may preempt'),
                            ('back', 'masks')])
    f.h = ly + S4
    return f


if __name__ == '__main__':
    sys.exit(figure.main('contexts', build, symbols=SYMBOLS))
