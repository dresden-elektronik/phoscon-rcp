#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""Full frames a second between two sticks, owned by docs/stats.md.

One group per sender, receiver and link, two bars in each: CSMA-CA on, in amber, and off. A bar is what
the receiving host is told, and its label is that rate with the MAC payload it carries; where the
receiver's radio acknowledged more than its host was told, the hatched rest of the bar reaches what
the sender saw acknowledged. The figures are measured with tools/rcp_throughput.py on a quiet channel,
127-octet PSDUs, 3 retries, 10 s a run, and are updated in PAIRS when they change.

    python3 tools/diagrams/throughput.py            # docs/images/throughput.png
    python3 tools/diagrams/throughput.py --check
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import figure  # noqa: E402
from figure import MX, S2, S4  # noqa: E402

W = 900
X0, X1 = 232, W - MX           # the labels left, the bars right
BAR, GAP, GROUP = 22, 6, 30
TOP = 200                      # the scale's end, in frames a second
STEP = 50

# one group per pairing and link: sender, receiver, the link, then for CSMA-CA on and off the frames a
# second the receiving host was told, the kbit/s of MAC payload in them, and the frames a second the
# sender saw acknowledged. A ConBee III's rate past 115 200 is set with UART_BITRATE, and its bridge's
# latency timer is 16 ms unless the link says otherwise.
PAIRS = (
    ('ConBee II', 'ConBee II', 'USB CDC at both ends', (108.0, 100.3, 108.1), (125.5, 116.4, 125.5)),
    ('ConBee II', 'ConBee III', 'into UART 115 200', (68.8, 63.9, 110.4), (68.0, 63.1, 128.6)),
    ('ConBee II', 'ConBee III', 'into UART 1 000 000', (110.5, 102.5, 110.7), (128.3, 119.1, 128.5)),
    ('ConBee III', 'ConBee II', 'from UART 115 200', (31.3, 29.0, 31.2), (31.3, 29.0, 31.3)),
    ('ConBee III', 'ConBee II', 'from UART 460 800, 1 ms timer', (87.2, 80.9, 87.2), (97.7, 90.6, 97.7)),
    ('ConBee III', 'ConBee II', 'from UART 1 000 000, 1 ms timer', (89.0, 82.6, 89.0), (110.9, 103.0, 110.9)),
)


def build():
    f = figure.Fig(W, 600, 'FULL FRAMES A SECOND', 'acknowledged 127-octet frames that reach the receiving host')
    scale = (X1 - X0) / float(TOP)
    y = f.top + 14
    for v in range(0, TOP + 1, STEP):
        x = X0 + v * scale
        f.line([(x, y - 4), (x, y)], 'f-tickm')
        f.text(x, y - 8, '%d fps' % v if v == TOP else '%d' % v, 'f-bit', 'end' if v == TOP else 'middle')
    y += S2
    for sender, receiver, link, on, off in PAIRS:
        f.text(MX, y + 15, '%s  →  %s' % (sender, receiver), 'f-lab')
        f.fits(link, 'f-sub', X0 - MX - S2, 'the label column')
        f.text(MX, y + 31, link, 'f-sub')
        for kind, (fps, kbit, acked) in (('f-amb', on), ('f-blk', off)):
            width = fps * scale
            if acked > fps + 1:
                f.rect(X0, y, acked * scale, BAR, 'f-keep', rx=0)
            f.rect(X0, y, width, BAR, kind, rx=0)
            label = '%.1f fps  %s  %.1f kbit/s' % (fps, figure.D, kbit)
            lx = X0 + max(width, acked * scale) + S2
            f.rect(lx, y + 4, figure.textW(label, 'f-num') + 12, BAR - 8, 'f-blk', rx=1)
            f.text(lx + 6, y + 15, label, 'f-num')
            y += BAR + GAP
        y += GROUP - GAP
    ly = f.legend(y + S2, [('amb', 'CSMA-CA on'), ('blk', 'CSMA-CA off'),
                           ('keep', 'acknowledged by the receiver, never reported to its host')])
    f.h = ly + S4
    return f


if __name__ == '__main__':
    sys.exit(figure.main('throughput', build, paths=('tools/rcp_throughput.py',)))
