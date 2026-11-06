#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""cb3_rcp_check.py — the ConBee III RCP's receiver and transmitter, against a ConBee II.

The ConBee III runs the rcp image and is driven as a Spinel RCP over its USART bridge (115 200 baud,
not a CDC device, so the line rate is set here); a ConBee II running this tree's RCP is the
peer, as in tools/rcp_pair.py, on a test channel and PAN. The ConBee III transmits only on --channel
(default 26) and the test PAN; the retune cell receives on --channel2.

Cells, each a burst of data frames from the ConBee II and a count of the ConBee III's STREAM_RAW
notifications (docs/testing.md §3 for the frame classes):

  1. promiscuous FULL: every frame of the burst is reported, byte-identical with its FCS;
  2. a second channel: the receiver follows a retune;
  3. filter OFF (a MAC node): a frame for its own short address is reported, one for a third
     address and one for another PAN are not - the third level filter, run in software;
  4. the ConBee III transmits a burst: the ConBee II reports every frame byte-identical;
  5. acknowledged transmission: a frame asking for an Ack, to the ConBee II as a MAC node, completes
     with LAST_STATUS 0; to the ConBee II in promiscuous mode, which does not acknowledge, with
     NO_ACK (17).

    .venv/bin/python tools/cb3_rcp_check.py --cb3 <cb3-port> \\
        --tx <cb2-port> [--channel 26] [--channel2 24]

Exit code 0 only when every cell passes.
"""
import argparse
import os
import sys
import termios
import time
import tty

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rcp_pair  # noqa: E402

PAN, OTHER_PAN = 0xBEE5, 0x0BEE
TX_SHORT, CB3_SHORT, THIRD = 0x0001, 0x0002, 0x0003


class UartNode(rcp_pair.Node):
    """rcp_pair's node over a USART bridge: the tty raw, at the board's line rate."""

    def __init__(self, port, name, baud=termios.B115200):
        super().__init__(port, name)
        fd = self.ser.fileno()
        tty.setraw(fd)
        attrs = termios.tcgetattr(fd)
        attrs[4] = attrs[5] = baud
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        termios.tcflush(fd, termios.TCIOFLUSH)


def burst(tx, rx, channel, n, dst_pan, dst, tag):
    sent = []
    rx.drain()
    for seq in range(n):
        psdu = rcp_pair.data_frame(seq, dst_pan, dst, TX_SHORT, b"cb3rx-" + bytes([tag, seq]))
        sent.append(psdu)
        tx.transmit(psdu, channel)
        tx.poll(0.03)
    rx.poll(0.5)
    got = [rcp_pair.psdu_of(v) for v in rx.frames]
    return sent, got


def cell(label, ok, detail):
    print(f"  {'PASS' if ok else 'FAIL'}  {label:46} {detail}")
    return ok


def receive_cells(a, tx, rx):
    """Cells 1-3: the ConBee III receives. Returns the failures, or None when arming failed."""
    fails = 0
    print(f"1. promiscuous, channel {a.channel}")
    tx.configure(a.channel, PAN, TX_SHORT, rcp_pair.OFF)
    rx.configure(a.channel, PAN, CB3_SHORT, rcp_pair.FULL)
    if not (tx.mode_ok and rx.mode_ok):
        print("  configuration did not take; stopping")
        return 2
    sent, got = burst(tx, rx, a.channel, a.frames, PAN, 0xFFFF, 1)
    same = sum(1 for p in sent if p in got)
    fails += not cell("every frame, byte-identical with its FCS", same == len(sent),
                      f"{same} of {len(sent)} identical, {len(got)} reported")

    print(f"2. retuned to channel {a.channel2}")
    tx.configure(a.channel2, PAN, TX_SHORT, rcp_pair.OFF)
    rx.configure(a.channel2, PAN, CB3_SHORT, rcp_pair.FULL)
    sent, got = burst(tx, rx, a.channel2, a.frames, PAN, 0xFFFF, 2)
    same = sum(1 for p in sent if p in got)
    fails += not cell("every frame on the new channel", same == len(sent), f"{same} of {len(sent)}")

    print(f"3. filter OFF (a MAC node), channel {a.channel2}")
    rx.configure(a.channel2, PAN, CB3_SHORT, rcp_pair.OFF)
    for label, pan, dst, want in (("our PAN, addressed to the ConBee III", PAN, CB3_SHORT, True),
                                  ("our PAN, broadcast", PAN, 0xFFFF, True),
                                  ("our PAN, a third address", PAN, THIRD, False),
                                  ("another PAN", OTHER_PAN, CB3_SHORT, False)):
        sent, got = burst(tx, rx, a.channel2, 3, pan, dst, 3)
        n = sum(1 for p in sent if p in got)
        fails += not cell(label, (n == len(sent)) if want else n == 0,
                          f"{n} of {len(sent)} reported (expected {'all' if want else 'none'})")
    return fails


def transmit_cells(a, tx, rx):
    """Cells 4-5: the ConBee III transmits, on --channel only."""
    fails = 0
    print(f"4. the ConBee III transmits, channel {a.channel}")
    tx.configure(a.channel, PAN, TX_SHORT, rcp_pair.FULL)
    rx.configure(a.channel, PAN, CB3_SHORT, rcp_pair.OFF)
    sent = []
    for seq in range(a.frames):
        psdu = rcp_pair.data_frame(seq, PAN, 0xFFFF, CB3_SHORT, b"cb3tx-" + bytes([seq]))
        sent.append(psdu)
        rx.transmit(psdu, a.channel)
        rx.poll(0.05)
    tx.poll(0.5)
    got = [rcp_pair.psdu_of(v) for v in tx.frames]
    same = sum(1 for p in sent if p in got)
    fails += not cell("every frame, byte-identical with its FCS", same == len(sent),
                      f"{same} of {len(sent)} received by the ConBee II")

    print("5. acknowledged transmission")
    for label, mode, expect in (("to a MAC node: acknowledged", rcp_pair.OFF, 0),
                                ("to a promiscuous node: NO_ACK", rcp_pair.NETWORK, 17)):
        tx.configure(a.channel, PAN, TX_SHORT, mode)
        rx.drain()
        tid = rx.transmit(rcp_pair.data_frame(0x40 + mode, PAN, TX_SHORT, CB3_SHORT, b"\x06\x06",
                                              ack_request=True), a.channel)
        rx.poll(1.0)
        value = rx.by_tid.get(tid)
        code = value[0] if value else None
        fails += not cell(label, code == expect, f"LAST_STATUS {code} (expected {expect})")
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cb3", required=True)
    ap.add_argument("--tx", required=True)
    ap.add_argument("--channel", type=int, default=26)
    ap.add_argument("--channel2", type=int, default=24)  # quiet on this bench; 15 carries a live network
    ap.add_argument("--frames", type=int, default=20)
    ap.add_argument("--tx-only", action="store_true", help="cells 4 and 5 only")
    # The same cells check any UART RCP of this tree: the ConBee I / RaspBee I (cbrb1) at 57 600.
    ap.add_argument("--baud", type=int, default=115200, help="the UART RCP's line rate (cbrb1: 57600)")
    a = ap.parse_args()
    speed = getattr(termios, f"B{a.baud}", None)
    if speed is None:
        sys.exit(f"cb3_rcp_check: baud {a.baud} is not supported by this system")
    tx = rcp_pair.Node(a.tx, "tx")
    rx = UartNode(a.cb3, "cb3", speed)
    try:
        fails = 0 if a.tx_only else receive_cells(a, tx, rx)
        if fails is None:
            return 2
        fails += transmit_cells(a, tx, rx)
    finally:
        tx.close()
        rx.close()
    print(f"== {fails} failure(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
