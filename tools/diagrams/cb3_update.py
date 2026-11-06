#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""How a running ConBee III image is updated, owned by docs/hardware_mg21.md section 10.7.

A sequence between GCFFlasher, the running image and the deSiLabs bootloader: the flasher's reset
frame, the image's hand-back and watchdog reset - the subject, in amber, because without the
hand-back the stick goes silent - the bootloader's unsolicited ID, the update request, the page
loop, the install and the check of the app CRC.

    python3 tools/diagrams/cb3_update.py            # docs/images/cb3-update.png
    python3 tools/diagrams/cb3_update.py --check
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import figure  # noqa: E402
from figure import D, S2, S4  # noqa: E402


def build():
    f = figure.Fig(900, 900, 'UPDATING A CONBEE III',
                   'the image hands the board back, then the bootloader takes the container')
    seq = figure.Seq(f, {'fl': (130, 'GCFFlasher', 'on the host'),
                         'app': (450, 'running image', 'this firmware'),
                         'bl': (770, 'bootloader', 'deSiLabs, below 0x4000')},
                     f.top + S2, head_w=200)
    seq.msg('fl', 'app', 'reset frame', 'WRITE_PARAMETER 0x26, the watchdog TTL')
    y = seq.msg('app', 'bl', 'watchdog reset', 'after handing back clocks, the UART, its pads',
                amber=True)
    seq.msg('bl', 'fl', '81 82  version, app CRC', 'unsolicited, about 2.3 s after the frame',
            back=True)
    seq.msg('fl', 'bl', '81 03  update request', 'size %s target %s type 61 %s CRC-32' % (D, D, D))
    seq.msg('bl', 'fl', '81 83  status', None, back=True)
    y0 = seq.y
    seq.skip(24)
    seq.msg('bl', 'fl', '81 04  data request', 'offset, length', back=True)
    seq.msg('fl', 'bl', '81 84  data', 'one 256-byte page')
    seq.frame('fl', 'bl', y0, 'FOR EVERY PAGE', pad=96)
    seq.self_msg('bl', 'install', 'check, erase, decrypt, write', side='left')
    seq.msg('fl', 'bl', '81 02  ID request')
    seq.msg('bl', 'fl', '81 82  app CRC', 'equals the image\'s: app checksum (OK)', back=True)
    end = seq.end()
    ly = f.legend(end + 30, [('out', 'a request or a reset'), ('back', 'an answer'),
                             ('now', 'the hand-back the reset needs'),
                             ('alt', 'a loop')])
    f.h = ly + S4
    return f


if __name__ == '__main__':
    sys.exit(figure.main('cb3-update', build, paths=['src/hal/mg21/hal_mg21.c',
                                                     'src/targets/cb3/flash_layout.cmake']))
