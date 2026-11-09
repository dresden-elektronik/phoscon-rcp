#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""rcp_throughput.py — how many acknowledged frames one RCP sends another per second, against the estimate.

A sends data frames to B that ask for an Ack, one at a time: the RCP has one transmit slot and answers a
second request BUSY (docs/protocol.md §7.1), so the sender is a closed loop - SET STREAM_RAW, wait for
that TID's completion, the next. B is a MAC node, promiscuous off, so it acknowledges each frame and
reports it to its host. Each payload carries a 32-bit counter, which tells B's duplicates (an Ack lost,
the frame retried) and missing frames apart; the sequence number wraps at 256.

For every PSDU size the measured rate is printed beside an estimate from the two links and the 2.4 GHz
O-QPSK timing of IEEE 802.15.4-2024:

  sender    the request's bytes on A's link, the air (CSMA-CA's mean first backoff and CCA, the turnaround,
            the frame, AIFS and the Ack) and the completion's bytes back; behind an FTDI bridge, rounded up
            to whole periods of its latency timer, since the bridge hands the host a short reply only
            when the timer runs out
  receiver  the notification's bytes on B's link, which overlap the sender's next cycle

B acknowledges in its radio whether or not its link keeps up, so A's rate is the sender's alone and
what reaches B's host the slower of the two; the stage that sets each is named. The byte counts are
the frames this tool sends, escapes included, at 10 bits a byte on a UART; a ConBee II's CDC link
counts as USB full-speed bulk, 19 packets of 64 bytes a millisecond. A local `path@baud` port is taken
to be behind an FTDI bridge (the ConBee III, the ConBee I) with the timer at --latency-timer-ms, 16 ms
unless given; on macOS the tool sets that timer for the run and puts 16 back afterwards.
Measured beside it: the completion latency's percentiles, B's radio cadence from its receive stamps,
and how far B's host fell behind its radio - a lag that grows is notifications queueing on B's link.

  python3 tools/rcp_throughput.py --a <cb2-port> --b <cb3-port>@115200 --channel 12
  python3 tools/rcp_throughput.py --a <cb2-port> --b <cb1-port>@57600 --channel 12 --both
  python3 tools/rcp_throughput.py --a <cb2-port> --b <cb1-port>@57600 --estimate

Transmits for --seconds per size and direction without a pause: a quiet channel and a private PAN.
"""
import argparse
import fcntl
import math
import os
import select
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rcp_matrix  # noqa: E402
import rcp_pair  # noqa: E402
import spinel_console as sc  # noqa: E402

P_LAST_STATUS, P_PROTOCOL_VERSION, P_NCP_VERSION, P_STREAM_RAW = 0x00, 0x01, 0x02, 0x71
CMD_RESET = 1
OK, FAILURE, BUSY, NO_ACK, CCA_FAILURE = 0, 1, 12, 17, 18
STATUS_NAMES = {OK: "OK", FAILURE: "FAILURE", BUSY: "BUSY", NO_ACK: "NO_ACK", CCA_FAILURE: "CCA_FAILURE"}
A_ADDR, B_ADDR = 0x0A01, 0x0B02
MAGIC = b"TP"
MHR_FCS = 11                    # PAN-compressed, both addresses short: 9 octets of header, 2 of FCS
PSDU_MIN = MHR_FCS + len(MAGIC) + 1 + 4
METADATA = 26                   # the RX notification's bytes after the PSDU (docs/protocol.md §7.2)

# IEEE 802.15.4-2024, the 2.4 GHz O-QPSK PHY: 62.5 ksymbol/s, two symbols an octet.
SYMBOL_US = 16
OCTET_US = 2 * SYMBOL_US
SHR_PHR = 6                     # preamble 4, SFD 1, PHR 1
TURNAROUND_US = 12 * SYMBOL_US  # aTurnaroundTime
CCA_US = 8 * SYMBOL_US          # phyCcaDuration
UNIT_BACKOFF_US = TURNAROUND_US + CCA_US
MIN_BE = 3                      # macMinBe
AIFS_US = 12 * SYMBOL_US        # macSifsPeriod
ACK_PSDU = 5
HOST_ESCAPES = (0x7E, 0x7D)
RCP_ESCAPES = (0x7E, 0x7D, 0x11, 0x13)
FTDI_TIMER_MS = 16              # an FTDI bridge's latency timer as it powers up
IOSSDATALAT = 0x80085400        # macOS _IOW('T', 0, unsigned long); its FTDI driver takes the timer in ms, 8 bits


class Link:
    """What a port spec says about its link: a UART's line rate, or a CDC pipe at USB full speed, and
    for a local UART the latency timer of the bridge in front of it."""

    def __init__(self, spec, timer_ms=None):
        _, sep, baud = spec.rpartition("@")
        self.baud = int(baud) if sep else 0
        self.byte_us = 10e6 / self.baud if self.baud else 1000.0 / (19 * 64)
        self.bridged = bool(sep) and not spec.startswith("ssh://")
        self.timer_us = (FTDI_TIMER_MS if timer_ms is None else timer_ms) * 1000.0 if self.bridged else 0.0

    def __str__(self):
        if not self.baud:
            return "USB CDC"
        return f"UART {self.baud}" + (f" behind a bridge timing out at {self.timer_us / 1000:g} ms" if self.bridged else "")


def wire_len(payload, escapes):
    """An HDLC frame's length on the wire: two flags, the payload and its FCS, each escaped byte doubled."""
    crc = sc.crc16(payload)
    body = bytes(payload) + bytes([crc & 0xFF, crc >> 8])
    return 2 + len(body) + sum(1 for b in body if b in escapes)


def psdu_for(counter, n, pan, run_id=0):
    payload = MAGIC + bytes([run_id & 0xFF]) + (counter & 0xFFFFFFFF).to_bytes(4, "little")
    return rcp_pair.data_frame(counter & 0xFF, pan, B_ADDR, A_ADDR, payload + b"\x55" * (n - MHR_FCS - len(payload)),
                               ack_request=True)


def trailer(channel, retries, csma):
    """txChannel, maxCsmaBackoffs, maxRetries, enableCSMACA; the rest takes its default (docs/protocol.md §7.1)."""
    return bytes([channel, 4, retries, 1 if csma else 0])


def request(tid, psdu, tail):
    return bytes([0x80 | tid, sc.CMD_PROP_VALUE_SET]) + sc.pack_uint(P_STREAM_RAW) + len(psdu).to_bytes(2, "little") \
        + psdu + tail


def air_us(n, csma):
    """One exchange on the air: the mean first backoff and its CCA, the turnaround, the frame, AIFS, the Ack."""
    access = ((2 ** MIN_BE - 1) / 2.0) * UNIT_BACKOFF_US + CCA_US if csma else 0.0
    return access + TURNAROUND_US + (SHR_PHR + n) * OCTET_US + AIFS_US + (SHR_PHR + ACK_PSDU) * OCTET_US


def estimate(psdu, tail, a, b, csma):
    """The model for one frame, in µs, with the rate it allows and the stage that sets it."""
    n = len(psdu)
    req = wire_len(request(1, psdu, tail), HOST_ESCAPES)
    done = wire_len(bytes([0x81, sc.CMD_PROP_VALUE_IS, P_LAST_STATUS, OK, 0, 0]), RCP_ESCAPES)
    notified = wire_len(bytes([0x80, sc.CMD_PROP_VALUE_IS, P_STREAM_RAW]) + n.to_bytes(2, "little") + psdu
                        + bytes(METADATA), RCP_ESCAPES)
    stages = {"request": req * a.byte_us, "air": air_us(n, csma), "completion": done * a.byte_us}
    line = sum(stages.values())
    cycle = math.ceil(line / a.timer_us) * a.timer_us if a.timer_us else line
    stages["bridge"] = cycle - line
    receiver = notified * b.byte_us
    sender = "the bridge's timer" if stages["bridge"] > max(stages["request"], stages["air"]) else \
        f"the {max(('request', 'air'), key=stages.get)}"
    limit = "receiver's link" if receiver > cycle else f"sender's cycle, most of it {sender}"
    return dict(n=n, req=req, done=done, notified=notified, stages=stages, cycle=cycle, receiver=receiver,
                line_fps=1e6 / line, sent_fps=1e6 / cycle, fps=1e6 / max(cycle, receiver), limit=limit)


class Wire:
    """A node's bytes for the timed loop, without the setup path's pacing: whole writes that wait for
    room, reads on select, every decoded frame stamped with the moment its bytes were read."""

    def __init__(self, rfd, wfd):
        self.rfd, self.wfd = rfd, wfd
        self.dec = sc.Decoder()

    @classmethod
    def of(cls, node):
        s = node.ser
        if isinstance(s, rcp_matrix.SshStream):
            return cls(s.proc.stdout.fileno(), s.proc.stdin.fileno())
        return cls(s.fileno(), s.fileno())

    def write(self, data):
        # A non-blocking tty takes part of a large write and returns its count; the rest is sent here,
        # not dropped.
        view = memoryview(data)
        while view:
            select.select([], [self.wfd], [], 1.0)
            try:
                view = view[os.write(self.wfd, view):]
            except BlockingIOError:
                pass

    def read(self):
        try:
            data = os.read(self.rfd, 4096)
        except (BlockingIOError, InterruptedError):
            return []
        now = time.perf_counter()
        return [(now, e) for e in map(sc.classify, self.dec.feed(data)) if e]


def ours(value, pan, run_id):
    """The counter of a notification that is one of this run's frames, or None."""
    p = rcp_pair.psdu_of(value)
    if len(p) < PSDU_MIN or p[3:7] != bytes([pan & 0xFF, pan >> 8, B_ADDR & 0xFF, B_ADDR >> 8]):
        return None
    if p[9:12] != MAGIC + bytes([run_id & 0xFF]):
        return None
    return int.from_bytes(p[12:16], "little")


def radio_stamp(value):
    n = value[0] | (value[1] << 8)
    return int.from_bytes(value[2 + n + 8:2 + n + 16], "little") if len(value) >= 2 + n + 16 else None


def since(ts, ts0):
    """µs from one receive stamp to another; a stamp may wrap at 32 bits (docs/protocol.md §7.2)."""
    return ((ts - ts0 + (1 << 31)) & 0xFFFFFFFF) - (1 << 31)


def percentile(values, q):
    if not values:
        return float("nan")
    s = sorted(values)
    return s[min(len(s) - 1, int(q * len(s)))]


def quiet(rx, pan, idle=0.2, limit=5.0):
    """Read B until its link has been idle for `idle`: the reports of the run before are not this one's."""
    end = time.perf_counter() + limit
    while time.perf_counter() < end:
        if not select.select([rx.rfd], [], [], idle)[0]:
            return
        rx.read()


def run(tx, rx, n, seconds, channel, pan, retries, csma, run_id=0, settle=0.3):
    """One size in one direction: the closed loop for `seconds`, then B's last reports until it is quiet."""
    tail = trailer(channel, retries, csma)
    quiet(rx, pan)
    statuses, latencies, acked, got = {}, [], set(), {}
    duplicates = timeouts = 0
    tid, counter, pending = 0, 0, None
    start = time.perf_counter()
    stop = start + seconds
    last = start
    while True:
        now = time.perf_counter()
        if pending is None:
            if now >= stop:
                break
            tid = tid % 15 + 1
            counter += 1
            sent = time.perf_counter()
            tx.write(sc.frame(request(tid, psdu_for(counter, n, pan, run_id), tail)))
            pending = (tid, sent, counter)
        elif now - pending[1] > 1.0:
            timeouts += 1
            pending = None
            continue
        ready, _, _ = select.select([tx.rfd, rx.rfd], [], [], 0.05)
        if tx.rfd in ready:
            for at, e in tx.read():
                if pending and e["prop"] == P_LAST_STATUS and e["tid"] == pending[0] and "last_status" in e:
                    code = e["last_status"]
                    statuses[code] = statuses.get(code, 0) + 1
                    latencies.append(at - pending[1])
                    if code == OK:
                        acked.add(pending[2])
                    last = at
                    pending = None
        if rx.rfd in ready:
            duplicates += take(rx, pan, got, run_id)
    # B's link may still be carrying reports it queued: read until it has been idle for `settle`.
    while select.select([rx.rfd], [], [], settle)[0]:
        duplicates += take(rx, pan, got, run_id)
    return dict(n=n, start=start, elapsed=last - start, sent=counter, statuses=statuses, timeouts=timeouts,
                latencies=latencies, acked=acked, got=got, duplicates=duplicates)


def take(rx, pan, got, run_id):
    """B's notifications into `got`, counter -> (host time, radio stamp); returns the duplicates seen."""
    duplicates = 0
    for at, e in rx.read():
        if e["prop"] != P_STREAM_RAW or e["tid"] != 0:
            continue
        counter = ours(e["value"], pan, run_id)
        if counter is None:
            continue
        if counter in got:
            duplicates += 1
        else:
            got[counter] = (at, radio_stamp(e["value"]))
    return duplicates


def report(r, est):
    ok = r["statuses"].get(OK, 0)
    fps = ok / r["elapsed"] if r["elapsed"] > 0 else 0.0
    payload = r["n"] - MHR_FCS
    lat = [x * 1000 for x in r["latencies"]]
    codes = "  ".join(f"{STATUS_NAMES.get(c, c)} {k}" for c, k in sorted(r["statuses"].items()))
    s = est["stages"]
    arrivals = [at for at, _ in r["got"].values()]
    span = max(arrivals) - r["start"] if arrivals else 0.0
    delivered = len(arrivals) / span if span > 0 else 0.0
    print(f"  PSDU {r['n']:3d}  estimate: A sends {est['sent_fps']:6.1f} fps, B's host gets {est['fps']:6.1f}, set by the"
          f" {est['limit']} (request {s['request'] / 1000:.2f} + air {s['air'] / 1000:.2f}"
          f" + completion {s['completion'] / 1000:.2f} + bridge {s['bridge'] / 1000:.2f} ms;"
          f" each report {est['receiver'] / 1000:.2f} ms on B's link)")
    print(f"            measured: A sends {fps:6.1f} fps = {fps / est['sent_fps'] * 100:5.1f} %, B's host gets"
          f" {delivered:6.1f} = {delivered / est['fps'] * 100:5.1f} %: {delivered * payload * 8 / 1000:.1f} kbit/s of payload")
    timeouts = f", {r['timeouts']} without a completion in 1 s" if r["timeouts"] else ""
    print(f"            {r['sent']} sent: {codes or 'no completion'}{timeouts}")
    unreported = len(r["acked"] - r["got"].keys())
    unacked = len(r["got"].keys() - r["acked"])
    print(f"            B reported {len(r['got'])} once, {r['duplicates']} again; acked but not reported {unreported},"
          f" reported without an OK {unacked}")
    if lat:
        print(f"            completion after the request's first byte: p50 {percentile(lat, .5):.2f}"
              f"  p90 {percentile(lat, .9):.2f}  p99 {percentile(lat, .99):.2f}  max {max(lat):.2f} ms;"
              f" sender cycle {r['elapsed'] / max(1, sum(r['statuses'].values())) * 1000:.2f} ms")
    stamped = sorted((c, at, ts) for c, (at, ts) in r["got"].items() if ts is not None)
    if len(stamped) > 1:
        _, at0, ts0 = stamped[0]
        gaps = [since(b[2], a[2]) / 1000.0 for a, b in zip(stamped, stamped[1:]) if b[0] == a[0] + 1]
        lag = [(at - at0) * 1000 - since(ts, ts0) / 1000.0 for _, at, ts in stamped]
        print(f"            B's radio, frame to next frame: p50 {percentile(gaps, .5):.2f} ms; B's host behind its radio by"
              f" {lag[-1] - min(lag):.1f} ms at the end, {max(lag) - min(lag):.1f} at most")


def latency_timer(node, ms):
    """The bridge's latency timer. The driver keeps it past the port's close, so the run puts it back."""
    if sys.platform == "darwin":
        fcntl.ioctl(node.ser.fileno(), IOSSDATALAT, struct.pack("L", ms))
    else:
        print(f"{node.name}: set the timer as root: echo {ms} > /sys/bus/usb-serial/devices/<ttyUSBn>/latency_timer")


def reset(node):
    node.drain()
    node.send(bytes([0x80 | 1, CMD_RESET, 2]))
    node.poll(0.6)


def identify(node):
    node.drain()
    for _ in range(10):
        node.request(sc.CMD_PROP_VALUE_GET, P_PROTOCOL_VERSION)
        node.request(sc.CMD_PROP_VALUE_GET, P_NCP_VERSION)
        node.poll(0.5)
        if node.values.get(P_NCP_VERSION):
            return node.values[P_NCP_VERSION].rstrip(b"\x00").decode(errors="replace")
    return None


def direction(tx, rx, links, args, sizes, first_id):
    print(f"{tx.name} -> {rx.name}: {tx.name} on {links[tx.name]}, {rx.name} on {links[rx.name]}, channel {args.channel},"
          f" PAN {args.pan:#06x}, CSMA-CA {'off' if args.no_csma else 'on'}, {args.retries} retries")
    tx.configure(args.channel, args.pan, A_ADDR, rcp_pair.OFF)
    rx.configure(args.channel, args.pan, B_ADDR, rcp_pair.OFF)
    if not (tx.mode_ok and rx.mode_ok):
        print("  a SET did not take; nothing measured")
        return 1
    wtx, wrx = Wire.of(tx), Wire.of(rx)
    for i, n in enumerate(sizes):
        r = run(wtx, wrx, n, args.seconds, args.channel, args.pan, args.retries, not args.no_csma, first_id + i)
        report(r, estimate(psdu_for(0, n, args.pan), trailer(args.channel, args.retries, not args.no_csma),
                           links[tx.name], links[rx.name], not args.no_csma))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--a", required=True, help="the sending RCP's port (path@baud for a UART RCP, or ssh://)")
    ap.add_argument("--b", required=True, help="the receiving RCP's port")
    ap.add_argument("--channel", type=int, help="the test channel; there is no default, since the run fills it")
    ap.add_argument("--pan", type=lambda x: int(x, 0), default=0xBEE5, help="a private test PAN")
    ap.add_argument("--sizes", default="18,32,64,100,127", help="PSDU lengths with their FCS, 18..127")
    ap.add_argument("--seconds", type=float, default=5.0, help="per size and direction")
    ap.add_argument("--retries", type=int, default=3, help="maxRetries in each request")
    ap.add_argument("--no-csma", action="store_true", help="enableCSMACA 0: no backoff and no CCA")
    ap.add_argument("--both", action="store_true", help="then B sends to A")
    ap.add_argument("--latency-timer-ms", type=int, help=f"the FTDI bridges' latency timer, 1..255; {FTDI_TIMER_MS} unless"
                    " given, and set for the run on macOS")
    ap.add_argument("--estimate", action="store_true", help="print the estimate only; no port is opened")
    args = ap.parse_args(argv)

    sizes = [int(x) for x in args.sizes.split(",")]
    if any(not PSDU_MIN <= n <= 127 for n in sizes):
        ap.error(f"--sizes: each PSDU is {PSDU_MIN}..127 octets")
    if args.latency_timer_ms is not None and not 1 <= args.latency_timer_ms <= 255:
        ap.error("--latency-timer-ms: 1..255")
    links = {"A": Link(args.a, args.latency_timer_ms), "B": Link(args.b, args.latency_timer_ms)}
    if args.estimate:
        csma = not args.no_csma
        pairs = (("A", "B"), ("B", "A")) if args.both else (("A", "B"),)
        for s, d in pairs:
            print(f"{s} -> {d}: {s} on {links[s]}, {d} on {links[d]}, CSMA-CA {'on' if csma else 'off'}")
            for n in sizes:
                e = estimate(psdu_for(0, n, args.pan), trailer(args.channel or 11, args.retries, csma),
                             links[s], links[d], csma)
                s_ = e["stages"]
                print(f"  PSDU {n:3d}  A sends {e['sent_fps']:6.1f} fps ({e['line_fps']:.1f} on the line rates alone),"
                      f" B's host gets {e['fps']:6.1f}, set by the {e['limit']}: request {e['req']} B"
                      f" {s_['request'] / 1000:.2f} ms, air {s_['air'] / 1000:.2f}, completion {e['done']} B"
                      f" {s_['completion'] / 1000:.2f}, bridge {s_['bridge'] / 1000:.2f}; notification {e['notified']} B"
                      f" {e['receiver'] / 1000:.2f} ms")
        return 0
    if args.channel is None or not 11 <= args.channel <= 26:
        ap.error("--channel 11..26 is required to transmit")

    a, b = rcp_matrix.open_node("A", args.a), rcp_matrix.open_node("B", args.b)
    timed = [n for n in (a, b) if links[n.name].bridged and args.latency_timer_ms is not None]
    fails = 0
    try:
        for node in timed:
            latency_timer(node, args.latency_timer_ms)
        for node in (a, b):
            reset(node)
            ident = identify(node)
            print(f"{node.name}: {node.port}: {ident or 'NO ANSWER'}")
            fails += ident is None
        if fails:
            return 1
        run_id = int(time.time()) & 0x7F
        fails += direction(a, b, links, args, sizes, run_id)
        if args.both:
            fails += direction(b, a, links, args, sizes, run_id + len(sizes))
    finally:
        for node in timed:
            latency_timer(node, FTDI_TIMER_MS)
        for node in (a, b):
            reset(node)
            node.close()
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
