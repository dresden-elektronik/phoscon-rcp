#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""The receive path, owned by docs/architecture.md section 3.

Two producers - the host link and the radio - run in interrupt context and hand their bytes and
frames to the main loop through fixed rings. The rings are the subject: they are where the two
contexts meet, and each one's overflow rule is printed on it. Every function and ring named here
is checked against src/ before the figure draws.

    python3 tools/diagrams/rx_path.py            # docs/images/rx-path.png
    python3 tools/diagrams/rx_path.py --check
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import figure  # noqa: E402
from figure import D, MX, S2, S4  # noqa: E402

W = 1200
BH = 54
SYMBOLS = ['RcpApp_PushFromIrq', 'rcpRxRing', 'Hdlc_RxByte', 'Rcp_Feed', 'Hdlc_Encode',
           'rcpAppRxFrame', 'radioRxDeliver', 'RADIO_RX_SLOTS', 'RCP_APP_RX_RING',
           'RCP_APP_TX_RING', 'COM_TX_MAX', 'Radio_Frame']

# the columns, left to right: outside, interrupt context, the rings, the main loop, the TX ring
SRC = (MX, 104)
IRQ = (152, 204)
RING = (384, 156)
LOOP_A, LOOP_B = (576, 160), (752, 160)
TXR = (948, 140)
OUT = (1108, 68)


def right(c):
    return c[0] + c[1]


def build():
    f = figure.Fig(W, 520, 'RX PATH: TWO PRODUCERS, ONE LOOP',
                   'interrupts fill fixed rings; the main loop empties them, and every drop is counted')
    top = f.top + S2
    ya, yb = top + 44, top + 168             # the host row and the radio row
    band_h = yb + BH + 74 - top
    f.band(IRQ[0] - 12, top, IRQ[1] + 24, band_h, 'INTERRUPT CONTEXT')
    f.band(LOOP_A[0] - 16, top, right(LOOP_B) - LOOP_A[0] + 32, band_h, 'MAIN LOOP')

    f.block(SRC[0], ya, SRC[1], BH, 'host', 'USB or UART', 'brt')
    f.block(SRC[0], yb, SRC[1], BH, 'radio', 'a frame arrives', 'brt')

    f.block(IRQ[0], ya, IRQ[1], BH, 'RcpApp_PushFromIrq', 'one byte, or a UART run')
    f.block(IRQ[0], yb, IRQ[1], BH, 'radio vector', 'read %s filter %s start the Ack' % (D, D))

    f.block(RING[0], ya, RING[1], BH, 'rcpRxRing', '512 B %s drop-oldest' % D, 'amb')
    f.block(RING[0], yb, RING[1], BH, 'Radio_Frame ring', '8 slots %s drop-newest' % D, 'amb')

    f.block(LOOP_A[0], ya, LOOP_A[1], BH, 'Hdlc_RxByte', 'decode, one byte a call')
    f.block(LOOP_B[0], ya, LOOP_B[1], BH, 'Rcp_Feed', 'the session answers')
    f.block(LOOP_A[0], yb, LOOP_A[1], BH, 'rcpAppRxFrame', 'a STREAM_RAW notice')
    f.block(LOOP_B[0], yb, LOOP_B[1], BH, 'Hdlc_Encode', 'one reply at a time')

    f.block(TXR[0], yb, TXR[1], BH, 'TX ring', 'drop-whole', 'amb')
    f.block(OUT[0], yb, OUT[1], BH, 'host', None, 'brt', center=True)

    ma, mb = ya + BH / 2.0, yb + BH / 2.0
    for row in ((SRC, IRQ, RING, LOOP_A, LOOP_B), (SRC, IRQ, RING, LOOP_A, LOOP_B, TXR, OUT)):
        y = ma if len(row) == 5 else mb
        for a, b in zip(row, row[1:]):
            f.arrow([(right(a) + 2, y), (b[0] - 2, y)])
    # the session's answer joins the radio's notifications on their way out
    cx = LOOP_B[0] + LOOP_B[1] / 2.0
    f.arrow([(cx, ya + BH + 2), (cx, yb - 2)])
    f.text(cx + S2, (ya + BH + yb) / 2.0 + 4, 'the reply', 'f-sub')
    f.text(TXR[0], yb + BH + 18, 'to the link in COM_TX_MAX packets', 'f-sub')

    # the Ack never reaches the loop: the vector starts it inside the turnaround
    ax = IRQ[0] + 40
    low = yb + BH + 30
    f.arrow([(ax, yb + BH + 2), (ax, low), (SRC[0] + SRC[1] / 2.0, low),
             (SRC[0] + SRC[1] / 2.0, yb + BH + 4)])
    f.text(IRQ[0], low + 18, 'the Ack, inside the 192 µs turnaround', 'f-sub')

    ly = f.legend(top + band_h + 34, [('amb', 'a ring: where interrupt and loop meet'),
                                      ('blk', 'a step'), ('band', 'the context it runs in'),
                                      ('brd', 'outside the firmware'),
                                      ('out', 'bytes or a frame, handed on')])
    f.h = ly + S4
    return f


if __name__ == '__main__':
    sys.exit(figure.main('rx-path', build, symbols=SYMBOLS))
