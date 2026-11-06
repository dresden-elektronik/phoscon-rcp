#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""The family map, owned by docs/boards.md section 1.

Five products, the four build targets that cover them, and the three silicon layers under those.
The targets are the subject, in amber: a target is what a build names. Every target and silicon
directory drawn is checked to exist in the tree.

    python3 tools/diagrams/family.py            # docs/images/family.png
    python3 tools/diagrams/family.py --check
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import figure  # noqa: E402
from figure import MX, S4  # noqa: E402

W = 900
BH, ROW = 50, 66
C1, W1 = MX, 260               # products and their host links
C2, W2 = 386, 110              # targets
C3, W3 = 622, W - MX - 622     # silicon layers

# (product, its host link, target)
PRODUCTS = [('ConBee II', 'USB, native CDC', 'cb2'),
            ('RaspBee II', 'Pi header UART, 115 200 baud', 'rb2'),
            ('ConBee III', 'USB through an FTDI bridge, 115 200', 'cb3'),
            ('ConBee I', 'USB through an FTDI bridge, 57 600', 'cbrb1'),
            ('RaspBee I', 'Pi header UART, 57 600 baud', 'cbrb1')]
# target -> silicon layer
SILICON = [('hal/samr21/', 'ATSAMR21E18A + AT86RF233', ['cb2', 'rb2']),
           ('hal/mg21/', 'EFR32MG21, radio on chip', ['cb3']),
           ('hal/avr/', 'ATmega256RFR2, radio on die', ['cbrb1'])]
PATHS = (['src/targets/' + t for t in ('cb2', 'rb2', 'cb3', 'cbrb1')]
         + ['src/' + s[0] for s in SILICON])


def build():
    f = figure.Fig(W, 520, 'ONE TREE, FIVE PRODUCTS',
                   'a build names a target; a target names its silicon layer')
    top = f.top + 28
    for x, words in ((C1, 'PRODUCT AND HOST LINK'), (C2, 'TARGET'), (C3, 'SILICON LAYER')):
        f.text(x, top - 12, words, 'f-cap')

    ymid = {}
    for i, (name, link, _t) in enumerate(PRODUCTS):
        y = top + i * ROW
        f.block(C1, y, W1, BH, name, link, 'brt')
        ymid[name] = y + BH / 2.0

    tmid = {}
    for t in ('cb2', 'rb2', 'cb3', 'cbrb1'):
        ys = [ymid[p] for p, _l, tt in PRODUCTS if tt == t]
        c = sum(ys) / len(ys)
        tmid[t] = c
        f.block(C2, c - BH / 2.0, W2, BH, t, None, 'amb', center=True)
        for y in ys:
            xm = C1 + W1 + 30
            f.line([(C1 + W1, y), (xm, y), (xm, c), (C2 - 2, c)], 'f-out', 'ink')

    for name, sub, targets in SILICON:
        c = sum(tmid[t] for t in targets) / len(targets)
        f.block(C3, c - BH / 2.0, W3, BH, name, sub, 'blk')
        for t in targets:
            xm = C2 + W2 + 40
            f.line([(C2 + W2, tmid[t]), (xm, tmid[t]), (xm, c), (C3 - 2, c)], 'f-out', 'ink')

    ly = f.legend(top + len(PRODUCTS) * ROW + 24,
                  [('brd', 'a product'), ('amb', 'a build target: -DDEBOARD='),
                   ('blk', 'the silicon layer its HAL lives in')])
    f.h = ly + S4
    return f


if __name__ == '__main__':
    sys.exit(figure.main('family', build, paths=PATHS))
