#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""The firmware's layers, owned by docs/architecture.md section 2.

Content only: the style, the renderer and the checks are in figure.py. Every block names the
source it stands for, and the generator refuses to draw when one of those paths is gone, so a run
cannot name a module that is not there. The one block that is not this tree's is the vendor radio
library the ConBee III's rcp image links, drawn hatched.

The figure is a stack read downwards. Amber goes to the portable core only, because that is what
the figure is about: the one run that is the same on every board and runs on the host in the
suites. The contracts are the rules between runs, and below the last one the tree splits by
silicon.

    python3 tools/diagrams/layers.py            # docs/images/layers.png
    python3 tools/diagrams/layers.py --check    # is the committed raster still this figure?
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import figure  # noqa: E402
from figure import D, MX, S2, S3, S4, S5  # noqa: E402

W = 1136
X0, X1 = 168, 960              # where blocks start and end; the gutter names each run
BH, BG = 46, 8
CONTRACT = 56                  # a contract: the air, the rule with its chips, the air
COL_GAP = 16

# (name, plain line, kind, the paths the block stands for)
APP = [
    [('main.c', 'init order %s pump %s trap' % (D, D), 'blk', ['src/app/main.c']),
     ('main_rcp.c', 'the RCP image\'s hooks', 'blk', ['src/app/main_rcp.c']),
     ('main_probe.c', 'the probe image\'s hooks', 'blk', ['src/app/main_probe.c'])],
    [('rcp_app.c', 'the transport seam', 'blk', ['src/app/rcp_app.c']),
     ('flasher_trap.c', 'the flasher\'s reset request', 'blk', ['src/app/flasher_trap.c']),
     ('monitor_app.c', 'the bench monitor', 'blk',
      ['src/app/monitor_app.c', 'src/app/mon_proto.c'])],
]
CORE = [
    [('hdlc', 'flags %s FCS %s resync' % (D, D), 'amb', ['src/rcp/hdlc.c']),
     ('spinel', 'header %s packed uint' % D, 'amb', ['src/rcp/spinel.c']),
     ('session', 'dispatch %s TX completion' % D, 'amb', ['src/rcp/session.c']),
     ('props', 'the property table', 'amb', ['src/rcp/props.c'])],
    [('props_stream', 'STREAM_RAW both ways', 'amb', ['src/rcp/props_stream.c']),
     ('mac', 'header %s Ack decision' % D, 'amb', ['src/rcp/mac.c']),
     ('srcmatch', 'who has a frame pending', 'amb', ['src/rcp/srcmatch.c']),
     ('scan', 'the energy-scan dwell', 'amb', ['src/rcp/scan.c'])],
    [('mac_sec', 'keys %s frame security' % D, 'amb', ['src/rcp/mac_sec.c']),
     ('props_debug', 'the record stream', 'amb', ['src/rcp/props_debug.c'])],
]
SHARED = [
    [('com/usb_cdc.c', 'the USB CDC link', 'blk', ['src/com/usb_cdc.c']),
     ('com/uart.c', 'any UART link', 'blk', ['src/com/uart.c']),
     ('hal/dbg.c', 'the debug-record queue', 'blk', ['src/hal/dbg.c']),
     ('usb/', 'the adopted USB stack', 'blk', ['src/usb'])],
]
# One column per silicon: the prefix is in the column's name, so a block carries the rest of it.
SILICON = [
    dict(name='hal/samr21/', sub='ATSAMR21E18A + AT86RF233', blocks=[
        ('hal_samr21', 'lifecycle %s reset' % D, 'blk', ['src/hal/samr21/hal_samr21.c']),
        ('clock', '48 MHz DFLL', 'blk', ['src/hal/samr21/samr21_clock.c']),
        ('rf', 'RF233 over SPI', 'blk', ['src/hal/samr21/samr21_rf.c']),
        ('radio', 'RX %s TX %s ED %s Ack' % (D, D, D), 'blk', ['src/hal/samr21/samr21_radio.c']),
        ('uart', 'SERCOM5 USART', 'blk', ['src/hal/samr21/samr21_uart.c']),
        ('usb_device', 'the CDC pipes', 'blk', ['src/hal/samr21/samr21_usb_device.c']),
    ], board=('ConBee II  %s  RaspBee II' % D, 'targets cb2 %s rb2' % D,
              ['src/startup/startup_samr21.S', 'src/targets/cb2', 'src/targets/rb2'])),
    dict(name='hal/mg21/', sub='EFR32MG21, radio on chip', blocks=[
        ('hal_mg21', 'lifecycle %s reset' % D, 'blk', ['src/hal/mg21/hal_mg21.c']),
        ('clock', 'the millisecond', 'blk', ['src/hal/mg21/mg21_clock.c']),
        ('own/', 'own sequencer', 'blk', ['src/hal/mg21/own/mg21_radio.c']),
        ('rail/', 'RAIL adapter', 'blk', ['src/hal/mg21/rail/mg21_rail.c']),
        ('uart', 'USART0', 'blk', ['src/hal/mg21/mg21_uart.c']),
        ('librail', 'linked, pinned', 'keep', []),
    ], board=('ConBee III', 'target cb3', ['src/startup/startup_mg21.S', 'src/targets/cb3'])),
    dict(name='hal/avr/', sub='ATmega256RFR2, radio on die', blocks=[
        ('hal_avr', 'lifecycle %s reset' % D, 'blk', ['src/hal/avr/hal_avr.c']),
        ('clock', 'the millisecond', 'blk', ['src/hal/avr/avr_clock.c']),
        ('radio', 'RX %s TX %s ED %s Ack' % (D, D, D), 'blk', ['src/hal/avr/avr_radio.c']),
        ('nvram', 'EEPROM identity', 'blk', ['src/hal/avr/avr_nvram.c']),
        ('uart', 'UART0', 'blk', ['src/hal/avr/avr_uart.c']),
        ('wdt', 'watchdog %s panic' % D, 'blk',
         ['src/hal/avr/avr_wdt.c', 'src/hal/avr/avr_panic.c']),
    ], board=('ConBee I  %s  RaspBee I' % D, 'target cbrb1',
              ['src/startup/startup_avr.S', 'src/targets/cbrb1'])),
]
CONTRACTS = [
    (['app_main.h'], 'App_Boot %s App_DataOut %s App_Poll: one mode file defines them' % (D, D),
     ['src/app/app_main.h']),
    (['hal.h', 'radio.h', 'com.h', 'dbg.h', 'board.h'],
     'the board edge: the core compiles against these, never the silicon',
     ['src/hal/hal.h', 'src/hal/radio.h', 'src/hal/com.h', 'src/hal/dbg.h', 'src/board/board.h']),
    (['uart_drv.h', 'mon_drv.h', 'hal_diag.h'], 'what a silicon layer supplies to the code above',
     ['src/hal/uart_drv.h', 'src/hal/mon_drv.h', 'src/hal/hal_diag.h']),
]
SIDE = ('lib/', 'u_bstream %s u_sstream: every byte and every string' % D,
        ['src/lib/u_bstream', 'src/lib/u_sstream'])


def paths():
    out = []
    for rows in (APP, CORE, SHARED):
        for row in rows:
            for b in row:
                out += b[3]
    for col in SILICON:
        for b in col['blocks']:
            out += b[3]
        out += col['board'][2]
    for c in CONTRACTS:
        out += c[2]
    return out + SIDE[2]


def run(f, y, rows, name, sub):
    f.text(MX, y + 18, name, 'f-lab')
    for k, ln in enumerate(sub):
        f.text(MX, y + 34 + 14 * k, ln, 'f-sub')
    for row in rows:
        n = len(row)
        w = (X1 - X0 - (n - 1) * S2) / float(n)
        for j, (bn, bs, kind, _p) in enumerate(row):
            f.block(X0 + j * (w + S2), y, w, BH, bn, bs, kind)
        y += BH + S2
    return y - S2


def contract(f, y, chips, detail):
    mid = y + CONTRACT / 2.0
    x = X0
    for c in chips:
        x += f.chip(x, mid - 12, c) + S1
    f.line([(x + S1, mid), (X1, mid)], 'f-rule')
    f.text(X1, mid - S2, detail, 'f-sub', 'end')
    return y + CONTRACT


S1 = figure.S1


def build():
    f = figure.Fig(W, 900, 'LAYERS: ONE TREE, FOUR TARGETS',
                   'calls go down; events come back up through a contract')
    y = top = f.top + S2
    y = run(f, y, APP, 'app/', ['the two images,', 'and the transport'])
    y = contract(f, y, *CONTRACTS[0][:2])
    y = run(f, y, CORE, 'rcp/', ['the portable core:', 'no register,', 'no SoC header'])
    y = contract(f, y, *CONTRACTS[1][:2])
    y = run(f, y, SHARED, 'com/ %s hal/' % D, ['shared by every', 'silicon'])
    y = contract(f, y, *CONTRACTS[2][:2])

    f.text(MX, y + 40, 'hal/<soc>/', 'f-lab')
    for k, ln in enumerate(['one per silicon:', 'the only code that', 'touches a register']):
        f.text(MX, y + 56 + 14 * k, ln, 'f-sub')
    cw = (X1 - X0 - 2 * COL_GAP) / 3.0
    bottom = y
    for i, col in enumerate(SILICON):
        cx = X0 + i * (cw + COL_GAP)
        f.group(cx, y + 12, cw, col['name'], col['sub'])
        by = y + 28
        bw = (cw - S2) / 2.0
        for k, (bn, bs, kind, _p) in enumerate(col['blocks']):
            f.block(cx + (k % 2) * (bw + S2), by + (k // 2) * (BH + S2), bw, BH, bn, bs, kind)
        by += 3 * (BH + S2) + S1
        name, sub, _p = col['board']
        f.block(cx, by, cw, 46, name, sub, 'brt')
        bottom = by + 46

    # the substrate stands beside every run, because every run writes through it
    sx = X1 + S5
    f.rect(sx, top, 40, bottom - top, 'f-blk')
    f.text(sx + 25, (top + bottom) / 2.0, '%s   %s' % SIDE[:2], 'f-id', 'middle', rotate=-90)
    ax = sx + 40 + S5
    f.line([(ax, top), (ax, bottom)], 'f-out', 'ink')
    f.text(ax + S2, (top + bottom) / 2.0, 'CALLS GO DOWN', 'f-cap', 'middle', rotate=90)
    ax += 36
    f.line([(ax, bottom), (ax, top)], 'f-out', 'ink')
    f.text(ax + S2, (top + bottom) / 2.0, 'EVENTS COME BACK UP', 'f-cap', 'middle', rotate=90)
    if ax + S2 + 12 > W - MX:
        raise SystemExit('layers: the arrows run past the page')

    ly = f.legend(bottom + 36, [('amb', 'the portable core, the same on every board'),
                                ('blk', 'a module'), ('tag', 'a contract header'),
                                ('keep', 'linked, not in the tree'), ('brd', 'the boards')])
    f.h = ly + 16
    return f


if __name__ == '__main__':
    sys.exit(figure.main('layers', build, paths=paths(), calls=True))
