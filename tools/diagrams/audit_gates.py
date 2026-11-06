#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""The audits a new board target passes before and at its first flash, owned by
docs/testing.md section 5.

Three stages in a fixed order - static, on the bench read-only, the first flash - each a column of
its lettered audits. The gates between them are the subject, in amber: a stage starts only when
the one before it is recorded as passing.

    python3 tools/diagrams/audit_gates.py            # docs/images/audit-gates.png
    python3 tools/diagrams/audit_gates.py --check
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import figure  # noqa: E402
from figure import MX, S2, S4  # noqa: E402

W = 900
RH = 26
STAGES = [
    ('static', 'no board, no flash', [
        ('A', 'register transcription'), ('B', 'guard mutation test'),
        ('C', 'layout arithmetic'), ('D', 'container vs the vendor\'s'),
        ('E', 'pre-flash guard refusals'), ('F', 'link audit of the ELF'),
        ('G', 'vector table identity'), ('H', 'bring-up arithmetic'),
        ('I', 'core unchanged'), ('J', 'what the image may write'),
        ('K', 'radio provenance')]),
    ('bench, read-only', 'an untouched board', [
        ('L', 'what the board is'), ('M', 'what it holds: MAC, PAN, key'),
        ('N', 'the bootloader\'s report'), ('O', 'the cold-boot window')]),
    ('the first flash', 'guarded, recovery written down', [
        ('P', 'what landed: the app CRC'), ('Q', 'M again: identity survived'),
        ('R', 'the DFU route taken'), ('S', 'an interrupted update'),
        ('T', 'the two-node rig')]),
]


def build():
    f = figure.Fig(W, 600, 'BEFORE A NEW BOARD IS FLASHED',
                   'three stages, in order; a stage starts only when the one before is recorded '
                   'as passing')
    gap = 56
    cw = (W - 2 * MX - 2 * gap) / 3.0
    top = f.top + 8
    bottom = top
    for i, (name, sub, audits) in enumerate(STAGES):
        x = MX + i * (cw + gap)
        f.block(x, top, cw, 46, name, sub, 'brt')
        y = top + 46 + S2
        for letter, words in audits:
            f.rect(x, y, cw, RH, 'f-blk', rx=1)
            f.text(x + 10, y + 17, letter, 'f-msg')
            f.text(x + 32, y + 17, words, 'f-sub')
            f.fits(words, 'f-sub', cw - 40, 'the audit %s' % letter)
            y += RH + 4
        bottom = max(bottom, y)
        if i < 2:
            gx = x + cw + 6
            f.arrow([(gx, top + 23), (gx + gap - 12, top + 23)])
            f.rect(x + cw + gap / 2.0 - 7, top + 23 - 7, 14, 14, 'f-now', rx=2)
    ly = f.legend(bottom + 30, [('now', 'a gate: the stage before, recorded as passing'),
                                ('brd', 'a stage'), ('blk', 'an audit')])
    f.h = ly + S4
    return f


if __name__ == '__main__':
    sys.exit(figure.main('audit-gates', build, paths=['tools/cb3_probe.py', 'tools/gcf_check.py']))
