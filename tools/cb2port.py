#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""cb2port.py - resolve the board's CDC port, or refuse.

The boards differ only in their USB serial, so no tool may guess one: a stale default points at a
retired device and the tool then reports nothing useful, which costs a session. Order: --port on the
command line, then $CB2_PORT, then a clear failure.
"""
import os
import sys


def resolve(argv=None):
    argv = sys.argv if argv is None else argv
    if "-h" in argv or "--help" in argv:
        print(sys.modules["__main__"].__doc__)
        raise SystemExit(0)
    for i, arg in enumerate(argv):
        if arg == "--port" and i + 1 < len(argv):
            return argv[i + 1]
        if arg.startswith("--port="):
            return arg.split("=", 1)[1]
    port = os.environ.get("CB2_PORT")
    if port is None:
        raise SystemExit("no port: pass --port <device> or set CB2_PORT (there is no default - "
                         "boards differ only in their USB serial)")
    return port
