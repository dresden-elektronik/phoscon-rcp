#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""The transmit path, owned by docs/architecture.md section 4.

A sequence: a `SET STREAM_RAW` is not answered when it is accepted, only when the radio reports
the outcome, and the request's TID is what ties the two. The TID is the figure's subject, marked
in amber where it leaves the host and where it comes back.

    python3 tools/diagrams/tx_path.py            # docs/images/tx-path.png
    python3 tools/diagrams/tx_path.py --check
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import figure  # noqa: E402
from figure import D, MX, S2, S4  # noqa: E402

SYMBOLS = ['Rcp_Feed', 'Radio_Tx', 'Radio_Poll', 'Rcp_TxDone']


def build():
    f = figure.Fig(880, 640, 'TX PATH: THE ANSWER WAITS FOR THE RADIO',
                   'a SET STREAM_RAW is answered once, when the frame is out, by a LAST_STATUS '
                   'carrying its TID')
    seq = figure.Seq(f, {
        'host': (196, 'host', 'the network stack'),
        'core': (500, 'session', 'rcp/session.c'),
        'radio': (772, 'radio', 'the silicon layer'),
    }, f.top + S2, head_w=170)
    t0 = seq.msg('host', 'core', 'SET STREAM_RAW  tid 3', 'the PSDU and a trailer',
                 amber=True)
    seq.self_msg('core', 'check', 'enabled, idle, well formed', side='left')
    y0 = seq.y
    seq.skip(18)
    seq.msg('core', 'host', 'VALUE_IS LAST_STATUS  tid 3', 'BUSY, INVALID_STATE or PARSE_ERROR',
            back=True)
    seq.frame('host', 'core', y0, 'IF REFUSED: AT ONCE', pad=40)
    seq.msg('core', 'radio', 'Radio_Tx', 'the PSDU is copied; the call returns')
    seq.skip(4)
    seq.note('core', 'radio', 'Radio_Poll, one register step per loop pass: CCA %s transmit %s '
             'the Ack' % (D, D), h=28, pad=60, kind='band')
    seq.skip(4)
    seq.msg('radio', 'core', 'Rcp_TxDone', 'the outcome, in thread context', back=True)
    t1 = seq.msg('core', 'host', 'VALUE_IS LAST_STATUS  tid 3', 'OK, NO_ACK, CCA_FAILURE ...',
                 back=True, amber=True)
    end = seq.end()
    f.bracket(seq.x['host'] - 64, t0, t1, ['accepted:', 'no answer', 'in between'], side='left',
              cls='f-sub')

    ly = f.legend(end + 30, [('out', 'a request or a call'), ('back', 'an answer or an event'),
                             ('now', 'the TID that ties them'),
                             ('band', 'what the radio does meanwhile'), ('alt', 'an alternative'),
                             ('br', 'the deferred interval')])
    f.h = ly + S4
    return f


if __name__ == '__main__':
    sys.exit(figure.main('tx-path', build, symbols=SYMBOLS))
