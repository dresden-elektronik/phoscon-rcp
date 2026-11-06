#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""rcp_matrix.py — every board against every other: a burst and an Ack, per ordered pair.

Each node is one RCP of this tree, driven over Spinel as in tools/rcp_pair.py, whatever its link:

  /dev/cu.usbmodemDE...                   a ConBee II, USB CDC
  /dev/cu.usbserial-DE...@115200          a ConBee III, a USART behind a bridge chip, at its line rate
  ssh://<pi-host>/dev/ttyAMA0@115200      a RaspBee II on a Raspberry Pi: the tty is opened there by a
                                          relay this tool starts over ssh (python3 on the far side);
                                          <pi-host> may be user@host
  ssh://<pi-host>/dev/ttyS0@57600         a RaspBee I on a Raspberry Pi 3, the same way

For every ordered pair (tx, rx), on --channel and the test PAN only, with every other node's radio
off (a bystander still a MAC node on the same PAN and address would acknowledge in rx's place):

  1. burst: rx in promiscuous FULL, tx sends --frames data frames; at least --min of them (0.9) must be
     reported by rx byte-identical with its FCS, every missing one is listed, and tx's completions are
     counted, so a transmit refused tells apart from a frame lost on the air;
  2. Ack: rx a MAC node (filter OFF, its own short address), tx sends a frame asking for an Ack;
     tx's LAST_STATUS must be 0 within three attempts, a lost frame or Ack being the air's. A node
     named in --no-ack (a radio back-end that does not compose an Ack yet, docs/hardware_mg21.md §10.5) is
     expected to leave tx with NO_ACK (17) instead.

    .venv/bin/python tools/rcp_matrix.py \\
        --node cb2a=<cb2-port> --node cb2b=<cb2-port-b> \\
        --node cb3=<cb3-port>@115200 \\
        --node rb2=ssh://<pi-host>/dev/ttyAMA0@115200

(add --no-ack cb3 when the ConBee III runs the `own` back-end, which composes no Ack.)

Exit code 0 only when every cell passes.
"""
import argparse
import os
import subprocess
import sys
import termios
import tty

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rcp_pair  # noqa: E402

PAN = 0xBEE5
NO_ACK = 17

# Run on the far side of ssh: the tty raw at the given rate, bytes relayed both ways unchanged.
RELAY = r"""
import os, select, sys, termios, tty
fd = os.open(sys.argv[1], os.O_RDWR | os.O_NOCTTY)
tty.setraw(fd)
a = termios.tcgetattr(fd)
a[4] = a[5] = getattr(termios, "B" + sys.argv[2])
a[2] |= termios.CLOCAL | termios.CREAD
termios.tcsetattr(fd, termios.TCSANOW, a)
termios.tcflush(fd, termios.TCIOFLUSH)
sys.stdout.write("RELAY\n"); sys.stdout.flush()
out, inp = sys.stdout.fileno(), sys.stdin.fileno()
while True:
    r, _, _ = select.select([fd, inp], [], [])
    if fd in r:
        os.write(out, os.read(fd, 4096))
    if inp in r:
        d = os.read(inp, 4096)
        if not d:
            break
        os.write(fd, d)
"""


class SshStream:
    """A tty on another host, as rcp_pair.Node's stream: non-blocking read, write, close."""

    def __init__(self, host, path, baud):
        self.proc = subprocess.Popen(["ssh", "-T", host, "python3", "-u", "-c", repr_arg(RELAY), path,
                                      str(baud)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     bufsize=0)
        line = self.proc.stdout.readline()
        if line.strip() != b"RELAY":
            raise RuntimeError(f"ssh relay to {host}:{path} did not start: {line!r}")
        os.set_blocking(self.proc.stdout.fileno(), False)

    def read(self, n):
        return os.read(self.proc.stdout.fileno(), n)

    def write(self, data):
        return os.write(self.proc.stdin.fileno(), data)

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=5)


def repr_arg(code):
    """The relay as one shell word for the remote shell ssh hands the command to."""
    return "'" + code.replace("'", "'\"'\"'") + "'"


def open_node(name, spec):
    # The rate follows the last '@': an ssh spec's host may carry a user@ of its own.
    port, sep, baud = spec.rpartition("@")
    if not sep:
        port, baud = spec, ""
    if port.startswith("ssh://"):
        host, _, path = port[len("ssh://"):].partition("/")
        return rcp_pair.Node(spec, name, stream=SshStream(host, "/" + path, int(baud or 38400)))
    node = rcp_pair.Node(port, name)
    if baud:
        fd = node.ser.fileno()
        tty.setraw(fd)
        attrs = termios.tcgetattr(fd)
        attrs[4] = attrs[5] = getattr(termios, f"B{baud}")
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        termios.tcflush(fd, termios.TCIOFLUSH)
    return node


def cell(label, ok, detail):
    print(f"  {'PASS' if ok else 'FAIL'}  {label:34} {detail}")
    return ok


def pair_cells(a, tx, rx, rx_acks):
    fails = 0
    tx.configure(a.channel, PAN, 0x0001, rcp_pair.OFF)
    rx.configure(a.channel, PAN, 0x0002, rcp_pair.FULL)
    if not (tx.mode_ok and rx.mode_ok):
        cell(f"{tx.name} -> {rx.name}: configuration", False, "a SET did not take")
        return 1
    sent = []
    tx.drain()
    for seq in range(a.frames):
        psdu = rcp_pair.data_frame(seq, PAN, 0xFFFF, 0x0001, b"mx-" + tx.name[:4].encode() + bytes([seq]))
        sent.append(psdu)
        tx.transmit(psdu, a.channel)
        tx.poll(0.05)
    rx.poll(0.8)
    tx.poll(0.2)
    got = [rcp_pair.psdu_of(v) for v in rx.frames]
    same = sum(1 for p in sent if p in got)
    # The sender's completions tell a transmit it refused from a frame lost on the air.
    done = sum(1 for v in tx.statuses if v and v[0] == 0)
    detail = f"{same} of {len(sent)} byte-identical, {len(got)} reported, {done} sent OK"
    missing = [p[2] for p in sent if p not in got]
    if missing:
        detail += f"; missing seq {missing}"
    odd = [g.hex() for g in got if g not in sent]
    if odd:
        detail += f"; not sent: {odd}"
    # A frame lost on the air is not a failure of the pair: this bench's channel 26 loses about one in
    # twenty to FCS errors (docs/hardware_samr21.md §6), so --min of the burst must arrive intact.
    fails += not cell(f"{tx.name} -> {rx.name}: burst", same >= a.min * len(sent), detail)

    rx.configure(a.channel, PAN, 0x0002, rcp_pair.OFF)
    want = 0 if rx_acks else NO_ACK
    # The cell is about whether rx acknowledges, not whether the air carried the frame and its Ack:
    # as in tools/rcp_pair.py, an exchange that ends in NO_ACK is repeated, up to three times.
    for attempt in range(1, 4 if rx_acks else 2):
        tx.drain()
        tid = tx.transmit(rcp_pair.data_frame(0x60 + attempt, PAN, 0x0002, 0x0001, b"\x06\x06",
                                              ack_request=True), a.channel)
        tx.poll(1.0)
        value = tx.by_tid.get(tid)
        code = value[0] if value else None
        if code == want:
            break
    fails += not cell(f"{tx.name} -> {rx.name}: Ack", code == want,
                      f"LAST_STATUS {code} (expected {want}{'' if rx_acks else ', --no-ack'}), "
                      f"attempt {attempt}")
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--node", action="append", required=True, metavar="NAME=SPEC")
    ap.add_argument("--no-ack", action="append", default=[], metavar="NAME")
    ap.add_argument("--channel", type=int, default=26)
    ap.add_argument("--frames", type=int, default=10)
    ap.add_argument("--min", type=float, default=0.9, help="share of a burst that must arrive intact")
    a = ap.parse_args()
    if a.channel != 26:
        print("note: the bench's test channel is 26")
    nodes = []
    try:
        for item in a.node:
            name, _, spec = item.partition("=")
            nodes.append(open_node(name, spec))
        for n in nodes:
            n.drain()
            n.request(rcp_pair.CMD_PROP_VALUE_GET, 0x01)
            n.poll(0.6)
            print(f"{n.name}: PROTOCOL_VERSION {n.values.get(0x01, b'').hex() or 'no answer'}")
        fails = 0
        for tx in nodes:
            for rx in nodes:
                if tx is rx:
                    continue
                # A bystander left a MAC node on the same PAN and address would acknowledge in
                # rx's place, so every node outside the pair has its radio off.
                for n in nodes:
                    if n is not tx and n is not rx:
                        n.set_checked(rcp_pair.P_PHY_ENABLED, b"\x00", b"\x00")
                fails += pair_cells(a, tx, rx, rx.name not in a.no_ack)
        for n in nodes:
            n.set_checked(rcp_pair.P_RAW_STREAM, b"\x00", b"\x00")
            n.set_checked(rcp_pair.P_PHY_ENABLED, b"\x00", b"\x00")
    finally:
        for n in nodes:
            n.close()
    print(f"== {fails} failure(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
