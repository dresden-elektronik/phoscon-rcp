#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""rcp_pair.py — two ConBee IIs, one Spinel conversation each: the deterministic rig.

The live-network captures told us a lot but could not settle the receive-filter semantics,
because they could not produce a *chosen* frame: a bursty Zigbee network gives you whatever
it gives you, and two identical runs returned nothing and a peer respectively. With two
RCPs there is no such ambiguity - node A transmits exactly the frame a test needs and node B
either reports it or does not, on a test channel and a test PAN, so nothing here touches a
real network.

What it does:
  1. configures both nodes (channel, PAN, short address, promiscuous mode, raw stream);
  2. round trip: A transmits a data frame addressed to B, B must report those bytes;
  3. large frame: A transmits a frame with a ~100-byte PSDU and B must report it
     byte-identical - the RX notification is far larger than one USB packet, so the
     outbound path must chunk it across packets rather than drop it whole;
  4. filter matrix: for B in FULL / NETWORK / OFF, four frame classes
       (a) our PAN, broadcast      (b) our PAN, addressed to B
       (c) our PAN, a third address (d) another PAN entirely
     with the expected delivery per mode taken from docs/testing.md §3:
       FULL    a b c d      NETWORK a b c        OFF  a b
  5. ACK policy: A transmits with the acknowledgment-request bit set and watches for an
     ACK (a 5-byte frame type 2 carrying its sequence number). Whether one comes back is
     the datapoint, not a failure: hardware ACK generation is off in every mode by design.

Exit code 0 only when every assertion passes.

  .venv/bin/python tools/rcp_pair.py --a <cb2-port> \\
      --b <cb2-port-b> --channel 26 --pan 0xbee5
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
import spinel_console as sc  # noqa: E402  (the protocol lives in one place)

CMD_PROP_VALUE_GET, CMD_PROP_VALUE_SET = sc.CMD_PROP_VALUE_GET, sc.CMD_PROP_VALUE_SET
P_PHY_ENABLED, P_PHY_CHAN, P_PANID, P_SADDR = 0x20, 0x21, 0x36, 0x35
P_PROMISC, P_RAW_STREAM, P_STREAM_RAW = 0x38, 0x37, 0x71

FULL, NETWORK, OFF = 2, 1, 0
FRAME_VERSION_2003 = 0x00


def set_line_rate(fd, baud):
    """The tty raw at `baud`, both directions, with anything already buffered discarded."""
    import termios
    import tty
    speed = getattr(termios, f"B{baud}", None)
    if speed is None:
        sys.exit(f"rcp_pair: baud {baud} is not supported by this system")
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    attrs[4] = attrs[5] = speed
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)


class Node:
    """One RCP over its tty: framing, arming, transmit and receive.

    A CDC port is named by its path. A UART RCP is named `path@baud` (the ConBee I / RaspBee I at
    57 600, the ConBee III at 115 200): the tty is put raw at that rate, because a CDC pipe's rate is
    the bus's and a UART's is the line's."""

    def __init__(self, port, name, stream=None):
        """`stream` replaces the tty: any object with non-blocking read, write and close."""
        self.port, self.name = port, name
        if stream is None:
            path, _, baud = port.partition("@")
            stream = open(path, "r+b", buffering=0)
            os.set_blocking(stream.fileno(), False)
            if baud:
                set_line_rate(stream.fileno(), int(baud))
        self.ser = stream
        self.dec = sc.Decoder()
        self.tid = 1
        self.frames = []          # every STREAM_RAW payload seen since the last drain
        self.statuses = []        # LAST_STATUS values
        self.by_tid = {}          # request TID -> LAST_STATUS value
        self.values = {}          # property id -> value, from any VALUE_IS
        # A single large write to the CDC tty is truncated on macOS: the device then sees a
        # partial Spinel frame and silently drops the request, which made a valid 100-byte-PSDU
        # transfer look like a firmware defect. Write in pieces instead.
        self._rawWrite = self.ser.write

        def chunkedWrite(data):
            for i in range(0, len(data), 32):
                self._rawWrite(data[i:i + 32])
                time.sleep(0.002)
            return len(data)

        self.ser.write = chunkedWrite

    def close(self):
        self.ser.close()

    def send(self, payload):
        self.ser.write(sc.frame(payload))

    def request(self, cmd, prop, value=b""):
        tid = self.tid
        self.tid = 1 if tid == 15 else tid + 1
        self.send(bytes([0x80 | tid, cmd]) + sc.pack_uint(prop) + value)
        return tid

    def poll(self, seconds):
        """Read for `seconds`, collecting STREAM_RAW frames and LAST_STATUS values."""
        deadline = time.time() + seconds
        while time.time() < deadline:
            try:
                data = self.ser.read(4096) or b""
            except (BlockingIOError, OSError):
                data = b""
            if not data:
                time.sleep(0.004)
                continue
            for body in self.dec.feed(data):
                entry = sc.classify(body)
                if entry is None:
                    continue
                if entry["prop"] == P_STREAM_RAW:
                    self.frames.append(entry["value"])
                elif entry["prop"] == 0x00 and entry["value"]:
                    self.statuses.append(entry["value"])
                    self.by_tid[entry["tid"]] = entry["value"]
                else:
                    self.values[entry["prop"]] = entry["value"]

    def drain(self):
        self.poll(0.05)
        self.frames.clear()
        self.statuses.clear()
        self.by_tid.clear()
        self.values.clear()

    # ---- configuration ---------------------------------------------------
    def set_checked(self, prop, value, expect, tries=4):
        """SET a property and *read it back*: the requested mode is a request, not a fact.

        The rig used to send the mode and assume. When a cell then disagreed with the
        spec, there was no way to tell a filter that leaked from a mode that never took -
        and the answer was "the mode never took", which cost
        three runs to find because the tool believed its own writes.
        """
        for _ in range(tries):
            self.request(CMD_PROP_VALUE_SET, prop, value)
            self.poll(0.35)
            # Whole value, not just its first byte: PANID/SADDR are two bytes, and the
            # "rejected" line printed the value that had in fact been accepted.
            if self.values.get(prop, b"") == expect:
                return True
        print(f"    !! {self.name} did not accept prop {prop:#04x} = {expect.hex()}: "
              f"reports {self.values.get(prop, b'?').hex()}")
        return False

    def configure(self, channel, pan, short, promisc, settle=1.2):
        """The arming sequence of docs/protocol.md §6, every field verified.

        Every input a cell later depends on is read back, not assumed: a mode that never took
        is indistinguishable from a filter that leaks, and the same is true of the
        channel, the PAN and the short address.
        """
        ok = True
        ok &= self.set_checked(P_PHY_ENABLED, b"\x01", b"\x01")
        ok &= self.set_checked(P_PHY_CHAN, bytes([channel]), bytes([channel]))
        ok &= self.set_checked(P_PANID, bytes([pan & 0xFF, (pan >> 8) & 0xFF]),
                               bytes([pan & 0xFF, (pan >> 8) & 0xFF]))
        ok &= self.set_checked(P_SADDR, bytes([short & 0xFF, (short >> 8) & 0xFF]),
                               bytes([short & 0xFF, (short >> 8) & 0xFF]))
        ok &= self.set_checked(P_PROMISC, bytes([promisc]), bytes([promisc]))
        # MAC_RAW_STREAM_ENABLED is what arms reception *and* what the RCP requires before it
        # will transmit at all (docs/protocol.md §7.1), so it is verified like the mode.
        ok &= self.set_checked(P_RAW_STREAM, b"\x01", b"\x01")
        self.poll(settle)
        self.drain()
        self.mode_ok = ok
        return self

    # ---- transmit --------------------------------------------------------
    def transmit(self, psdu, channel, ack_request=False):
        """docs/protocol.md §7.1: length, PSDU, then the 16-byte trailer. CSMA off, no retries."""
        trailer = bytes([channel, 0, 0, 0, 0, 0, 0,
                         0, 0, 0, 0,      # txDelay
                         0, 0, 0, 0,      # txDelayBase
                         0])              # rxChannelAfterTx: keep
        payload = len(psdu).to_bytes(2, "little") + psdu + trailer
        return self.request(CMD_PROP_VALUE_SET, P_STREAM_RAW, payload)


def fcs(frame):
    """802.15.4-2024 7.2.11's CRC-16, low octet first: what the host puts at the end of a PSDU.

    The PSDU handed to Radio_Tx carries its own FCS (docs/protocol.md §7.1), and the
    transceiver replaces those two octets with the checksum it computes over the rest, so a
    frame built here must end with the same value a receiver will check.
    """
    crc = 0x0000
    for octet in frame:
        crc ^= octet
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return bytes([crc & 0xFF, crc >> 8])


def data_frame(seq, dst_pan, dst_addr, src_addr, payload, ack_request=False):
    """A minimal 802.15.4 data frame, PAN-compressed, both addresses 16-bit, its FCS last.

    The receiver's STREAM_RAW reports the frame with the two FCS bytes at the end, which is why
    the comparisons below slice to the sent length.
    """
    fcf = (FRAME_VERSION_2003 << 12) | (2 << 14) | (2 << 10) | (1 << 6) | 0x01
    if ack_request:
        fcf |= 0x20
    body = (bytes([fcf & 0xFF, fcf >> 8]) + bytes([seq])
            + bytes([dst_pan & 0xFF, dst_pan >> 8])
            + bytes([dst_addr & 0xFF, dst_addr >> 8])
            + bytes([src_addr & 0xFF, src_addr >> 8]) + payload)
    return body + fcs(body)


def command_frame(seq, dst_pan, dst_addr, src_addr, command):
    """A MAC command frame, PAN-compressed, both addresses 16-bit - the shape a polling device
    sends. The frame type is 3, so the transceiver treats it as a MAC command and, for a Data
    Request, answers it with the Frame Pending bit it was configured with."""
    fcf = (FRAME_VERSION_2003 << 12) | (2 << 14) | (2 << 10) | (1 << 6) | 0x20 | 0x03
    body = (bytes([fcf & 0xFF, fcf >> 8]) + bytes([seq])
            + bytes([dst_pan & 0xFF, dst_pan >> 8])
            + bytes([dst_addr & 0xFF, dst_addr >> 8])
            + bytes([src_addr & 0xFF, src_addr >> 8]) + command)
    return body + fcs(body)


def command_frame_ext_src(seq, dst_pan, dst_addr, src_ext_msb, command):
    """The same command frame with an extended source: the address is given MSB first, the way
    a host writes it into MAC_SRC_MATCH_EXTENDED_ADDRESSES, and travels LSB first on air."""
    fcf = (FRAME_VERSION_2003 << 12) | (3 << 14) | (2 << 10) | (1 << 6) | 0x20 | 0x03
    body = (bytes([fcf & 0xFF, fcf >> 8]) + bytes([seq])
            + bytes([dst_pan & 0xFF, dst_pan >> 8])
            + bytes([dst_addr & 0xFF, dst_addr >> 8])
            + src_ext_msb[::-1] + command)
    return body + fcs(body)


def psdu_of(value):
    """The PSDU out of a STREAM_RAW payload (docs/protocol.md §7.2: u16 length, PSDU, metadata)."""
    if len(value) < 2:
        return b""
    n = value[0] | (value[1] << 8)
    return value[2:2 + n]


def run_cell(a, b, label, psdu, expect, channel):
    """Transmit, then assert whether B reported the frame.

    One retry when delivery was expected: this is RF over a bench, and a single lost frame
    is a lost frame, not a finding. A cell that must be *dropped* is asserted on the first
    transmit alone - retrying it would only make a real leak more likely to show up as a
    pass.
    """
    b.drain()
    a.transmit(psdu, channel)
    b.poll(1.6)
    got = any(psdu_of(f)[:len(psdu)] == psdu for f in b.frames)
    if expect and not got:
        a.transmit(psdu, channel)
        b.poll(1.6)
        got = any(psdu_of(f)[:len(psdu)] == psdu for f in b.frames)
    ok = got == expect
    print(f"    {'PASS' if ok else 'FAIL'}  {label:46s} "
          f"{'delivered' if got else 'dropped':9s} (expected "
          f"{'delivered' if expect else 'dropped'})")
    return ok


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", required=True, help="transmitting RCP's port")
    ap.add_argument("--b", required=True, help="receiving RCP's port (path@baud for a UART RCP)")
    ap.add_argument("--channel", type=int, default=26,
                    help="test channel; 26 is the quiet one, and the default on purpose")
    ap.add_argument("--pan", type=lambda x: int(x, 0), default=0xBEE5,
                    help="test PAN (must not be a live network's)")
    ap.add_argument("--foreign-pan", type=lambda x: int(x, 0), default=0x0BEE)
    args = ap.parse_args(argv)

    if args.pan == args.foreign_pan:
        sys.exit("the test PAN and the foreign PAN must differ")
    a_addr, b_addr, other_addr = 0x0A01, 0x0B02, 0x0C03

    a, b = Node(args.a, "A"), Node(args.b, "B")
    failures = []
    try:
        print(f"rig: A={args.a} B={args.b} channel={args.channel} pan={args.pan:#06x}")

        # Both nodes must be *running* before anything is asserted. A stick that has just
        # been flashed answers nothing for ~12 s, and every SET then goes nowhere - which
        # looks exactly like ten filter failures. It did, once, on the run right after a
        # flash; the fix is a handshake rather than a sleep, because a sleep is a guess.
        for node in (a, b):
            alive = False
            for _ in range(20):
                node.drain()
                node.request(CMD_PROP_VALUE_GET, 0x01)      # PROTOCOL_VERSION
                node.poll(0.5)
                if node.values.get(0x01):
                    alive = True
                    break
            print(f"  {node.name}={node.port}: "
                  f"{'answers ' + node.values[0x01].hex(' ') if alive else 'NO ANSWER'}")
            if not alive:
                failures.append(True)
        if failures:
            print("== a node is not running; nothing else can be asserted")
            return 1
        # A a MAC node: the rail back-end does not see an Ack while promiscuous (docs/protocol.md).
        a.configure(args.channel, args.pan, a_addr, OFF)
        b.configure(args.channel, args.pan, b_addr, FULL)

        print("  1. round trip")
        psdu = data_frame(1, args.pan, b_addr, a_addr, b"\xaa\x55")
        # Warm the link up before asserting anything. The first frames after a boot can be
        # lost before the receiver's radio is really up - a decaying window, not a
        # filter property - so the rig spends it here and says when it did.
        for attempt in range(1, 9):
            b.drain()
            a.transmit(psdu, args.channel)
            b.poll(1.0)
            if any(psdu_of(f)[:len(psdu)] == psdu for f in b.frames):
                if attempt > 1:
                    print(f"    (link needed {attempt} warm-up frame(s) to deliver)")
                break
        else:
            print("    !! no warm-up frame was delivered in 8 attempts")
        # A's own completion is part of the round trip: a SET STREAM_RAW the RCP refused (no
        # raw stream armed) transmits nothing and looks exactly like a lost frame.
        a.drain()
        tid = a.transmit(psdu, args.channel)
        a.poll(1.5)
        verdict = a.by_tid.get(tid)
        ok = verdict is not None and verdict[0] == 0
        print(f"    {'PASS' if ok else 'FAIL'}  A's transmission completed OK"
              f"{' ' * 28}A's LAST_STATUS {verdict[0] if verdict else '-'} (expected 0)")
        failures.append(not ok)
        failures.append(not run_cell(a, b, "A -> B, addressed to B (FULL)",
                                     psdu, True, args.channel))

        # A ~100-byte PSDU makes the RX notification far larger than the 64-byte USB
        # packet the outbound path once staged whole. The RCP must HDLC-encode it into the
        # TX ring and chunk it across packets; the old all-or-nothing staging dropped the
        # whole notification, so this cell failed on that firmware.
        print("  1b. large frame: a PSDU larger than one USB packet")
        large = data_frame(0x20, args.pan, b_addr, a_addr, bytes(range(0x40, 0x40 + 91)))
        failures.append(not run_cell(a, b, "A -> B, 100-byte PSDU (FULL)",
                                     large, True, args.channel))

        print("  2. filter matrix (B's mode x frame class)")
        classes = [
            ("a our PAN, broadcast",   data_frame(0x10, args.pan, 0xFFFF, a_addr, b"\x01\x01"), True),
            ("b our PAN, to B",        data_frame(0x11, args.pan, b_addr, a_addr, b"\x02\x02"), True),
            ("c our PAN, third addr",  data_frame(0x12, args.pan, other_addr, a_addr, b"\x03\x03"), None),
            ("d foreign PAN",          data_frame(0x13, args.foreign_pan, b_addr, a_addr, b"\x04\x04"), None),
        ]
        for mode, name in ((FULL, "FULL"), (NETWORK, "NETWORK"), (OFF, "OFF")):
            b.configure(args.channel, args.pan, b_addr, mode)
            print(f"    B in {name} (PAN={args.pan:#06x} SADDR={b_addr:#06x}):"
                  f"{'  MODE NOT ACCEPTED' if not b.mode_ok else ''}")
            if not b.mode_ok:
                failures.append(True)
            for label, psdu, _ in classes:
                if label.startswith("a ") or label.startswith("b "):
                    expect = True
                elif name == "FULL":
                    expect = True
                elif name == "NETWORK":
                    expect = not label.startswith("d ")
                else:
                    expect = False
                failures.append(not run_cell(a, b, label, psdu, expect, args.channel))

        print("  3. TX-side ACK handling: A requests an acknowledgment, and A's own "
              "completion is the verdict")
        # The transceiver's ARET engine waits for the acknowledgment in hardware, so this
        # is where 6.5's "OK vs NO_ACK" becomes observable - and the rig asserts it in both
        # directions: a MAC node acknowledges, a promiscuous receiver must not.
        for mode, name, expect in ((OFF, "OFF: a MAC node acknowledges", 0),
                                   (NETWORK, "NETWORK: promiscuous, must not acknowledge", 17)):
            b.configure(args.channel, args.pan, b_addr, mode)
            a.drain()
            seq = 0x30 + mode
            tid = a.transmit(data_frame(seq, args.pan, b_addr, a_addr, b"\x06\x06",
                                        ack_request=True), args.channel)
            a.poll(1.5)
            value = a.by_tid.get(tid)
            code = value[0] if value else None
            ok = code == expect
            print(f"    {'PASS' if ok else 'FAIL'}  {name:44s} A's LAST_STATUS "
                  f"{code if code is not None else '-'} (expected {expect})")
            failures.append(not ok)

        print("  4. Frame Pending: what B's Ack to a poll says, per address, and what B's host is told")
        # B composes the Ack itself, in its radio vector, and takes Frame Pending from the
        # source-match table its host writes (docs/protocol.md 10.2). Each cell is one exchange:
        # A's completion carries the Frame Pending subfield of the Ack A received, and B's
        # notification carries MD_FLAG bit 4, what B put into that Ack. A transmits without
        # retries, so a NO_ACK is the link losing a frame, not a verdict: the cell repeats it.
        b.configure(args.channel, args.pan, b_addr, OFF)
        a_ext = bytes.fromhex("00124b0000a0a0a1")   # MSB first: Spinel 'E', MAC_15_4_LADDR order
        other_ext = bytes.fromhex("00124b0000a0a0a2")
        seq = [0x40]

        def exchange(psdu_for_seq):
            for _ in range(4):
                seq[0] = (seq[0] + 1) & 0xFF
                b.drain()
                a.drain()
                tid = a.transmit(psdu_for_seq(seq[0]), args.channel)
                a.poll(0.6)
                b.poll(0.4)
                value = a.by_tid.get(tid)
                if value and value[0] == 17:
                    continue
                flag = None
                for v in b.frames:
                    n = v[0] | (v[1] << 8)
                    if n > 2 and v[2 + 2] == seq[0]:
                        flag = v[2 + n + 2] | (v[2 + n + 3] << 8)
                fp = value[1] if value and value[0] == 0 and len(value) > 1 else None
                return fp, flag
            return None, None

        def poll_short(s):
            return command_frame(s, args.pan, b_addr, a_addr, b"\x04")

        def poll_ext(ext):
            return lambda s: command_frame_ext_src(s, args.pan, b_addr, ext, b"\x04")

        def data(s):
            return data_frame(s, args.pan, b_addr, a_addr, b"\x07\x07", ack_request=True)

        def set_prop(prop, value):
            failures.append(not b.set_checked(prop, value, value))

        cells = (
            ("table off: a poll is answered pending", None, poll_short, 1),
            ("table off: a data frame is answered 0", None, data, 0),
            ("table on, empty: poll answered 0", lambda: (set_prop(0x1303, b"\x01"), set_prop(0x1304, b"")), poll_short, 0),
            ("table on, A's short address in it", lambda: set_prop(0x1304, a_addr.to_bytes(2, "little")), poll_short, 1),
            ("table on, a data frame from A is answered 0", None, data, 0),
            ("table on, another short address only", lambda: set_prop(0x1304, (0x1234).to_bytes(2, "little")), poll_short, 0),
            ("table on, A's extended address in it", lambda: set_prop(0x1305, a_ext), poll_ext(a_ext), 1),
            ("table on, an extended address not in it", None, poll_ext(other_ext), 0),
            ("table off again: a poll is answered pending", lambda: (set_prop(0x1305, b""), set_prop(0x1304, b""), set_prop(0x1303, b"\x00")), poll_short, 1),
        )
        for label, setup, build, want_fp in cells:
            if setup:
                setup()
            fp, flag = exchange(build)
            want_flag = 0x0010 if want_fp else 0x0000
            ok = fp == want_fp and flag == want_flag
            print(f"    {'PASS' if ok else 'FAIL'}  {label:46s} A reads {fp if fp is not None else '-'}, "
                  f"B reports {('0x%04x' % flag) if flag is not None else '-'} "
                  f"(expected {want_fp}, 0x{want_flag:04x})")
            failures.append(not ok)
    finally:
        a.close()
        b.close()

    print(f"== {sum(failures)} failure(s)")
    return 1 if any(failures) else 0


if __name__ == "__main__":
    sys.exit(main())
