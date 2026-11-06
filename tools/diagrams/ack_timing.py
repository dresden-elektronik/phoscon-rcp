#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""The Ack's timing budget, owned by docs/ack-pending-frame.md section 3.

A Data Request with a nine-octet MAC header on a microsecond scale: what is on the air when, the
interrupts that mark it, and the window left to decide the Ack's Frame Pending bit from each of
them. The windows are the subject, in amber. The last lane is this firmware's own sequence
(section 5), on the same scale, so the reader sees where it lands inside the budget.

Every position is computed from the air timing - 16 us a symbol, 32 us an octet - so the scale is
true; nothing here is drawn by eye.

    python3 tools/diagrams/ack_timing.py            # docs/images/ack-timing.png
    python3 tools/diagrams/ack_timing.py --check
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import figure  # noqa: E402
from figure import MX, S2, S4  # noqa: E402

W = 900
X0, X1 = 196, W - MX           # the scale's 0 and its end
SPAN = 840                     # us shown
LANE = 30

OCTET = 32
SHR = 5 * OCTET                # preamble and SFD
RX_START = SHR + OCTET         # after the PHR
AMI = RX_START + 9 * OCTET     # the nine-octet header is in
TRX_END = AMI + OCTET + 2 * OCTET   # the command octet, then the FCS
ACK_DUE = TRX_END + 12 * 16    # the turnaround: twelve symbols
STAMP = TRX_END + 9            # section 5: the vector's entry, no earlier than tIRQ after the end
TX_START = STAMP + 167
ABANDON = STAMP + 441


def x(us):
    return X0 + (X1 - X0) * us / float(SPAN)


def seg(f, y, a, b, label, cls='f-blk', tcls='f-axl'):
    f.rect(x(a), y, x(b) - x(a), LANE, cls, rx=1)
    if label:
        f.fits(label, tcls, x(b) - x(a) - 4, 'the span %d..%d us' % (a, b))
        f.text((x(a) + x(b)) / 2.0, y + LANE / 2.0 + 4, label, tcls, 'middle')


def lane(f, y, name, sub):
    f.text(MX, y + 13, name, 'f-lab')
    f.text(MX, y + 27, sub, 'f-sub')


def build():
    f = figure.Fig(W, 520, 'THE ACK\'S TIMING BUDGET',
                   'a Data Request with a nine-octet MAC header, at 250 kbit/s: '
                   '32 µs an octet, to scale')
    top = f.top + 30
    y_air, y_ami, y_end, y_fw = top, top + 64, top + 108, top + 152
    bottom = y_fw + LANE

    # the events, as rules through every lane, named at the top
    for us, name in ((RX_START, 'RX_START'), (AMI, 'AMI'), (TRX_END, 'TRX_END'),
                     (ACK_DUE, 'Ack due')):
        f.line([(x(us), top - 8), (x(us), bottom + 6)], 'f-tickm')
        f.text(x(us), top - 14, name, 'f-num', 'middle')

    lane(f, y_air, 'on the air', 'the request, then the Ack')
    seg(f, y_air, 0, SHR, 'SHR')
    seg(f, y_air, SHR, RX_START, 'PHR')
    seg(f, y_air, RX_START, AMI, 'MAC header: FCF DSN PAN dst src')
    seg(f, y_air, AMI, AMI + OCTET, 'cmd')
    seg(f, y_air, AMI + OCTET, TRX_END, 'FCS')
    seg(f, y_air, TRX_END, ACK_DUE, 'turnaround', 'f-gap')
    seg(f, y_air, ACK_DUE, SPAN, 'Ack', 'f-brd', 'f-bsub')

    lane(f, y_ami, 'decide at AMI', 'approach 1, early event')
    seg(f, y_ami, AMI, ACK_DUE, '%d µs' % (ACK_DUE - AMI), 'f-amb', 'f-num')
    lane(f, y_end, 'decide at TRX_END', 'approach 1, end of frame')
    seg(f, y_end, TRX_END, ACK_DUE, '%d µs' % (ACK_DUE - TRX_END), 'f-amb', 'f-num')

    lane(f, y_fw, 'this firmware', 'section 5: composes the Ack')
    seg(f, y_fw, STAMP, TX_START, 'the radio vector')
    seg(f, y_fw, TX_START, ACK_DUE, None, 'f-gap')
    f.text(x(TX_START), y_fw + LANE + 14, 'TX_START', 'f-num', 'middle')
    f.text(x(STAMP), y_fw + LANE + 14, 'stamp', 'f-num', 'end')
    f.text(X1, y_fw + LANE + 30, 'abandoned if it cannot start by stamp + %d µs' % (ABANDON - STAMP),
           'f-sub', 'end')

    # the scale
    sy = bottom + 54
    f.line([(X0, sy), (X1, sy)], 'f-rule')
    for us in range(0, SPAN + 1, 20):
        major = us % 100 == 0
        f.line([(x(us), sy), (x(us), sy + (7 if major else 4))], 'f-tickm' if major else 'f-tick')
        if major:
            f.text(x(us), sy + 20, str(us), 'f-axl', 'middle')
    f.text(X0 - S2, sy + 20, 'µs', 'f-axl', 'end')

    ly = f.legend(sy + 52, [('amb', 'the time left to decide Frame Pending'),
                            ('blk', 'a field or a step'), ('gap', 'nothing on the air'),
                            ('brd', 'the Ack')])
    f.h = ly + S4
    return f


if __name__ == '__main__':
    sys.exit(figure.main('ack-timing', build))
