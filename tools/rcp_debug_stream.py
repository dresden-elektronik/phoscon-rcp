#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""rcp_debug_stream.py - read the RCP image's debug records over Spinel.

The records needed a separate build (`-DCB2_RCP_DEBUG_STREAM`) for as long as the RCP
transport owned the IN endpoint. They now travel in-band as `RCP_LOG_CRASH_DUMP` (`0xB2`,
upstream's crash-dump trigger): an empty write enables the stream, a `u8` payload sets it,
and each notification carries `{u16 count}` plus that many 8-byte records
(docs/protocol.md section 10).

  tools/rcp_debug_stream.py --port /dev/cu.usbmodemXXXX --seconds 6 [--decode]
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import rcp_pair as rig       # noqa: E402
import spinel_console as sc  # noqa: E402

P_LOG = 0x00B2
RECORD = 8
FIELDS = ("magic", "component", "line", "reason")


class LogNode(rig.Node):
    """A Node that keeps every RCP_LOG_CRASH_DUMP value, not just the last."""

    def __init__(self, port, name):
        rig.Node.__init__(self, port, name)
        self.batches = []

    def poll(self, seconds):
        rig.Node.poll(self, seconds)
        # A batch is {u16 count} + records, so anything shorter is the GET reply that
        # reports the enable state - leave it in values for enabled() to read.
        if len(self.values.get(P_LOG, b"")) >= 2:
            self.batches.append(self.values.pop(P_LOG))


def enabled(node):
    """Read the log property back: the state is a fact, not a request."""
    node.request(sc.CMD_PROP_VALUE_GET, P_LOG)
    node.poll(0.4)
    value = node.values.pop(P_LOG, None)
    if value is None:
        return "no reply"
    return value[0] if value else 0


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--seconds", type=float, default=6.0)
    parser.add_argument("--decode", action="store_true", help="print each record's fields")
    args = parser.parse_args(argv)

    node = LogNode(args.port, "log")
    try:
        node.drain()
        print(f"  enabled at start: {enabled(node)}")

        node.request(sc.CMD_PROP_VALUE_SET, P_LOG, b"\x01")
        node.poll(0.4)
        print(f"  after SET 1:      {enabled(node)}")

        node.batches.clear()
        node.poll(args.seconds)

        records = 0
        for batch in node.batches:
            if len(batch) < 2:
                print(f"    !! short value: {batch.hex()}")
                continue
            count = batch[0] | (batch[1] << 8)
            records += count
            for i in range(count) if args.decode else []:
                rec = batch[2 + i * RECORD:2 + (i + 1) * RECORD]
                words = [rec[j] | (rec[j + 1] << 8) for j in range(0, RECORD, 2)]
                print("    " + " ".join(f"{n}={w:#06x}" for n, w in zip(FIELDS, words)))

        print(f"  batches: {len(node.batches)}, records: {records}")

        node.request(sc.CMD_PROP_VALUE_SET, P_LOG, b"\x00")
        node.poll(0.4)
        print(f"  after SET 0:      {enabled(node)}")

        node.batches.clear()
        node.poll(1.5)
        print(f"  batches after disabling: {len(node.batches)}")
        return 0 if records > 0 and not node.batches else 1
    finally:
        node.close()


if __name__ == "__main__":
    sys.exit(main())
