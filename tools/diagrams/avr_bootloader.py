#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""The ConBee I / RaspBee I bootloader dialog, owned by docs/hardware_avr.md section 11.2.

A sequence between the flasher and the V1.02 bootloader: the reset, the banner, the four key
bytes that must land inside the half-second start window - the subject, in amber - the header,
the page loop and the verdict. The order is GCFFlasher's V1 upload path and the bootloader's own.

    python3 tools/diagrams/avr_bootloader.py            # docs/images/avr-bootloader.png
    python3 tools/diagrams/avr_bootloader.py --check
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import figure  # noqa: E402
from figure import S2, S4  # noqa: E402


def build():
    f = figure.Fig(880, 900, 'THE CONBEE I AND RASPBEE I BOOTLOADER',
                   'the key bytes must arrive within 0.5 s of the reset, or the app starts')
    seq = figure.Seq(f, {'fl': (230, 'GCFFlasher', 'on the host'),
                         'bl': (670, 'bootloader', 'RaspBee Bootloader V1.02')},
                     f.top + S2, head_w=200)
    t0 = seq.msg('fl', 'bl', 'reset', 'FTDI CBUS on a ConBee I, GPIO17 on a RaspBee I')
    seq.msg('bl', 'fl', 'RaspBee Bootloader basic | premium', 'the banner, on every reset',
            back=True)
    t1 = seq.msg('fl', 'bl', '1A 1C  A9 AE', 'unlock, then start the update: one write',
                 amber=True)
    f.bracket(seq.x['fl'] - 24, t0, t1, ['inside', '0.5 s'], side='left')
    seq.msg('bl', 'fl', 'READY', 'its receive buffer flushed', back=True)
    seq.msg('fl', 'bl', 'header, 10 bytes', 'size %s target %s type %s CRC-8' % ((figure.D,) * 3))

    y0 = seq.y
    seq.skip(24)
    seq.msg('bl', 'fl', 'GET <page> ;', 'the page number, u16 little-endian', back=True)
    seq.msg('fl', 'bl', '256 bytes', 'the last page padded with 0xFF')
    seq.frame('fl', 'bl', y0, 'FOR EVERY PAGE: A RESET HERE ABORTS THE UPDATE', pad=140)

    seq.msg('bl', 'fl', '#  VALID CRC 0x..', 'read back from flash, over the size', back=True)
    seq.msg('bl', 'fl', 'STARTING APP', 'and a jump to address 0', back=True)
    y1 = seq.y
    seq.skip(24)
    seq.msg('bl', 'fl', '#  INVALID CRC 0x..', 'page 0 erased: the bootloader stays', back=True)
    seq.frame('fl', 'bl', y1, 'OR, ON A BAD IMAGE', pad=140)
    end = seq.end()

    ly = f.legend(end + 30, [('out', 'flasher to bootloader'), ('back', 'bootloader to flasher'),
                             ('now', 'the keys that open the update'),
                             ('br', 'the start window'), ('alt', 'a loop or an alternative')])
    f.h = ly + S4
    return f


if __name__ == '__main__':
    sys.exit(figure.main('avr-bootloader', build))
