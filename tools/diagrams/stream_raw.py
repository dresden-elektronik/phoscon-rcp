#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""The two STREAM_RAW values, owned by docs/protocol.md section 7.

Each value is a length, the PSDU, and a fixed block after it: the TX request's 16-byte trailer and
the RX notification's 26 bytes of metadata. The fixed block is drawn magnified, byte to scale, and
the subject is which of its bytes this firmware actually uses, in amber; the rest is read and
dropped, a constant, or a length prefix. The two block sizes are checked against props_stream.h.

    python3 tools/diagrams/stream_raw.py            # docs/images/stream-raw.png
    python3 tools/diagrams/stream_raw.py --check
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import figure  # noqa: E402
from figure import MX, S2, S4  # noqa: E402

W = 900
FW = W - 2 * MX
H = 36

# (name below, bytes, kind, type letter): amb used, gap read and dropped or constant, tag a prefix
TRAILER = [('txChannel', 1, 'amb', 'C'), ('maxCsmaBackoffs', 1, 'gap', 'C'),
           ('maxRetries', 1, 'amb', 'C'), ('enableCSMACA', 1, 'amb', 'b'),
           ('isHeaderUpdated', 1, 'amb', 'b'), ('isRetransmission', 1, 'gap', 'b'),
           ('isSecurityProcessed', 1, 'amb', 'b'), ('txDelay', 4, 'amb', 'L'),
           ('txDelayBase', 4, 'amb', 'L'), ('rxChannelAfterTx', 1, 'amb', 'C')]
METADATA = [('MD_POWER: RSSI', 1, 'amb', 'c'), ('MD_NOISE: -128', 1, 'gap', 'c'),
            ('MD_FLAG', 2, 'amb', 'S'), ('MD_PHY length', 2, 'tag', 'S'),
            ('channel', 1, 'amb', 'C'), ('LQI', 1, 'amb', 'C'), ('timestamp, µs', 8, 'amb', 'L64'),
            ('MD_VEND length', 2, 'tag', 'S'), ('RX error', 1, 'amb', 'i'),
            ('MD_MAC length', 2, 'tag', 'S'), ('Ack key index', 1, 'amb', 'C'),
            ('Ack frame counter', 4, 'amb', 'L')]


def sizes():
    """The two fixed blocks as props_stream.h states them."""
    with open(os.path.join(figure.ROOT, 'src', 'rcp', 'props_stream.h')) as f:
        h = f.read()
    trailer = int(re.search(r'PROPS_STREAM_TX_TRAILER\s+(\d+)u', h).group(1))
    # the RX comment counts frameDataLen in its total; after the PSDU it is that minus 2
    total = int(re.search(r'MD_MAC \(2 \+ 5\) = (\d+) bytes', h).group(1))
    return trailer, total - 2


def part(f, y, label, fixed_name, fields):
    """One value: the coarse strip, then its fixed block magnified with the field names under it."""
    nfix = sum(b for _n, b, _k, _t in fields)
    f.text(MX, y, label, 'f-cap')
    y += 22
    coarse = [('len', '2 B', 96, 'tag', '0'), ('PSDU', 'n bytes, FCS included', 300, 'blk', '2'),
              (fixed_name, '%d B' % nfix, FW - 396, 'blk', 'n+2')]
    figure.strip(f, MX, y, coarse, 1, H + 8)
    fx0 = MX + 396
    y2 = y + H + 8 + 44
    f.line([(fx0, y + H + 8), (MX, y2 - 14)], 'f-rule')
    f.line([(MX + FW, y + H + 8), (MX + FW, y2 - 14)], 'f-rule')
    unit = FW / float(nfix)
    x, off = MX, 2
    for name, b, kind, letter in fields:
        w = b * unit
        f.block(x, y2, w, H, letter, None, kind, center=True)
        f.text(x + 2, y2 - S1, 'n+%d' % off, 'f-bit')
        f.text(x + w / 2.0 + 4, y2 + H + 8, name, 'f-sub', 'end', rotate=-90)
        x += w
        off += b
    return y2 + H + 8 + max(figure.textW(n, 'f-sub') for n, _b, _k, _t in fields) + 30


S1 = figure.S1


def build():
    trailer, metadata = sizes()
    for fields, want, what in ((TRAILER, trailer, 'trailer'), (METADATA, metadata, 'metadata')):
        have = sum(b for _n, b, _k, _t in fields)
        if have != want:
            raise SystemExit('stream-raw: the %s is drawn as %d bytes, props_stream.h says %d'
                             % (what, have, want))
    f = figure.Fig(W, 900, 'THE TWO STREAM_RAW VALUES',
                   'a length, the PSDU, and a fixed block; amber marks the bytes this firmware uses')
    y = f.top + 12
    y = part(f, y, 'HOST TO RCP: SET STREAM_RAW, THE TX REQUEST', 'trailer', TRAILER)
    y = part(f, y + 10, 'RCP TO HOST: VALUE_IS STREAM_RAW, THE RX NOTIFICATION', 'metadata',
             METADATA)
    ly = f.legend(y, [('amb', 'used by this firmware'),
                      ('gap', 'read and dropped, or a constant'),
                      ('tag', 'a length'), ('blk', 'the frame and the block it is followed by')])
    f.h = ly + S4
    return f


if __name__ == '__main__':
    sys.exit(figure.main('stream-raw', build))
