#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""The ConBee II / RaspBee II clock tree, owned by docs/hardware_samr21.md section 2.

The board has no crystal for the SoC, so the 48 MHz comes from the radio: its CLKM output, divided
down to a 32 kHz reference, locks the DFLL - the subject, in amber. The bootloader's own regime,
the DFLL recovering its rate from the USB start-of-frame, is drawn as the alternative it is, and
the watchdog runs on its own slow chain.

    python3 tools/diagrams/samr21_clocks.py            # docs/images/samr21-clocks.png
    python3 tools/diagrams/samr21_clocks.py --check
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import figure  # noqa: E402
from figure import MX, S2, S4  # noqa: E402

W = 900
BH = 50
PATHS = ['src/hal/samr21/samr21_clock.c']


def chain(f, y, items, gaps, labels):
    x = MX
    xs = []
    for (name, sub, kind, w), g, lab in zip(items, gaps + [0], labels + ['']):
        f.block(x, y, w, BH, name, sub, kind)
        xs.append((x, w))
        if g:
            f.arrow([(x + w + 2, y + BH / 2.0), (x + w + g - 2, y + BH / 2.0)])
            if lab:
                f.text(x + w + g / 2.0, y + BH / 2.0 - 7, lab, 'f-num', 'middle')
                f.fits(lab, 'f-num', g - 6, 'the arrow %r' % lab)
        x += w + g
    return xs


def build():
    f = figure.Fig(W, 520, 'THE CLOCK TREE OF THE CONBEE II AND RASPBEE II',
                   'no crystal on the SoC: the radio\'s clock output is the reference')
    y = f.top + 24
    f.text(MX, y - 10, 'THE CORE, 48 MHZ', 'f-cap')
    xs = chain(f, y, [('AT86RF233', 'CLKM output', 'brt', 120),
                      ('PC16', 'GCLK_IO1', 'tag', 96),
                      ('gen1', '÷ 250', 'blk', 96),
                      ('DFLL48M', 'closed loop, × 1500', 'amb', 164),
                      ('gen0', 'CPU, SERCOMs, USB', 'blk', 168)],
               [44, 44, 56, 64], ['8 MHz', '', '32 kHz', '48 MHz'])

    # the bootloader's regime: no reference until the host's start-of-frame arrives
    dx, dw = xs[3]
    yb = y + BH + 46
    f.block(dx - 40, yb, dw + 80, 40, 'USB start-of-frame', 'the bootloader\'s regime (USBCRM)',
            'gap')
    f.line([(dx + dw / 2.0, yb - 2), (dx + dw / 2.0, y + BH + 4)], 'f-back', 'line')

    y2 = yb + 40 + 54
    f.text(MX, y2 - 10, 'THE WATCHDOG', 'f-cap')
    chain(f, y2, [('OSCULP32K', 'always on', 'blk', 120),
                  ('gen4', '÷ 16', 'blk', 96),
                  ('WDT', 'channel 3: 8 s', 'blk', 150)],
          [56, 56], ['32 kHz', '2 kHz'])
    f.text(MX + 120 + 56 + 96 + 56 + 150 + S4, y2 + 20, 'the bootloader sets ALWAYSON:', 'f-sub')
    f.text(MX + 120 + 56 + 96 + 56 + 150 + S4, y2 + 34, 'the app can feed it, not stop it',
           'f-sub')

    ly = f.legend(y2 + BH + 34, [('amb', 'the loop that makes the 48 MHz'),
                                 ('blk', 'a generator or a block'),
                                 ('gap', 'the alternative reference'),
                                 ('brd', 'outside the SoC'), ('tag', 'a pad')])
    f.h = ly + S4
    return f


if __name__ == '__main__':
    sys.exit(figure.main('samr21-clocks', build, paths=PATHS))
