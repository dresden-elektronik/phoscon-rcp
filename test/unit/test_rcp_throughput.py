#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""test_rcp_throughput.py — tools/rcp_throughput.py's estimate and its loop, without a stick.

The estimate counts the bytes of the frames the tool sends, so it is checked against the reference
captures of docs/protocol.md §8 and the notification length of §7.2; the air term against the 802.15.4
figures it is built from. The loop runs against a fake RCP pair on socket pairs: a sender that answers
each request with its TID's completion and a receiver that reports the frame, with chosen frames
answered NO_ACK, reported twice, or acknowledged and never reported - each must land in its own count.
"""
import os
import socket
import sys
import threading
import types

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "tools"))
sys.modules.setdefault("serial", types.ModuleType("serial"))  # spinel_console asks for pyserial; nothing here opens a port

import rcp_throughput as tp  # noqa: E402
import spinel_console as sc  # noqa: E402

failures = 0


def check(cond, desc, detail=""):
    global failures
    if cond:
        print(f"  ok    {desc}")
    else:
        failures += 1
        print(f"  FAIL  {desc}" + (f"  [{detail}]" if detail else ""))


def estimate_cases():
    # docs/protocol.md §8: a 12-byte broadcast with a 4-byte trailer, and its deferred completion.
    req = bytes.fromhex("8b03710c00418801efbeffff00005a154a1a000000")
    wire = bytes.fromhex("7e8b03710c00418801efbeffff00005a154a1a0000001e7c7e")
    check(sc.frame(req) == wire, "the reference request encodes to the captured bytes")
    check(tp.wire_len(req, tp.HOST_ESCAPES) == len(wire) == 25, "the request's wire length is the capture's",
          tp.wire_len(req, tp.HOST_ESCAPES))
    check(tp.wire_len(bytes.fromhex("8b0600000000"), tp.RCP_ESCAPES) == 10, "the completion is 10 bytes")
    check(tp.wire_len(bytes.fromhex("8b03"), tp.HOST_ESCAPES) == tp.wire_len(bytes.fromhex("8b03"), tp.RCP_ESCAPES),
          "no escape, the same length both ways")
    check(tp.wire_len(b"\x11", tp.RCP_ESCAPES) == tp.wire_len(b"\x11", tp.HOST_ESCAPES) + 1,
          "0x11 is escaped by the RCP, not by the host")

    a, b = tp.Link("/dev/x@115200"), tp.Link("/dev/y")
    psdu = tp.psdu_for(1, 18, 0xBEE5)
    e = tp.estimate(psdu, tp.trailer(12, 3, True), a, b, True)
    check(len(psdu) == 18 and psdu[0] & 0x20, "the PSDU is the size asked and asks for an Ack")
    value = (18).to_bytes(2, "little") + psdu + bytes(tp.METADATA)
    check(len(value) == 18 + 28 and e["notified"] == tp.wire_len(bytes([0x80, 6, 0x71]) + value, tp.RCP_ESCAPES),
          "the notification's value is n + 28 (docs/protocol.md §7.2)", e["notified"])
    check(abs(e["stages"]["request"] - e["req"] * 10e6 / 115200) < 1e-6, "a UART byte is 10 bits")
    check(e["line_fps"] > e["sent_fps"], "the bridge's timer slows the sender below its line rate")
    check(tp.air_us(127, True) == 1120 + 128 + 192 + 133 * 32 + 192 + 11 * 32,
          "127 octets with CSMA-CA: backoff 3.5 units, CCA, turnaround, frame, AIFS, Ack", tp.air_us(127, True))
    check(tp.air_us(127, False) == tp.air_us(127, True) - 1120 - 128, "without CSMA-CA, no backoff and no CCA")
    slow = tp.estimate(tp.psdu_for(1, 127, 0xBEE5), tp.trailer(12, 3, True), b, tp.Link("/dev/z@57600"), True)
    check(slow["limit"] == "receiver's link" and abs(slow["fps"] - 1e6 / slow["receiver"]) < 1e-9,
          "a CDC sender into a 57 600 receiver is the receiver's link", slow["limit"])
    timed = tp.estimate(psdu, tp.trailer(12, 3, True), a, b, True)
    check(a.timer_us == 16000 and timed["cycle"] == 16000 and timed["stages"]["bridge"] > 0,
          "behind a bridge the cycle is a whole number of its 16 ms timer periods", timed["cycle"])
    untimed = tp.estimate(psdu, tp.trailer(12, 3, True), tp.Link("/dev/x@115200", 0), b, True)
    check(untimed["stages"]["bridge"] == 0 and abs(untimed["cycle"] - sum(untimed["stages"].values())) < 1e-6,
          "a timer of 0 leaves the line rate's cycle")
    check(not tp.Link("ssh://pi/dev/ttyAMA0@57600").bridged, "a UART on a Pi's header has no bridge")
    fast = tp.estimate(tp.psdu_for(1, 127, 0xBEE5), tp.trailer(12, 3, True), b, b, True)
    check(fast["limit"].endswith("the air"), "CDC to CDC is the air", fast["limit"])


class FakePair:
    """A as a fake RCP that completes each request, B as one that reports it, both on socket pairs."""

    def __init__(self, no_ack=(), twice=(), unreported=()):
        self.a_host, self.a_rcp = socket.socketpair()
        self.b_host, self.b_rcp = socket.socketpair()
        for s in (self.a_host, self.b_host):
            s.setblocking(False)
        self.no_ack, self.twice, self.unreported = set(no_ack), set(twice), set(unreported)
        self.stamp = 0
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        dec = sc.Decoder()
        while True:
            data = self.a_rcp.recv(4096)
            if not data:
                return
            for body in dec.feed(data):
                tid, n = body[0] & 0x0F, body[3] | (body[4] << 8)
                psdu = body[5:5 + n]
                counter = int.from_bytes(psdu[12:16], "little")
                status = tp.NO_ACK if counter in self.no_ack else tp.OK
                if counter not in self.unreported:
                    self.stamp += 4000
                    meta = bytearray(tp.METADATA)
                    meta[8:16] = self.stamp.to_bytes(8, "little")
                    note = sc.frame(bytes([0x80, sc.CMD_PROP_VALUE_IS, tp.P_STREAM_RAW]) + n.to_bytes(2, "little")
                                    + psdu + bytes(meta))
                    self.b_rcp.sendall(note * (2 if counter in self.twice else 1))
                self.a_rcp.sendall(sc.frame(bytes([0x80 | tid, sc.CMD_PROP_VALUE_IS, tp.P_LAST_STATUS, status, 0, 0])))

    def close(self):
        for s in (self.a_host, self.a_rcp, self.b_host, self.b_rcp):
            s.close()


def loop_cases():
    pair = FakePair(no_ack={3}, twice={5}, unreported={7})
    tx = tp.Wire(pair.a_host.fileno(), pair.a_host.fileno())
    rx = tp.Wire(pair.b_host.fileno(), pair.b_host.fileno())
    r = tp.run(tx, rx, 100, 0.3, 12, 0xBEE5, 3, True, settle=0.1)
    pair.close()
    sent, ok = r["sent"], r["statuses"].get(tp.OK, 0)
    check(sent > 20, "the loop sends back to back", sent)
    check(r["timeouts"] == 0 and sum(r["statuses"].values()) == sent, "every request got its own completion",
          r["statuses"])
    check(r["statuses"].get(tp.NO_ACK) == 1 and ok == sent - 1, "the NO_ACK frame is counted as one", r["statuses"])
    check(r["duplicates"] == 1, "the frame reported twice is one duplicate", r["duplicates"])
    check(r["acked"] - r["got"].keys() == {7}, "the frame acked and never reported is the only one missing",
          sorted(r["acked"] - r["got"].keys()))
    check(r["got"].keys() - r["acked"] == {3}, "the frame reported after a NO_ACK is reported without an OK")
    check(len(r["latencies"]) == sent and all(x >= 0 for x in r["latencies"]), "a latency per completion")


def main():
    print("rcp_throughput: the estimate")
    estimate_cases()
    print("rcp_throughput: the closed loop against a fake pair")
    loop_cases()
    print(f"== {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
