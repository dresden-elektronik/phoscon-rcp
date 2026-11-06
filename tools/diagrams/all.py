#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""Build, or with --check verify, every figure in docs/images/ (CONTRIBUTING.md §4.1).

    python3 tools/diagrams/all.py              # rebuild every figure
    python3 tools/diagrams/all.py --check      # is every committed raster current?
    python3 tools/diagrams/all.py --names      # the tree checks alone, no renderer

Each figure script is run as its own process with the same options, so one failing figure is
reported and the rest still run. Exit is non-zero when any figure failed.
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    scripts = sorted(n for n in os.listdir(HERE)
                     if n.endswith('.py') and n not in ('figure.py', 'all.py'))
    failed = []
    for n in scripts:
        res = subprocess.run([sys.executable, os.path.join(HERE, n)] + sys.argv[1:])
        if res.returncode != 0:
            failed.append(n)
    if failed:
        print('failed: %s' % ', '.join(failed))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
