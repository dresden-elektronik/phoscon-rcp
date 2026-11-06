#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""One frame on the host link, at three magnifications, owned by docs/protocol.md section 4.

The HDLC frame on the wire, the Spinel frame it carries, and the header byte inside that; then the
worked example of docs/protocol.md section 3, byte by byte. The TID is the subject, in amber: it is
the one field of the header a host reads on every frame, and the token the TX path turns on.

    python3 tools/diagrams/frame.py            # docs/images/frame.png
    python3 tools/diagrams/frame.py --check
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import figure  # noqa: E402
from figure import D, MX, S1, S2, S4  # noqa: E402

W = 880
FW = W - 2 * MX
BH = 48

WIRE = [('FEND', '0x7E', 64, 'tag'),
        ('Spinel frame, escaped', '0x7E 0x7D 0x11 0x13 go out as 0x7D, byte ^ 0x20', 560, 'blk'),
        ('FCS', 'CRC-16/X.25, low first', 144, 'blk'),
        ('FEND', '0x7E', 64, 'tag')]
SPINEL = [('header', '1 byte', 120, 'blk'),
          ('command', '1 byte: GET, SET, IS ...', 200, 'blk'),
          ('property id', 'packed uint, 1 to 5 bytes', 216, 'blk'),
          ('value', 'as the property\'s type says', 296, 'blk')]
# bit 7 .. bit 0, and the fields they make
BITS = ['1', '0', '0', '0', 't', 't', 't', 't']
FIELDS = [(0, 2, 'FLG', ['always 10, so a', 'header is 0x80..0xBF']),
          (2, 4, 'IID', ['0 on the wire']),
          (4, 8, 'TID', ['0: unsolicited', 'an answer echoes the request\'s TID'])]
# docs/protocol.md section 3: payload 89 06 25 13 -> 7E 89 06 25 7D 33 9B 81 7E
EXAMPLE = ['7E', '89', '06', '25', '7D', '33', '9B', '81', '7E']
SPANS = [(0, 1, 'FEND'), (1, 2, 'header, TID 9'), (2, 3, 'VALUE_IS'), (3, 4, 'property'),
         (4, 6, '0x13, escaped'), (6, 8, 'FCS'), (8, 9, 'FEND')]


def zoom(f, x0, x1, y, X0, X1, Y):
    """The two lines that say: the part above, drawn larger below."""
    f.line([(x0, y), (X0, Y)], 'f-rule')
    f.line([(x1, y), (X1, Y)], 'f-rule')


def build():
    f = figure.Fig(W, 700, 'ONE FRAME ON THE HOST LINK',
                   'HDLC carries Spinel; the first Spinel byte carries the transaction id')
    y = f.top + 14
    f.text(MX, y, 'HDLC, ON THE WIRE', 'f-cap')
    y += S2
    figure.strip(f, MX, y, WIRE, 1, BH, offsets=False)
    px0, px1 = MX + WIRE[0][2], MX + WIRE[0][2] + WIRE[1][2]

    y2 = y + BH + 56
    zoom(f, px0, px1, y + BH, MX, MX + FW, y2 - 22)
    f.text(MX, y2 - 8, 'SPINEL FRAME', 'f-cap')
    figure.strip(f, MX, y2, SPINEL, 1, BH, offsets=False)

    y3 = y2 + BH + 64
    cell = 64
    zoom(f, MX, MX + SPINEL[0][2], y2 + BH, MX, MX + 8 * cell, y3 - 30)
    f.text(MX, y3 - 16, 'HEADER BYTE', 'f-cap')
    for i, b in enumerate(BITS):
        x = MX + i * cell
        f.text(x + cell / 2.0, y3 - S1, str(7 - i), 'f-bit', 'middle')
        kind = 'amb' if i >= 4 else 'blk'
        f.block(x, y3, cell, 36, b, None, kind, center=True)
    fy = y3 + 36 + 14
    for a, b, name, sub in FIELDS:
        x0, x1 = MX + a * cell, MX + b * cell
        f.hbracket(x0 + 2, x1 - 2, fy - 8, None, below=False)
        f.text(x0 + 2, fy + 10, name, 'f-msg')
        for k, ln in enumerate(sub):
            f.text(x0 + 2, fy + 25 + 14 * k, ln, 'f-sub')
            f.fits(ln, 'f-sub', x1 - x0 - S2, 'the field %s' % name)

    # the worked example of section 3, byte by byte, under the header it starts with
    y4 = fy + 84
    ecell = FW / float(len(EXAMPLE))
    f.text(MX, y4 - 16, 'EXAMPLE: THE PAYLOAD 89 06 25 13 ON THE WIRE', 'f-cap')
    for i, b in enumerate(EXAMPLE):
        kind = 'tag' if i in (0, len(EXAMPLE) - 1) else ('amb' if i == 1 else 'blk')
        f.block(MX + i * ecell, y4, ecell, 36, b, None, kind, center=True)
    for a, b, words in SPANS:
        x0, x1 = MX + a * ecell, MX + b * ecell
        f.hbracket(x0 + 3, x1 - 3, y4 + 44, None, below=True)
        f.text((x0 + x1) / 2.0, y4 + 44 + 20, words, 'f-sub', 'middle')
        f.fits(words, 'f-sub', x1 - x0 - S2, 'the example')

    ly = f.legend(y4 + 100, [('amb', 'the transaction id'), ('blk', 'a field'),
                            ('tag', 'the flag byte that opens and closes a frame')])
    f.h = ly + S4
    return f


if __name__ == '__main__':
    sys.exit(figure.main('frame', build))
