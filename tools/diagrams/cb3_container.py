#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""The ConBee III's nested GCF container, owned by docs/build-and-flash.md section 10.

The outer GCF header, and the container it carries, magnified: the product magic - the subject, in
amber, because the bootloader installs nothing whose magic is not its own DeviceID - the size, the
image header, the AES image and the CRC-32 tail. Field offsets are the ones tools/gcf_check.py
reads. Not to scale.

    python3 tools/diagrams/cb3_container.py            # docs/images/cb3-container.png
    python3 tools/diagrams/cb3_container.py --check
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import figure  # noqa: E402
from figure import D, MX, S4  # noqa: E402

W = 900
FW = W - 2 * MX
BH = 50

OUTER = [('GCF header', 'type 60 %s target 0x4000 %s CRC-8' % (D, D), 300, 'tag', '0'),
         ('the inner container', 'the payload; the outer CRC-8 covers it', FW - 300, 'blk', '14')]
INNER = [('magic', '0xDEC0DE03', 112, 'amb', '+0'),
         ('size', 'u32', 80, 'blk', '+4'),
         ('image header', 'size %s type 61 %s target %s plain size' % (D, D, D), 300, 'blk', '+8'),
         ('the app', 'AES-256-CBC, key 1', FW - 112 - 80 - 300 - 120, 'gap', '+28'),
         ('CRC-32', 'over all before it', 120, 'blk', 'end − 4')]


def build():
    f = figure.Fig(W, 400, 'THE CONBEE III CONTAINER',
                   'a GCF container that carries a container; the bootloader checks both layers')
    y = f.top + 34
    f.text(MX, y - 20, 'OUTER', 'f-cap')
    figure.strip(f, MX, y, OUTER, 1, BH)
    y2 = y + BH + 72
    f.line([(MX + 300, y + BH), (MX, y2 - 36)], 'f-rule')
    f.line([(MX + FW, y + BH), (MX + FW, y2 - 36)], 'f-rule')
    f.text(MX, y2 - 20, 'INNER: THE PAYLOAD, STORED IN PLAIN BUT FOR THE APP', 'f-cap')
    figure.strip(f, MX, y2, INNER, 1, BH)
    ly = f.legend(y2 + BH + 34, [('amb', 'must equal the stick\'s DeviceID'),
                                 ('blk', 'a field'), ('tag', 'the GCF header every board has'),
                                 ('gap', 'the image; not to scale')])
    f.h = ly + S4
    return f


if __name__ == '__main__':
    sys.exit(figure.main('cb3-container', build, paths=['tools/gcf_check.py']))
