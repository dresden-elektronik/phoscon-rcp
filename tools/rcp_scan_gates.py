#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""rcp_scan_gates.py - the energy-scan contract, replayed against a board.

docs/protocol.md 10.1 promises what a dwell does and what each refusal means. Most of it is proved by
the host suites, which cannot prove three things: that the machine on the board ends, that it puts the
radio back, and that a second request while a dwell runs is refused rather than queued. This script
replays those dialogs and prints a verdict per gate.

Each gate is one dialog on one port and asserts on the frames the board sends, not on dBm values:
levels are what the room is doing, and this rig sits next to real Zigbee networks (docs/testing.md 3).

    .venv/bin/python tools/rcp_scan_gates.py --port <cb2-port>
    .venv/bin/python tools/rcp_scan_gates.py --port <cb2-port> --peer <other-port>

With --peer, the second stick transmits to the first, which is a MAC node, around dwells: a board that
has moved on from the host's channel or filter reads every property back correctly, so only a frame
on the air shows it. This transmits on --channel: pick one that is quiet here.

The board must carry the RCP image. A probe-image stick enumerates the same and answers nothing.
"""
import argparse
import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])

import rcp_pair as rp  # noqa: E402
import spinel_console as sc  # noqa: E402

P_PHY_ENABLED, P_PHY_CHAN, P_RAW_STREAM = 0x20, 0x21, 0x37
P_SCAN_STATE, P_SCAN_MASK, P_SCAN_PERIOD, P_SCAN_RESULT = 0x30, 0x31, 0x32, 0x39
P_RADIO_CAPS, P_LOG = 0x120B, 0xB2
OK, UNIMPLEMENTED, INVALID_ARG, INVALID_STATE, PROP_NOT_FOUND = 0, 2, 3, 4, 13
CMD_RESET = 1
MAC_PAN, MAC_NODE, MAC_PEER, MAC_OTHER = 0xBEEF, 0x0001, 0x0002, 0x0777

PSDU = bytes.fromhex("41 20 00 be de ff ff 01 00 00 00 00 02 5a".replace(" ", ""))

results = []
records = bytearray()


def packedUint(data):
    """Decode the one packed uint a value carries, the way OpenThread's decoder does."""
    value = 0
    shift = 0
    for byte in data:
        value |= (byte & 0x7F) << shift
        if not (byte & 0x80):
            return value
        shift += 7
    return None


def verdict(name, passed, detail):
    results.append(passed)
    print(f"{'PASS' if passed else 'FAIL'}  {name}" + (f"  {detail}" if detail else ""))


def harvest(node):
    """Take the record batches the board is publishing, so the gates can be read against what the
    firmware actually saw rather than against what it reported to the host."""
    if P_LOG in node.values:
        records.extend(node.values.pop(P_LOG))


def arm(node, channel):
    """Power on the PHY and the raw stream on one channel, as the bring-up sequence does."""
    node.request(sc.CMD_PROP_VALUE_SET, P_PHY_ENABLED, b"\x01")
    node.poll(0.15)
    node.request(sc.CMD_PROP_VALUE_SET, P_PHY_CHAN, bytes([channel]))
    node.poll(0.15)
    node.request(sc.CMD_PROP_VALUE_SET, P_RAW_STREAM, b"\x01")
    node.poll(0.15)


def statusOf(node, tid):
    """The LAST_STATUS a request was answered with, or None when it was answered with a value."""
    return node.by_tid[tid][0] if node.by_tid.get(tid) else None


def startAndWait(node, tid, ms, wantResult=True, tx=False):
    """Wait out a dwell. `tx` submits transmits while waiting - at an interval long enough that the
    client's TID space is not recycled under it, which would confuse the dialog being observed."""
    """Wait out a dwell, returning (echo, result, completed)."""
    echo = None
    result = None
    completed = False
    deadline = time.time() + ms / 1000.0 + 3.0
    nextTx = time.time()
    while time.time() < deadline:
        if tx and time.time() >= nextTx:
            node.request(sc.CMD_PROP_VALUE_SET, 0x71, PSDU)
            nextTx = time.time() + 0.1
        node.poll(0.05)
        if P_SCAN_STATE in node.values:
            seen = node.values[P_SCAN_STATE]
            if seen == b"\x00":
                completed = True
            elif echo is None:
                echo = seen
        if P_SCAN_RESULT in node.values:
            result = node.values.pop(P_SCAN_RESULT)
        harvest(node)
        if statusOf(node, tid) is not None:
            return echo, result, completed, statusOf(node, tid)
        if result is not None and completed:
            break
    return echo, result, completed, None


def macRound(node, peer, channel, name):
    """Twenty frames that ask this node for an Ack and ten for another address: a verdict on all three
    counts. Twenty and not one, and a bar of 18, because a frame can be lost on the air; a foreign
    frame handed to the host cannot be."""
    acked = delivered = foreign = 0
    for n in range(30):
        mine = n < 20
        marker = bytes([0x5A, n])
        frame = rp.data_frame(n, MAC_PAN, MAC_NODE if mine else MAC_OTHER, MAC_PEER, marker, ack_request=mine)
        node.drain()
        peer.drain()
        tid = peer.transmit(frame, channel)
        peer.poll(0.15)
        node.poll(0.05)
        heard = any(marker in bytes(v) for v in node.frames)
        status = peer.by_tid.get(tid)
        if mine:
            acked += bool(status and status[0] == OK)
            delivered += heard
        else:
            foreign += heard
    verdict(name, acked >= 18 and delivered >= 18 and foreign == 0,
            f"Acked {acked} of 20, delivered {delivered} of 20, foreign frames delivered {foreign} of 10")


def dwellOn(node, channel):
    node.drain()
    node.request(sc.CMD_PROP_VALUE_SET, P_SCAN_MASK, bytes([1, channel]))
    node.poll(0.2)
    tid = node.request(sc.CMD_PROP_VALUE_SET, P_SCAN_STATE, b"\x02")
    startAndWait(node, tid, 300)


def peerGates(node, peer, channel):
    """The radio itself is back after a dwell: on the air, not only in what a GET reports. A radio left
    on the scanned channel hears nothing at all, and neither a read-back nor a host's RESET shows it."""
    other = 26 if channel != 26 else 25
    node.configure(channel, MAC_PAN, MAC_NODE, rp.OFF)
    peer.configure(channel, MAC_PAN, MAC_PEER, rp.OFF)
    macRound(node, peer, channel, "a MAC node before any dwell")
    dwellOn(node, channel)
    macRound(node, peer, channel, "a MAC node after a dwell on its own channel")
    dwellOn(node, other)
    macRound(node, peer, channel, f"a MAC node after a dwell on channel {other}")
    node.send(bytes([0x81, CMD_RESET]))
    node.poll(1.5)
    node.configure(channel, MAC_PAN, MAC_NODE, rp.OFF)
    macRound(node, peer, channel, "a MAC node after those dwells, a RESET and arming again")
    for stick in (node, peer):
        stick.send(bytes([0x81, CMD_RESET]))
        stick.poll(1.5)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", required=True)
    ap.add_argument("--channel", type=int, default=13)
    ap.add_argument("--peer", help="a second RCP that transmits to this one, around dwells (path, or path@baud)")
    ap.add_argument("--period", type=int, default=250)
    ap.add_argument("--records", help="write the board's debug records to this file (decode with "
                                      "tools/dbg.py --file <path>)")
    args = ap.parse_args()

    node = rp.Node(args.port, "gate")
    try:
        if args.peer:
            peer = rp.Node(args.peer, "peer")
            try:
                peerGates(node, peer, args.channel)
            finally:
                peer.close()
            node.drain()

        # The board's own record stream, for the verdict at the end: a scan that quietly measured
        # nothing would still pass every frame-level gate above.
        node.request(sc.CMD_PROP_VALUE_SET, P_LOG, b"\x01")
        node.poll(0.6)
        harvest(node)

        # ---- caps: the promise a host acts on ----
        node.drain()
        tid = node.request(sc.CMD_PROP_VALUE_GET, P_RADIO_CAPS)
        node.poll(0.4)
        raw = node.values.get(P_RADIO_CAPS, b"")
        caps = packedUint(raw)
        verdict("RADIO_CAPS advertises ENERGY_SCAN",
                (caps or 0) & 0x02,
                f"decoded {hex(caps) if caps is not None else 'none'} from {raw.hex() or 'none'}")

        # ---- an event id is not a property ----
        node.drain()
        tid = node.request(sc.CMD_PROP_VALUE_GET, P_SCAN_RESULT)
        node.poll(0.4)
        verdict("GET MAC_ENERGY_SCAN_RESULT is PROP_NOT_FOUND",
                statusOf(node, tid) == PROP_NOT_FOUND,
                f"LAST_STATUS {statusOf(node, tid)}")

        # ---- a scan needs a PHY to scan with ----
        node.drain()
        node.request(sc.CMD_PROP_VALUE_SET, P_PHY_ENABLED, b"\x00")
        node.poll(0.2)
        node.request(sc.CMD_PROP_VALUE_SET, P_SCAN_MASK, bytes([1, args.channel]))
        node.poll(0.2)
        tid = node.request(sc.CMD_PROP_VALUE_SET, P_SCAN_STATE, b"\x02")
        node.poll(0.4)
        verdict("a start with PHY_ENABLED off is INVALID_STATE",
                statusOf(node, tid) == INVALID_STATE,
                f"LAST_STATUS {statusOf(node, tid)}")

        arm(node, args.channel)

        # ---- a band is not a channel ----
        node.drain()
        whole = bytes([16]) + bytes(range(11, 27))
        tid = node.request(sc.CMD_PROP_VALUE_SET, P_SCAN_MASK, whole)
        node.poll(0.4)
        stored = statusOf(node, tid) is None and node.values.get(P_SCAN_MASK) == whole
        tid = node.request(sc.CMD_PROP_VALUE_SET, P_SCAN_STATE, b"\x02")
        node.poll(0.4)
        verdict("the whole band stores, and a dwell on it is INVALID_ARG",
                stored and statusOf(node, tid) == INVALID_ARG,
                f"stored {stored}, start LAST_STATUS {statusOf(node, tid)}")

        # ---- the kinds a raw RCP does not do ----
        node.drain()
        for kind, label in ((1, "beacon"), (3, "discover"), (7, "out of range")):
            tid = node.request(sc.CMD_PROP_VALUE_SET, P_SCAN_STATE, bytes([kind]))
            node.poll(0.3)
            verdict(f"{label} scan is UNIMPLEMENTED",
                    statusOf(node, tid) == UNIMPLEMENTED,
                    f"LAST_STATUS {statusOf(node, tid)}")

        # ---- a conformant dwell, start to completion ----
        node.drain()
        node.request(sc.CMD_PROP_VALUE_SET, P_SCAN_MASK, bytes([1, args.channel]))
        node.poll(0.2)
        node.request(sc.CMD_PROP_VALUE_SET, P_SCAN_PERIOD,
                     bytes([args.period & 0xFF, (args.period >> 8) & 0xFF]))
        node.poll(0.2)
        tid = node.request(sc.CMD_PROP_VALUE_SET, P_SCAN_STATE, b"\x02")
        echo, result, completed, refused = startAndWait(node, tid, args.period)
        verdict("a dwell echoes 0x30 = 2, reports a level, and completes",
                refused is None and echo == b"\x02" and result is not None and completed,
                f"echo {echo.hex() if echo else '-'}, result "
                f"{result[1] - 256 if result and result[1] > 127 else (result[1] if result else '-')} dBm "
                f"on channel {result[0] if result else '-'}, completed {completed}")

        # ---- the host's receive state came back ----
        node.drain()
        tid = node.request(sc.CMD_PROP_VALUE_GET, P_PHY_CHAN)
        node.poll(0.4)
        chan = node.values.get(P_PHY_CHAN, b"")
        verdict("after a dwell the channel the host asked for is what GET reports",
                chan == bytes([args.channel]), f"PHY_CHAN {chan.hex() if chan else '-'}")

        # ---- a second dwell is refused, not queued ----
        node.drain()
        node.request(sc.CMD_PROP_VALUE_SET, P_SCAN_PERIOD, bytes([200, 0]))
        node.poll(0.2)
        first = node.request(sc.CMD_PROP_VALUE_SET, P_SCAN_STATE, b"\x02")
        node.poll(0.1)
        second = node.request(sc.CMD_PROP_VALUE_SET, P_SCAN_STATE, b"\x02")
        node.poll(0.4)
        verdict("a second start while a dwell runs is INVALID_STATE",
                statusOf(node, second) == INVALID_STATE,
                f"first answered {statusOf(node, first)}, second {statusOf(node, second)}")
        _, _, _, _ = startAndWait(node, first, 400)

        # ---- a transmit outranks a dwell, and the dwell still ends ----
        node.drain()
        node.request(sc.CMD_PROP_VALUE_SET, P_SCAN_PERIOD, bytes([250, 0]))
        node.poll(0.2)
        tid = node.request(sc.CMD_PROP_VALUE_SET, P_SCAN_STATE, b"\x02")
        echo, result, completed, refused = startAndWait(node, tid, 250, tx=True)
        verdict("a dwell survives transmits submitted while it runs",
                result is not None and completed,
                f"echo {echo.hex() if echo else '-'}, result present {result is not None}, "
                f"completed {completed}")

        node.drain()
        tid = node.request(sc.CMD_PROP_VALUE_GET, P_SCAN_STATE)
        node.poll(0.4)
        verdict("and after all of that the state reads idle",
                node.values.get(P_SCAN_STATE) == b"\x00",
                f"GET 0x30 {node.values.get(P_SCAN_STATE, b'-')}")

        # ---- a disarm ends a dwell and says so ----
        node.drain()
        node.request(sc.CMD_PROP_VALUE_SET, P_SCAN_PERIOD, bytes([900 & 0xFF, (900 >> 8) & 0xFF]))
        node.poll(0.2)
        tid = node.request(sc.CMD_PROP_VALUE_SET, P_SCAN_STATE, b"\x02")
        node.poll(0.25)
        node.request(sc.CMD_PROP_VALUE_SET, P_PHY_ENABLED, b"\x00")
        echo, result, completed, refused = startAndWait(node, tid, 900)
        sentinel = result is not None and result[1] == 127
        verdict("a disarm mid-dwell reports +127 and completes",
                sentinel and completed,
                f"result {result.hex() if result else '-'}, completed {completed}")

        node.poll(0.8)
        harvest(node)
        if args.records:
            with open(args.records, "wb") as fh:
                fh.write(bytes(records))
            print(f"== {len(records) // 8} debug record(s) written to {args.records}")

        total = len(results)
        good = sum(1 for r in results if r)
        print(f"== {good} of {total} gates passed")
        return 0 if good == total else 1
    finally:
        node.close()


if __name__ == "__main__":
    sys.exit(main())
