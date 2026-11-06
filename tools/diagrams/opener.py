#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""The README's opener: the project's name, what a host does with it, and the boards it runs on.

The one figure that is decoration (CONTRIBUTING.md §4.1): its own palettes and faces, no
ground so it takes the page's, and no margin so it lines up with the README's text. It is written wide
and for a phone, each in a dark and a light theme, for the README's <picture> element to choose from,
at 3x, because GitHub scales an image down to its column and a Retina display then needs about two
pixels per point. The README repeats its text in the alt text.

    python3 tools/diagrams/opener.py            # docs/images/opener[-phone]-{dark,light}.png
    python3 tools/diagrams/opener.py --check    # are the committed rasters still these figures?
"""

import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import figure  # noqa: E402

WIDE, PHONE = 880, 480
SCALE = 3
Palette = collections.namedtuple('Palette', 'ink dim line')
# GitHub's own text, muted text and border colours, so the figure belongs to the page in either theme
THEMES = (('dark', Palette('#f0f6fc', '#9198a1', '#3d444d')),
          ('light', Palette('#1f2328', '#59636e', '#d1d9e0')))
DISPLAY = "'DIN Condensed', 'Bahnschrift Condensed', 'Arial Narrow', sans-serif"
NAME = "'DIN Alternate', 'Bahnschrift', Arial, sans-serif"
BODY = "'Avenir Next', 'Segoe UI', Helvetica, sans-serif"

LINES = ('The host runs the network stack;', 'the adapter implements PHY and MAC only.')
HOSTS = (
    ('Zigbee', ('Coordinator or router, with the stack on the host.',)),
    ('Thread 1.4', ('Matter over Thread, with Home Assistant.',)),
    ('802.15.4 sniffer', ('Promiscuous receive with the raw frame stream.',)),
)
HOSTS_PHONE = (
    ('Zigbee', ('Coordinator or router, with the stack on the host.',)),
    ('Thread 1.4', ('Matter over Thread, with Home Assistant.',)),
    ('802.15.4 sniffer', ('Promiscuous receive with the raw frame stream.',)),
)
BOARDS = ('ConBee III', 'ConBee II', 'ConBee I', 'RaspBee II', 'RaspBee I')


def text(f, x, y, s, family, size, fill, weight=400):
    f.raw('<text x="%g" y="%g" style="font-family:%s;font-size:%gpx;font-weight:%d;fill:%s">%s</text>'
          % (x, y, family, size, weight, fill, figure.esc(s)))


def hairline(f, x, y, w, fill):
    f.raw('<rect x="%g" y="%g" width="%g" height="1" style="fill:%s"/>' % (x, y, w, fill))


def wide(p):
    """The name on the left, and to its right a table on one grid: what a host does, and the boards."""
    f = figure.Fig(WIDE, 236)
    text(f, -2, 78, 'Phoscon', DISPLAY, 104, p.ink, 700)
    text(f, -2, 166, 'RCP', DISPLAY, 104, p.ink, 700)
    text(f, 0, 200, LINES[0], BODY, 14.5, p.dim)
    text(f, 0, 222, LINES[1], BODY, 14.5, p.dim)

    left, label, top, row = 326, 132, 5, 56
    right = left + label
    rows = [(name, lines[0]) for name, lines in HOSTS]
    for i in range(len(rows) + 2):
        hairline(f, left, top + i * row, WIDE - left, p.line)
    for i, (name, line) in enumerate(rows):
        y = top + i * row + row / 2.0 + 6
        text(f, left, y, name, NAME, 18, p.ink, 700)
        text(f, right, y, line, BODY, 15, p.dim)
    y = top + 3 * row + row / 2.0 + 6
    text(f, left, y, 'Boards', NAME, 18, p.ink, 700)
    step = (WIDE - right) / float(len(BOARDS))
    for i, name in enumerate(BOARDS):
        text(f, right + i * step, y, name, BODY, 15, p.dim)
    return f


def phone(p):
    """The same table, with each row's label over its text."""
    f = figure.Fig(PHONE, 522)
    text(f, -3, 84, 'Phoscon RCP', DISPLAY, 113, p.ink, 700)
    text(f, 0, 118, LINES[0], BODY, 20, p.dim)
    text(f, 0, 145, LINES[1], BODY, 20, p.dim)
    y = 172
    for name, lines in HOSTS_PHONE:
        hairline(f, 0, y, PHONE, p.line)
        text(f, 0, y + 36, name, NAME, 26, p.ink, 700)
        for n, line in enumerate(lines):
            text(f, 0, y + 64 + n * 25, line, BODY, 19, p.dim)
        y += 56 + 25 * len(lines)
    hairline(f, 0, y, PHONE, p.line)
    text(f, 0, y + 36, 'Boards', NAME, 26, p.ink, 700)
    for i, name in enumerate(BOARDS):
        text(f, (i % 3) * 160, y + 64 + (i // 3) * 25, name, BODY, 19, p.dim)
    y += 56 + 50
    hairline(f, 0, y, PHONE, p.line)
    return f


if __name__ == '__main__':
    failed = 0
    for suffix, layout in (('', wide), ('-phone', phone)):
        for theme, palette in THEMES:
            failed |= figure.main('opener%s-%s' % (suffix, theme),
                                  lambda layout=layout, palette=palette: layout(palette),
                                  scale=SCALE, transparent=True)
    sys.exit(failed)
