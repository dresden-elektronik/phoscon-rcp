#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""Join hard-wrapped Markdown prose into one line per paragraph or list item.

Tables, headings, code fences and blank lines are left alone. List-item
continuation lines are merged into their item, nested items stay separate,
and blockquote paragraphs are joined. Run without arguments for a dry run.
"""

import glob
import re
import sys

ITEM = re.compile(r'^(\s*)([-*]\s+|\d+\.\s+)')


def is_table(line):
    return line.lstrip().startswith('|')


def is_heading(line):
    return line.lstrip().startswith('#')


def unwrap(text):
    out = []
    para = []          # prose paragraph being accumulated
    state = {'item': None, 'quote': None}
    fence = False

    def flush_para():
        if para:
            out.append(' '.join(p.strip() for p in para))
            para.clear()

    def flush_item():
        if state['item']:
            indent, prefix, parts = state['item']
            out.append(indent + prefix + ' '.join(p.strip() for p in parts))
            state['item'] = None

    def flush_quote():
        if state['quote']:
            out.append(' '.join(re.sub(r'^>\s?', '', q.strip()) for q in state['quote']))
            state['quote'] = None

    def flush_all():
        flush_para()
        flush_item()
        flush_quote()

    for line in text.split('\n'):
        stripped = line.strip()

        if stripped.startswith('```'):
            flush_all()
            fence = not fence
            out.append(line.rstrip())
            continue

        if fence:
            out.append(line.rstrip())
            continue

        if not stripped:
            flush_all()
            out.append('')
            continue

        if is_heading(line) or is_table(line):
            flush_all()
            out.append(line.rstrip())
            continue

        if stripped.startswith('>'):
            flush_para()
            flush_item()
            if state['quote'] is None:
                state['quote'] = [line.rstrip()]
            else:
                state['quote'].append(line.rstrip())
            continue

        match = ITEM.match(line)
        if match:
            flush_para()
            flush_item()
            flush_quote()
            indent, prefix = match.group(1), match.group(2)
            state['item'] = (indent, prefix, [line[len(indent) + len(prefix):]])
            continue

        if state['item']:
            state['item'][2].append(stripped)
        else:
            para.append(stripped)

    flush_all()
    return '\n'.join(out).rstrip() + '\n'


def main():
    files = sorted(set(glob.glob('*.md') + glob.glob('docs/*.md')
                       + glob.glob('docs/tmp/*.md')
                       + glob.glob('src/**/*.md')))
    if 'refs' in sys.argv:
        sys.exit('refs/ is reference-only')
    dirty = []
    for path in files:
        original = open(path).read()
        fixed = unwrap(original)
        if fixed != original:
            dirty.append(path)
            print(f'{path}: {original.count(chr(10))} -> {fixed.count(chr(10))} lines')
            if '--apply' in sys.argv:
                open(path, 'w').write(fixed)
        else:
            print(f'{path}: already unwrapped')
    if '--check' in sys.argv and dirty:
        sys.exit('wrapped prose in: ' + ' '.join(dirty))


if __name__ == '__main__':
    main()
