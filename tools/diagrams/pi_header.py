#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""The Raspberry Pi header a RaspBee sits on, owned by docs/build-and-flash.md section 5.1.

The first twenty pins of the 40-pin header with the three the link uses in amber - the UART on
GPIO14/GPIO15 and the module reset on GPIO17 - and, beside them, which of the SoC's UARTs those two
pins carry on each Pi model, because that decides the port to name.

    python3 tools/diagrams/pi_header.py            # docs/images/pi-header.png
    python3 tools/diagrams/pi_header.py --check
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import figure  # noqa: E402
from figure import MX, S2, S4  # noqa: E402

W = 900
PIN = 34
# the header's first twenty pins, by number, with their usual names
PINS = {1: '3V3', 2: '5V', 3: 'GPIO2', 4: '5V', 5: 'GPIO3', 6: 'GND', 7: 'GPIO4',
        8: 'GPIO14', 9: 'GND', 10: 'GPIO15', 11: 'GPIO17', 12: 'GPIO18', 13: 'GPIO27',
        14: 'GND', 15: 'GPIO22', 16: 'GPIO23', 17: '3V3', 18: 'GPIO24', 19: 'GPIO10', 20: 'GND'}
USED = (8, 10, 11)


def build():
    f = figure.Fig(W, 480, 'THE PI HEADER A RASPBEE SITS ON',
                   'three pins carry the link; which UART is behind GPIO14 and GPIO15 depends on '
                   'the Pi')
    x0 = MX + 96
    yt = f.top + 74                # the even row
    yb = yt + PIN + 6              # the odd row
    f.text(MX, yt + PIN / 2.0 + 4, 'even pins', 'f-sub')
    f.text(MX, yb + PIN / 2.0 + 4, 'odd pins', 'f-sub')
    f.rect(x0 - 8, yt - 8, 10 * (PIN + 6) + 10, 2 * PIN + 22, 'f-brd', rx=3)
    for k in range(10):
        odd, even = 2 * k + 1, 2 * k + 2
        x = x0 + k * (PIN + 6)
        for n, y in ((even, yt), (odd, yb)):
            amb = n in USED
            f.rect(x, y, PIN, PIN, 'f-amb' if amb else 'f-blk', rx=PIN / 2.0)
            f.text(x + PIN / 2.0, y + PIN / 2.0 + 4, str(n), 'f-aid' if amb else 'f-num', 'middle')
        # names: the even row's above, the odd row's below
        f.text(x + PIN / 2.0 + 4, yt - 14, PINS[even], 'f-bit', 'start', rotate=-90)
        f.text(x + PIN / 2.0 - 4, yb + PIN + 14, PINS[odd], 'f-bit', 'end', rotate=-90)

    # what the three amber pins carry
    ly = yb + PIN + 76
    lx = x0 + 10 * (PIN + 6) + 40
    rows = [(8, 'GPIO14  TXD', 'the Pi transmits, the module receives'),
            (10, 'GPIO15  RXD', 'the module transmits, the Pi receives'),
            (11, 'GPIO17  reset', 'driven low for 120 ms: the module resets')]
    for i, (n, name, sub) in enumerate(rows):
        y = f.top + 40 + i * 50
        f.block(lx, y, W - MX - lx, 42, name, sub, 'amb')
    f.text(lx, f.top + 26, 'THE LINK', 'f-cap')

    # which UART the two UART pins carry
    ty = f.top + 220
    f.text(lx, ty, 'BEHIND GPIO14 AND GPIO15', 'f-cap')
    models = [('Raspberry Pi 5', '/dev/ttyAMA0', 'pinctrl names them TXD0 / RXD0'),
              ('Pi 3, Zero W, Bluetooth on', '/dev/ttyS0', 'the mini UART: TXD1 / RXD1')]
    for i, (model, port, sub) in enumerate(models):
        y = ty + 14 + i * 58
        f.rect(lx, y, W - MX - lx, 50, 'f-blk')
        f.text(lx + 12, y + 19, model, 'f-lab')
        f.text(W - MX - 12, y + 19, port, 'f-id', 'end')
        f.text(lx + 12, y + 37, sub, 'f-sub')
    f.text(lx, ty + 14 + 2 * 58 + 10, 'name the port: /dev/serial0 can point elsewhere', 'f-lab')

    ly = f.legend(max(ly, ty + 14 + 2 * 58 + 40), [('amb', 'a pin the link uses'),
                                                   ('blk', 'a pin it leaves alone'),
                                                   ('brd', 'the header, pin 1 at the left')])
    f.h = ly + S4
    return f


if __name__ == '__main__':
    sys.exit(figure.main('pi-header', build, paths=['tools/rb2_flash.py']))
