#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""The host's startup sequence, owned by docs/protocol.md section 6.

What the reference host sends after it opens the link, and what the RCP answers: the identity
queries, which the RCP answers from power-on, the reset, and the two ways on from there - a
coordinator or a sniffer. The one unsolicited answer, the reset's LAST_STATUS with TID 0, is the
subject. Every property drawn is checked against the property table in src/.

    python3 tools/diagrams/startup.py            # docs/images/startup.png
    python3 tools/diagrams/startup.py --check
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import figure  # noqa: E402
from figure import D, S2, S4  # noqa: E402

# the Spinel name the figure prints, and the id the property table spells it with
PROPS = {'PROTOCOL_VERSION': 'PROTOCOL_VERSION', 'NCP_VERSION': 'NCP_VERSION',
         'INTERFACE_TYPE': 'INTERFACE_TYPE', 'RCP_API_VERSION': 'RCP_API_VERSION',
         'RCP_MIN_HOST_API_VERSION': 'RCP_MIN_HOST_API', 'LAST_STATUS': 'LAST_STATUS',
         'PHY_ENABLED': 'PHY_ENABLED', 'PHY_CHAN': 'PHY_CHAN', 'PHY_TX_POWER': 'PHY_TX_POWER',
         'MAC_15_4_LADDR': 'MAC_LADDR', 'MAC_15_4_SADDR': 'MAC_SADDR',
         'MAC_15_4_PANID': 'MAC_PANID', 'MAC_RX_ON_WHEN_IDLE': 'MAC_RX_ON_WHEN_IDLE',
         'MAC_RAW_STREAM_ENABLED': 'MAC_RAW_STREAM', 'MAC_PROMISCUOUS_MODE': 'MAC_PROMISCUOUS',
         'STREAM_RAW': 'STREAM_RAW'}

QUERIES = [('PROTOCOL_VERSION', '4.3'), ('NCP_VERSION', 'DSPINEL/1.0; <radio>; <version>'),
           ('INTERFACE_TYPE', '3, Spinel'), ('RCP_API_VERSION', '4'),
           ('RCP_MIN_HOST_API_VERSION', '4')]


def symbols():
    return ['PROPS_ID_' + p for p in PROPS.values()]


def build():
    f = figure.Fig(880, 1100, 'HOST STARTUP',
                   'identify, reset, then become a coordinator or a sniffer')
    seq = figure.Seq(f, {'host': (250, 'host', 'Zigbee stack or sniffer'),
                         'rcp': (690, 'RCP', 'this firmware')}, f.top + S2, head_w=200)
    seq.msg('host', 'rcp', '0x7E', 'a bare flag flushes the transport')
    y0 = seq.y + 6
    for prop, answer in QUERIES:
        seq.msg('host', 'rcp', 'GET ' + prop)
        seq.msg('rcp', 'host', 'VALUE_IS  ' + answer, back=True)
    f.bracket(seq.x['host'] - 24, y0, seq.y - 6,
              ['answered from', 'power-on, before', 'the reset'], side='left', cls='f-sub')
    seq.msg('host', 'rcp', 'RESET  2', 'the type is accepted and ignored')
    seq.msg('rcp', 'host', 'VALUE_IS LAST_STATUS = 114  tid 0', 'RESET_SOFTWARE, unsolicited',
            back=True, amber=True)

    y1 = seq.y
    seq.skip(24)
    seq.msg('host', 'rcp', 'SET PHY_ENABLED 1 %s PHY_CHAN %s PHY_TX_POWER' % (D, D))
    seq.msg('host', 'rcp', 'SET MAC_15_4_LADDR %s _SADDR %s _PANID' % (D, D))
    seq.msg('host', 'rcp', 'SET MAC_RX_ON_WHEN_IDLE %s MAC_RAW_STREAM_ENABLED' % D,
            'each SET is answered by VALUE_IS')
    seq.frame('host', 'rcp', y1, 'TO FORM A NETWORK', pad=150)

    y2 = seq.y
    seq.skip(24)
    seq.msg('host', 'rcp', 'SET PHY_ENABLED 1 %s PHY_CHAN' % D)
    seq.msg('host', 'rcp', 'SET MAC_PROMISCUOUS_MODE 2', 'FULL: every frame on the channel')
    seq.msg('host', 'rcp', 'SET MAC_RX_ON_WHEN_IDLE %s MAC_RAW_STREAM_ENABLED' % D)
    seq.msg('rcp', 'host', 'VALUE_IS STREAM_RAW  tid 0', 'every frame heard, unsolicited',
            back=True)
    seq.frame('host', 'rcp', y2, 'OR, TO START A SNIFFER', pad=150)
    end = seq.end()

    ly = f.legend(end + 30, [('out', 'host to RCP'), ('back', 'RCP to host'),
                             ('now', 'the one unsolicited reset answer'),
                             ('alt', 'one way on from the reset'), ('br', 'a span of steps')])
    f.h = ly + S4
    return f


if __name__ == '__main__':
    sys.exit(figure.main('startup', build, symbols=symbols()))
