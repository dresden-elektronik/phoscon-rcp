#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""Shared vocabulary, renderer and checks for the figures in docs/images/.

A figure is a program's output, never a drawing tool's document: every label is a string in a
figure script, so a change to a figure is a change that shows up in a diff. This module owns
everything that is not content - the drawing primitives, the layouts several figures share
(a sequence, a byte strip, a scale), the render pipeline and the checks - and the style is the
Phoscon design language's figure classes in phoscon.css, so a figure names roles (`f-blk`,
`f-out`) and never a colour. A figure script supplies content only.

    python3 tools/diagrams/layers.py                 # writes docs/images/layers.png
    python3 tools/diagrams/layers.py --scale 3       # a 3x raster of the same layout
    python3 tools/diagrams/layers.py --svg /tmp/x.html --no-render
    python3 tools/diagrams/layers.py --names         # the tree checks alone, no renderer
    python3 tools/diagrams/layers.py --check         # is the committed raster current?
    python3 tools/diagrams/all.py [--check]          # every figure

The pipeline is SVG -> an HTML page sized to the figure -> a headless Chrome screenshot at the
figure's scale. Output is deterministic, so a rebuild of an unchanged source is a byte-identical PNG,
which is what --check relies on.

The conventions a figure follows are stated where the figures are referenced:
CONTRIBUTING.md §4.1.
"""

import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
CSS = os.path.join(HERE, 'phoscon.css')

D = '·'                   # the separator inside one plain line: a · b · c

# One spacing scale, the design language's 8 px base.
S1, S2, S3, S4, S5 = 4, 8, 12, 16, 24
MX = 24                        # the margin to the page edge

# What each text class is set in: (face, size px, weight, tracking em). textW() and measure() read it.
FACE = {
    'f-id': ('mono', 13), 'f-aid': ('mono', 13), 'f-bid': ('mono', 13),
    'f-msg': ('mono', 12), 'f-lab': ('mono', 12), 'f-title': ('mono', 12, 0.08),
    'f-cap': ('mono', 10.5, 0.08), 'f-num': ('mono', 11), 'f-axl': ('mono', 11),
    'f-bit': ('mono', 10), 'f-err': ('mono', 11), 'f-ok': ('mono', 11),
    'f-sub': ('sans', 11), 'f-asub': ('sans', 11), 'f-bsub': ('sans', 11),
    'f-lead': ('sans', 12), 'f-head': ('sans', 13, 0, 'bold'), 'f-brt': ('sans', 13, 0, 'bold'),
}

# A block's kind picks its fill class and the two text classes set on it.
KIND = {
    'blk': ('f-blk', 'f-id', 'f-sub'),     # in the tree
    'amb': ('f-amb', 'f-aid', 'f-asub'),   # the subject of the figure
    'owe': ('f-owe', 'f-id', 'f-sub'),     # owed: drawn, not built
    'tag': ('f-tag', 'f-id', 'f-sub'),     # a contract, a header, an outside party
    'brd': ('f-brd', 'f-bid', 'f-bsub'),   # hardware
    'brt': ('f-brd', 'f-brt', 'f-bsub'),   # hardware named in prose (a board's name)
    'gap': ('f-gap', 'f-id', 'f-sub'),     # repeated, elided, or not to scale
    'keep': ('f-keep', 'f-id', 'f-sub'),   # must survive: not this firmware's to write
    'hd': ('f-hd', 'f-head', 'f-sub'),     # a participant's head
}


def esc(s):
    return s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


def textW(s, cls):
    """A layout estimate of a rendered line's width, from the faces' measured averages.

    It places things (a chip's width, a legend's next swatch). Whether a label fits is not
    estimated: every label is measured in the renderer before a figure is drawn (measure()).
    """
    spec = FACE[cls]
    face, size = spec[0], spec[1]
    track = spec[2] if len(spec) > 2 else 0
    per = 0.602 if face == 'mono' else (0.50 if len(spec) > 3 else 0.48)
    return len(s) * size * (per + track)


class Fig(object):
    """One figure: a page of w x h px, drawn top-down with the design language's classes."""

    def __init__(self, w, h, title=None, lead=None):
        self.w, self.h = w, h
        self.parts = []
        self.fit = []              # (text, class, room, where): measured before the figure draws
        self.top = MX
        if title:
            self.text(MX, MX + 10, title, 'f-title')
            self.top += 18
            if lead:
                self.text(MX, MX + 30, lead, 'f-lead')
                self.top += 20
            self.top += S4

    def fits(self, s, cls, room, where):
        self.fit.append((s, cls, room, where))

    # ---------------------------------------------------------------- primitives

    def raw(self, s):
        self.parts.append(s)

    def text(self, x, y, s, cls, anchor='start', rotate=None):
        tr = ''
        if rotate is not None:
            tr = ' transform="rotate(%g %g %g)"' % (rotate, x, y)
        a = '' if anchor == 'start' else ' text-anchor="%s"' % anchor
        self.raw('<text class="%s" x="%g" y="%g"%s%s>%s</text>' % (cls, x, y, a, tr, esc(s)))

    def rect(self, x, y, w, h, cls, rx=2):
        self.raw('<rect class="%s" x="%g" y="%g" width="%g" height="%g" rx="%g"/>'
                 % (cls, x, y, w, h, rx))

    def line(self, pts, cls='f-out', head=None, tail=False):
        """A polyline; head='ink' or 'line' puts an arrowhead at the end, tail at the start too."""
        d = 'M%g %g ' % pts[0] + ' '.join('L%g %g' % p for p in pts[1:])
        m = ''
        if head:
            m = ' marker-end="url(#a-%s)"' % head
            if tail:
                m += ' marker-start="url(#a-%s-r)"' % head
        self.raw('<path class="%s" d="%s"%s/>' % (cls, d, m))

    def arrow(self, pts, back=False):
        """A call (solid ink) or, with back, an answer (dashed line colour)."""
        self.line(pts, 'f-back' if back else 'f-out', 'line' if back else 'ink')

    # ---------------------------------------------------------------- blocks

    def block(self, x, y, w, h, name, sub=None, kind='blk', center=False):
        """A block: the kind's fill, its name in the identifier voice, one plain line under it."""
        box, tn, ts = KIND[kind]
        pad = S3 if not center else S1
        self.fits(name, tn, w - 2 * pad, 'its block')
        if sub:
            self.fits(sub, ts, w - 2 * pad, 'the block %r' % name)
        if kind == 'owe':
            self.rect(x + 0.75, y + 0.75, w - 1.5, h - 1.5, box)
        else:
            self.rect(x, y, w, h, box)
        tx, anchor = (x + w / 2.0, 'middle') if center else (x + pad, 'start')
        if sub:
            self.text(tx, y + h / 2.0 - 3, name, tn, anchor)
            self.text(tx, y + h / 2.0 + 13, sub, ts, anchor)
        else:
            self.text(tx, y + h / 2.0 + 4.5, name, tn, anchor)

    def chip(self, x, y, name, h=24):
        """A contract's name on an ink outline; returns its width."""
        w = textW(name, 'f-id') + 2 * S2
        self.rect(x, y, w, h, 'f-tag', rx=1)
        self.text(x + S2, y + h / 2.0 + 4.5, name, 'f-id')
        return w

    def group(self, x, y, w, name, sub=None):
        """A group is named by a label and a rule, never by a box drawn around it."""
        self.text(x, y, name, 'f-lab')
        if sub:
            self.text(x + textW(name, 'f-lab') + S2, y, sub, 'f-sub')
        self.line([(x, y + 7), (x + w, y + 7)], 'f-rule')

    def band(self, x, y, w, h, label=None):
        """A context: a raised tone behind what runs in it, its name in caps at the top left."""
        self.rect(x, y, w, h, 'f-band', rx=3)
        if label:
            self.text(x + S2, y + 14, label, 'f-cap')

    def bracket(self, x, y0, y1, label=None, side='left', cls='f-num'):
        """A measured interval: a square bracket from y0 to y1, its value beside it."""
        k = -6 if side == 'left' else 6
        self.raw('<path class="f-br" d="M%g %g H%g V%g H%g"/>' % (x, y0, x + k, y1, x))
        if label:
            lines = label if isinstance(label, list) else [label]
            mid = (y0 + y1) / 2.0 - (len(lines) - 1) * 6.5 + 4
            for i, ln in enumerate(lines):
                self.text(x + k + (-S1 if side == 'left' else S1), mid + i * 13, ln, cls,
                          'end' if side == 'left' else 'start')

    def hbracket(self, x0, x1, y, label=None, below=False, cls='f-num'):
        """A measured interval along a scale: ticks at both ends, the value centred on it."""
        k = 6 if below else -6
        self.raw('<path class="f-br" d="M%g %g V%g H%g V%g"/>' % (x0, y, y + k, x1, y))
        if label:
            self.text((x0 + x1) / 2.0, y + k + (13 if below else -S1), label, cls, 'middle')

    # ---------------------------------------------------------------- legend

    def legend(self, y, items, x=MX):
        """The key under every figure: (kind, words) pairs on one line, wrapping if they must."""
        cx = x
        for kind, words in items:
            w = 18 + S2 + textW(words, 'f-sub') + S5
            if cx + w > self.w - MX and cx > x:
                cx, y = x, y + 20
            sy = y - 9
            if kind in KIND:
                if kind == 'owe':
                    self.rect(cx + 0.75, sy + 0.75, 16.5, 10.5, KIND[kind][0], rx=1)
                else:
                    self.rect(cx, sy, 18, 12, KIND[kind][0], rx=1)
            elif kind in ('out', 'back'):
                self.line([(cx, sy + 6), (cx + 18, sy + 6)], 'f-' + kind)
            elif kind == 'br':
                self.raw('<path class="f-br" d="M%g %g H%g V%g H%g"/>'
                         % (cx + 9, sy, cx + 5, sy + 12, cx + 9))
            elif kind == 'alt':
                self.rect(cx, sy, 18, 12, 'f-alt', rx=1)
            elif kind == 'band':
                self.rect(cx, sy, 18, 12, 'f-band', rx=1)
            elif kind in ('pass', 'fail', 'none', 'now'):
                self.rect(cx + 3, sy, 12, 12, 'f-' + kind, rx=2)
            else:
                raise SystemExit('figure: no legend swatch for %r' % kind)
            self.text(cx + 18 + S2, y, words, 'f-sub')
            cx += w
        return y

    # ---------------------------------------------------------------- output

    def svg(self):
        defs = (
            '<defs>'
            '<marker id="a-ink" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" '
            'markerHeight="7" orient="auto"><path d="M0 0 L10 5 L0 10 z" class="k-ink"/></marker>'
            '<marker id="a-ink-r" viewBox="0 0 10 10" refX="1" refY="5" markerWidth="7" '
            'markerHeight="7" orient="auto"><path d="M10 0 L0 5 L10 10 z" class="k-ink"/></marker>'
            '<marker id="a-line" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" '
            'markerHeight="7" orient="auto"><path d="M0 0 L10 5 L0 10 z" class="k-line"/></marker>'
            '<marker id="a-line-r" viewBox="0 0 10 10" refX="1" refY="5" markerWidth="7" '
            'markerHeight="7" orient="auto"><path d="M10 0 L0 5 L10 10 z" class="k-line"/></marker>'
            '<pattern id="f-hatch" width="6" height="6" patternUnits="userSpaceOnUse" '
            'patternTransform="rotate(45)"><rect width="6" height="6" fill="#ffffff"/>'
            '<line class="f-hatchl" x1="0" y1="0" x2="0" y2="6"/></pattern>'
            '</defs>')
        return ('<svg class="diag" xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
                'viewBox="0 0 %d %d">%s\n%s\n</svg>'
                % (self.w, self.h, self.w, self.h, defs, '\n'.join(self.parts)))


# ------------------------------------------------------------- layout: sequence

class Seq(object):
    """A sequence: participants across the top, time down the page, one message per step.

    A call is a solid ink arrow, an answer a dashed one in the line colour, so direction reads in
    grayscale too. Messages are added in order; each advances the clock by its own height.
    """

    def __init__(self, fig, heads, y, head_w=150, step=34):
        self.f, self.step = fig, step
        self.x = {}
        self.y0 = y
        for key, (x, name, sub) in heads.items():
            self.x[key] = x
            fig.block(x - head_w / 2.0, y, head_w, 40 if sub else 30, name, sub, 'hd', center=True)
        self.y = y + (40 if any(h[2] for h in heads.values()) else 30) + S5
        self.top = self.y - S5 + 2

    def msg(self, a, b, name, sub=None, back=False, note=None, amber=False):
        """One message from a to b; its name sits above the arrow, a plain line below it."""
        f, xa, xb = self.f, self.x[a], self.x[b]
        right = xb > xa
        y = self.y + 14
        lx = min(xa, xb) + S3 if right else max(xa, xb) - S3
        anchor = 'start' if right else 'end'
        room = abs(xb - xa) - 2 * S3
        f.fits(name, 'f-msg', room, 'the arrow %r' % name)
        f.text(lx, y - 6, name, 'f-msg', anchor)
        f.arrow([(xa + (2 if right else -2), y), (xb - (2 if right else -2), y)], back)
        if amber:
            mx = xb - (30 if right else -30)
            f.rect(mx - 4, y - 7, 8, 14, 'f-now', rx=1)
        h = 14 + S3
        if sub:
            f.fits(sub, 'f-sub', room, 'the arrow %r' % name)
            f.text(lx, y + 15, sub, 'f-sub', anchor)
            h += 12
        if note:
            f.fits(note, 'f-sub', room, 'the arrow %r' % name)
            f.text(max(xa, xb) - S3 if right else min(xa, xb) + S3, y - 6, note, 'f-sub',
                   'end' if right else 'start')
        self.y += max(self.step, h + S2)
        return y

    def self_msg(self, a, name, sub=None, side='right'):
        """A step inside one participant: a short loop out and back, its name beside it."""
        f, x = self.f, self.x[a]
        y = self.y + 8
        k = 28 if side == 'right' else -28
        f.line([(x + (2 if k > 0 else -2), y), (x + k, y), (x + k, y + 14),
                (x + (2 if k > 0 else -2), y + 14)], 'f-out', 'ink')
        tx = x + k + (S2 if k > 0 else -S2)
        anchor = 'start' if k > 0 else 'end'
        f.text(tx, y + 11, name, 'f-msg', anchor)
        if sub:
            f.text(tx, y + 25, sub, 'f-sub', anchor)
        self.y += 14 + S5 + (12 if sub else 0)
        return y

    def note(self, a, b, text, h=24, kind='gap', pad=40):
        """A band across participants a..b: something that repeats, or is elided."""
        f = self.f
        x0, x1 = min(self.x[a], self.x[b]) - pad, max(self.x[a], self.x[b]) + pad
        f.fits(text, 'f-sub', x1 - x0 - 2 * S2, 'the note across %s..%s' % (a, b))
        y = self.y + 2
        f.rect(x0, y, x1 - x0, h, 'f-' + kind, rx=2)
        f.text((x0 + x1) / 2.0, y + h / 2.0 + 4, text, 'f-sub', 'middle')
        self.y += h + S3
        return y

    def frame(self, a, b, y0, label, pad=56):
        """An alternative: a dashed frame around the messages since y0, its condition in caps."""
        f = self.f
        x0, x1 = min(self.x[a], self.x[b]) - pad, max(self.x[a], self.x[b]) + pad
        f.rect(x0, y0, x1 - x0, self.y - y0, 'f-alt', rx=2)
        f.text(x0 + S2, y0 + 14, label, 'f-cap')
        self.y += S2

    def skip(self, dy):
        self.y += dy

    def label(self, text, cls='f-cap', x=None):
        """A phase label across the diagram, set left of the first lifeline."""
        self.f.text(x if x is not None else MX, self.y + 14, text, cls)
        self.y += 22

    def end(self, pad=S3):
        """Draw the lifelines down to where the last message stopped."""
        # the lifelines go under everything drawn so far
        self.f.parts[:0] = ['<path class="f-life" d="M%g %g V%g"/>' % (x, self.top, self.y + pad)
                            for x in self.x.values()]
        self.y += pad
        return self.y


# ------------------------------------------------------------- layout: byte strip

def strip(fig, x, y, fields, unit, h=40, offsets=True, top_label=None):
    """A run of fields left to right, each `units` wide; offsets above, a plain line inside.

    A field is (name, sub, units, kind[, offset label]). The offset label defaults to the running
    byte count; a field whose width is not to scale is drawn with kind 'gap'.
    """
    cx, at = x, 0
    for fld in fields:
        name, sub, units, kind = fld[:4]
        w = units * unit
        fig.block(cx, y, w, h, name, sub, kind, center=True)
        if offsets:
            off = fld[4] if len(fld) > 4 else str(at)
            if off:
                fig.text(cx + 2, y - S1, off, 'f-bit')
        cx += w
        if isinstance(units, (int, float)):
            at += units
    return cx


# ------------------------------------------------------------------- render

def measure(fig):
    """Measure every recorded label in the renderer's own fonts; refuse the figure on an overflow.

    One headless Chrome run sets each label in its class's face on a canvas, so the check is the
    width the PNG will have, not an estimate of it.
    """
    if not fig.fit:
        return []
    fonts = {}
    with open(CSS) as f:
        css = f.read()
    sans = re.search(r'--sans:\s*([^;]+);', css).group(1)
    mono = re.search(r'--mono:\s*([^;]+);', css).group(1)
    for cls, spec in FACE.items():
        weight = '600' if (spec[0] == 'mono' or len(spec) > 3) else '400'
        if cls in ('f-axl', 'f-bit'):
            weight = '400'
        fonts[cls] = ('%s %gpx %s' % (weight, spec[1], mono if spec[0] == 'mono' else sans),
                      (spec[2] if len(spec) > 2 else 0) * spec[1])
    items = [[s, fonts[c][0], fonts[c][1]] for s, c, _r, _w in fig.fit]
    import json
    html = ('<html><body><div id="o"></div><script>const I=%s;'
            'const c=document.createElement("canvas").getContext("2d");'
            'document.getElementById("o").textContent=I.map(([s,f,t])=>{c.font=f;'
            'return (c.measureText(s).width+t*s.length).toFixed(1)}).join(",");'
            '</script></body></html>' % json.dumps(items))
    work = tempfile.mkdtemp(prefix='phoscon-rcp-measure-')
    path = os.path.join(work, 'm.html')
    with open(path, 'w') as f:
        f.write(html)
    dom = subprocess.check_output([findChrome(), '--headless', '--disable-gpu', '--dump-dom',
                                   'file://' + path], stderr=subprocess.DEVNULL).decode()
    shutil.rmtree(work, ignore_errors=True)
    widths = [float(v) for v in re.search(r'id="o">([^<]*)<', dom).group(1).split(',')]
    return ['%r does not fit in %s (%.0f px of room, %.0f set)' % (s, where, room, w)
            for (s, _c, room, where), w in zip(fig.fit, widths) if w > room]


def page(svg, transparent=False):
    with open(CSS) as f:
        css = f.read()
    ground = 'transparent' if transparent else 'var(--page)'
    return ('<!doctype html><html><head><meta charset="utf-8"><style>%s'
            '* { margin:0; padding:0; box-sizing:border-box; }'
            'html,body { background:%s; -webkit-print-color-adjust:exact;'
            ' print-color-adjust:exact; }'
            '</style></head><body>%s</body></html>' % (css, ground, svg))


def findChrome():
    for path in (os.environ.get('CHROME'),
                 '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
                 shutil.which('google-chrome'), shutil.which('chromium')):
        if path and os.path.exists(path):
            return path
    raise SystemExit('no Chrome found: set $CHROME to the binary')


def render(body, out_png, w, h, scale, transparent=False):
    chrome = findChrome()
    work = tempfile.mkdtemp(prefix='phoscon-rcp-figure-')
    html = os.path.join(work, 'figure.html')
    with open(html, 'w') as f:
        f.write(body)
    argv = [chrome, '--headless', '--disable-gpu', '--hide-scrollbars',
            '--window-size=%d,%d' % (w, h), '--force-device-scale-factor=%d' % scale,
            '--screenshot=' + out_png, 'file://' + html]
    if transparent:
        argv.insert(1, '--default-background-color=00000000')
    subprocess.check_call(argv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    shutil.rmtree(work, ignore_errors=True)
    if not os.path.exists(out_png):
        raise SystemExit('no raster produced; the render failed')
    return struct.unpack('>II', open(out_png, 'rb').read(24)[16:24])


# ------------------------------------------------------------------- checks

CALL = re.compile(r'\b(HAL|Radio)_[A-Za-z0-9_]+\s*\(')   # a call, not a type like Radio_Frame
RADIO_CALLERS = ('src/app/rcp_app.c',)                   # the seam that owns the radio contract
RADIO_CALL = re.compile(r'\bRadio_[A-Za-z0-9_]+\s*\(')


def sources(*parts):
    """Every .c, .h and .S under a directory of the tree, as paths relative to the root."""
    out = []
    for base, _, names in os.walk(os.path.join(ROOT, *parts)):
        for n in sorted(names):
            if n.endswith(('.c', '.h', '.S')):
                out.append(os.path.relpath(os.path.join(base, n), ROOT))
    return out


def checkPaths(paths):
    """Every path a figure draws as this tree's must exist; a '!' prefix means it must not."""
    bad = []
    for p in paths:
        absent = p.startswith('!')
        p = p.lstrip('!')
        there = os.path.exists(os.path.join(ROOT, p))
        if absent and there:
            bad.append('%s is drawn as not this firmware\'s, but it is in the tree' % p)
        if not absent and not there:
            bad.append('%s is drawn, but it does not exist' % p)
    return bad


_TREE = []


def checkSymbols(symbols):
    """Every identifier a figure prints - a function, a constant, a property - is in src/."""
    if not _TREE:
        for rel in sources('src'):
            with open(os.path.join(ROOT, rel), errors='replace') as f:
                _TREE.append(f.read())
    blob = '\n'.join(_TREE)
    return ['%s is drawn, but src/ never names it' % s for s in symbols
            if not re.search(r'(?<![A-Za-z0-9_])%s(?![A-Za-z0-9_])' % re.escape(s), blob)]


def checkCallDirection():
    """The rule the layer figure draws: the core never calls the glue, and one seam owns the radio."""
    bad = []
    for rel in sources('src', 'rcp'):
        with open(os.path.join(ROOT, rel)) as f:
            for line in f:
                if CALL.search(line) and not line.lstrip().startswith('*'):
                    bad.append('%s calls the glue (%s)' % (rel, line.strip()[:60]))
    for rel in sources('src'):
        if rel.startswith('src/hal/'):
            continue
        with open(os.path.join(ROOT, rel)) as f:
            if any(RADIO_CALL.search(l) for l in f) and rel not in RADIO_CALLERS:
                bad.append('%s calls the radio contract but is not the seam' % rel)
    return bad


# ---------------------------------------------------------------------- cli

USAGE = ('usage: <figure>.py [--out PNG] [--svg HTML] [--scale N] [--no-render] '
         '[--names] [--check]')


def main(name, build, paths=(), symbols=(), calls=False, scale=2, transparent=False):
    """The CLI every figure gets: same options, same defaults, same checks.

    `build` returns a Fig. `paths` and `symbols` are what the figure claims about the tree;
    `calls` adds the call-direction rule for a figure that draws it, and `scale` is its raster's
    pixels per point unless --scale says otherwise. A `transparent` figure paints no page ground.
    """
    out = os.path.join(ROOT, 'docs', 'images', name + '.png')
    svg_path, do_render = None, True
    names_only, check = False, False

    argv = sys.argv[1:]
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == '--out':
            i += 1
            out = argv[i]
        elif a == '--svg':
            i += 1
            svg_path = argv[i]
        elif a == '--scale':
            i += 1
            scale = int(argv[i])
        elif a == '--no-render':
            do_render = False
        elif a == '--names':
            names_only = True
        elif a == '--check':
            check = True
        else:
            raise SystemExit(USAGE)
        i += 1

    bad = checkPaths(paths) + checkSymbols(symbols) + (checkCallDirection() if calls else [])
    if bad:
        raise SystemExit('%s: the figure no longer matches the tree:\n  %s'
                         % (name, '\n  '.join(bad)))
    if names_only:
        print('%s: names and claims match the tree' % name)
        return 0

    fig = build()
    bad = measure(fig)
    if bad:
        raise SystemExit('%s: a label overflows:\n  %s' % (name, '\n  '.join(bad)))
    page_html = page(fig.svg(), transparent)
    if svg_path:
        with open(svg_path, 'w') as f:
            f.write(page_html)
        print('wrote %s' % svg_path)
    if check:
        work = tempfile.mkdtemp(prefix='phoscon-rcp-check-')
        fresh = os.path.join(work, name + '.png')
        render(page_html, fresh, fig.w, fig.h, scale, transparent)
        same = os.path.exists(out) and open(fresh, 'rb').read() == open(out, 'rb').read()
        shutil.rmtree(work, ignore_errors=True)
        if not same:
            raise SystemExit('%s: docs/images/%s.png is stale - rebuild it' % (name, name))
        print('%s: %s.png is current' % (name, name))
        return 0
    if not do_render:
        return 0
    w, h = render(page_html, out, fig.w, fig.h, scale, transparent)
    print('wrote %s (%dx%d, %d KB)' % (os.path.relpath(out, ROOT), w, h,
                                        os.path.getsize(out) // 1024))
    return 0
