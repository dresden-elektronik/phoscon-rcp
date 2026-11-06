#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""From a build to a running image, owned by docs/build-and-flash.md section 5.

The host's four steps, and what the board does while the flasher talks to it. The guard is the
subject: the one step a container must pass before anything touches a board.

    python3 tools/diagrams/flash_flow.py            # docs/images/flash-flow.png
    python3 tools/diagrams/flash_flow.py --check
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import figure  # noqa: E402
from figure import MX, S2, S4  # noqa: E402

W = 900
BH = 50
GAP = 66
HOST = [('cmake --build', 'the image', 'blk', 150),
        ('GCFTool -b <board>', 'the container', 'blk', 170),
        ('gcf_check.py', 'the pre-flash guard', 'amb', 150),
        ('GCFFlasher -d <port>', 'talks to the board', 'blk', 182)]
ON_ARROW = ['.bin', '.GCF', 'passed']
PATHS = ['tools/gcf_check.py']


def build():
    f = figure.Fig(W, 520, 'FROM A BUILD TO A RUNNING IMAGE',
                   'nothing reaches a board that the guard has not passed')
    y1 = f.top + 54
    f.text(MX, y1 - 14, 'ON THE HOST', 'f-cap')
    x = MX
    xs = []
    for i, (name, sub, kind, w) in enumerate(HOST):
        f.block(x, y1, w, BH, name, sub, kind)
        xs.append((x, w))
        if i < len(ON_ARROW):
            f.arrow([(x + w + 2, y1 + BH / 2.0), (x + w + GAP - 2, y1 + BH / 2.0)])
            f.text(x + w + GAP / 2.0, y1 + BH / 2.0 - 6, ON_ARROW[i], 'f-msg', 'middle')
        x += w + GAP

    # the refusal: the guard's other exit, above it
    gx, gw = xs[2]
    f.arrow([(gx + gw / 2.0, y1 - 2), (gx + gw / 2.0, y1 - 30)])
    f.text(gx + gw / 2.0 + S2, y1 - 22, 'refused: nothing is flashed', 'f-err')

    y2 = y1 + BH + 110
    f.text(MX, y2 - 14, 'ON THE BOARD', 'f-cap')
    board = [(230, 180, 'running app', 'this firmware or the vendor\'s', 'blk'),
             (506, 140, 'bootloader', 'its start window', 'tag'),
             (W - MX - 140, 140, 'new image', 'after the CRC check', 'blk')]
    for bx, bw, name, sub, kind in board:
        f.block(bx, y2, bw, BH, name, sub, kind)
    words = ['resets into', 'pages, CRC']
    for (ax, aw, _n, _s, _k), (bx, _bw, _n2, _s2, _k2), w in zip(board, board[1:], words):
        f.arrow([(ax + aw + 2, y2 + BH / 2.0), (bx - 2, y2 + BH / 2.0)])
        f.text((ax + aw + bx) / 2.0, y2 + BH / 2.0 - 6, w, 'f-sub', 'middle')

    fx, fw = xs[3]
    yb = y1 + BH
    a1, a2 = fx + 22, fx + 82
    app_x = board[0][0] + board[0][1] / 2.0
    boot_x = board[1][0] + board[1][1] / 2.0
    f.arrow([(a1, yb + 2), (a1, yb + 30), (app_x, yb + 30), (app_x, y2 - 2)])
    f.text(app_x + S2, yb + 24, 'the reset frame', 'f-sub')
    f.arrow([(a2, yb + 2), (a2, yb + 62), (boot_x + 30, yb + 62), (boot_x + 30, y2 - 2)])
    f.text(a2 - S2, yb + 56, 'the container, page by page', 'f-sub', 'end')

    ly = f.legend(y2 + BH + 40, [('amb', 'the guard'), ('blk', 'a step or an image'),
                                 ('tag', 'the bootloader, which a flash never touches'),
                                 ('out', 'what is handed on')])
    f.h = ly + S4
    return f


if __name__ == '__main__':
    sys.exit(figure.main('flash-flow', build, paths=PATHS))
