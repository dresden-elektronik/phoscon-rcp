#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""rcp_energy_sweep.py — a per-channel radio profile over Spinel, plus the harness for a real energy scan.

Two modes, one tool, because the plan needs both and they must be comparable:

  --mode rssi   retunes 11..26 one channel at a time and counts what the receiver actually
                hears, with the RSSI the RCP reports for each frame. This is the
                **calibration baseline**: it is received-frame RSSI (the Frame Buffer's ED
                byte, the energy over that frame's reception, 1 dB steps), not a requested
                energy measurement, so it is compared against an ED sweep in *channel shape
                and order*, never as an absolute dBm agreement (docs/hardware_samr21.md §6).

  --mode scan   the host sequence of docs/protocol.md 10.1: SET MAC_SCAN_MASK with one channel,
                SET MAC_SCAN_PERIOD, SET MAC_SCAN_STATE = 2, then wait for the unsolicited
                VALUE_IS MAC_ENERGY_SCAN_RESULT and the VALUE_IS MAC_SCAN_STATE = 0 that ends the
                dwell. This is the acceptance harness: with a peer transmitting on one channel, that
                channel must come back loudest and a channel with nothing on it must come back near
                the floor. Asking for a whole band in one SET is refused, exactly as a stock
                OpenThread RCP refuses it, and the script prints that refusal rather than working
                around it.

Per-channel arming is read back, not assumed - the rule of docs/testing.md §3: a mode that
never took is indistinguishable from a filter that leaks.

The transmitter is optional: a second stick pushing frames on one channel makes the profile
meaningful, and sweeping the receiver across neighbouring channels is what shows the leakage.
The known environment on this bench is a live network on channel 15 with PAN 0xde20 (audible
on 20 as well) and four foreign PANs on 25 - the shape this tool must reproduce.

  .venv/bin/python tools/rcp_energy_sweep.py --port <cb2-port-b> \\
      --tx-port <cb2-port> --tx-channel 20 --dwell 250
  .venv/bin/python tools/rcp_energy_sweep.py --port <cb2-port-b> \\
      --mode scan --channels 11-26 --period 128
"""
import argparse
import os
import statistics
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
import rcp_pair as rp  # noqa: E402  (the rig already owns framing, arming and TX)
import spinel_console as sc  # noqa: E402

P_PHY_ENABLED, P_PHY_CHAN, P_PROMISC, P_RAW_STREAM = 0x20, 0x21, 0x38, 0x37
P_PROTOCOL_VERSION, P_STREAM_RAW = 0x01, 0x71
P_SCAN_STATE, P_SCAN_MASK, P_SCAN_PERIOD, P_SCAN_RESULT = 0x30, 0x31, 0x32, 0x39

SPINEL_STATUS_NAMES = {
    0: "OK", 2: "UNIMPLEMENTED", 3: "INVALID_ARGUMENT", 4: "INVALID_STATE",
    9: "PARSE_ERROR", 11: "NOMEM", 12: "BUSY", 13: "PROP_NOT_FOUND",
}

PROMISC = {"off": 0, "network": 1, "node": 1, "full": 2}


def s8(value):
    return value - 256 if value > 127 else value


def parseChannels(spec):
    """`11-26`, `11,15,20` or a mix."""
    out = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            lo, hi = part.split("-", 1)
            out.extend(range(int(lo, 0), int(hi, 0) + 1))
        else:
            out.append(int(part, 0))
    return [c for c in out if 11 <= c <= 26]


def hasSpinelSession(node):
    """A probe image enumerates identically and answers nothing over Spinel: ask first."""
    tid = node.request(sc.CMD_PROP_VALUE_GET, P_PROTOCOL_VERSION)
    for _ in range(10):
        node.poll(0.15)
        if P_PROTOCOL_VERSION in node.values or node.statuses:
            return True
    return False


def rxFrame(entry):
    """One STREAM_RAW value -> (len, rssi, lqi, channel, badFcsFlag). docs/protocol.md §7.2."""
    value = entry["value"]
    if len(value) < 2:
        return None
    flen = value[0] | (value[1] << 8)
    meta = value[2 + flen:]
    if len(meta) < 8:
        return None
    return (flen, s8(meta[0]), meta[7], meta[6], meta[2] | (meta[3] << 8))


def scanResult(value):
    """VALUE_IS MAC_SCAN_RESULT = `Cc`: channel, dBm."""
    if len(value) < 2:
        return None
    return value[0], s8(value[1])


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", required=True, help="the receiving RCP (always pass it explicitly)")
    ap.add_argument("--mode", choices=("rssi", "scan", "ab"), default="rssi",
                    help="ab sweeps the band twice - peer silent, then peer transmitting - and "
                         "reports the level change per channel")
    ap.add_argument("--channels", default="11-26")
    ap.add_argument("--dwell", type=int, default=250, help="ms per channel, --mode rssi")
    ap.add_argument("--period", type=int, default=128, help="ms per channel, --mode scan (MAC_SCAN_PERIOD)")
    ap.add_argument("--promisc", default="full", choices=sorted(PROMISC))
    ap.add_argument("--pan", default="0xffff", help="frame filter for the receiver; 0xffff = nothing armed")
    ap.add_argument("--tx-port", help="a second RCP that transmits while the receiver sweeps")
    ap.add_argument("--tx-channel", type=int, default=20)
    ap.add_argument("--tx-interval", type=int, default=80, help="ms between transmitted frames")
    ap.add_argument("--tx-frame", default="41 20 00 be de ff ff 01 00 00 00 00 02 5a",
                    help="PSDU hex for the transmitter (a short data frame by default)")
    args = ap.parse_args()

    channels = parseChannels(args.channels)
    if not channels:
        print("no channels in range: --channels expects 11..26")
        return 2

    rx = rp.Node(args.port, "rx")
    tx = rp.Node(args.tx_port, "tx") if args.tx_port else None
    try:
        if not hasSpinelSession(rx):
            print(f"!! {args.port}: no Spinel answer. A probe-image stick enumerates the same "
                  f"and says nothing here - check which image this port carries.")
            return 2

        # One arming for the whole sweep; only the channel changes per step. PAN 0xffff means
        # "nothing armed", which is also what lets a foreign-PAN beacon through a filtered
        # receiver (docs/hardware_samr21.md §6, the IEEE third-level rules).
        rx.configure(channels[0], int(args.pan, 0), 0xFFFF, PROMISC[args.promisc])
        if tx is not None:
            tx.configure(args.tx_channel, 0xdead, 0x0001, PROMISC["off"])
            print(f"tx {args.tx_port} on channel {args.tx_channel}, one frame every "
                  f"{args.tx_interval} ms")

        if args.mode == "scan":
            return sweepScan(rx, channels, args.period, tx, args)

        if args.mode == "ab":
            if tx is None:
                print("--mode ab needs --tx-port: the point is a channel that changes")
                return 2
            quiet = {}
            busy = {}
            code = sweepScan(rx, channels, args.period, None, args, collect=quiet,
                             label="peer silent")
            print()
            code2 = sweepScan(rx, channels, args.period, tx, args, collect=busy,
                              label=f"peer transmitting on channel {args.tx_channel} "
                                    f"every {args.tx_interval} ms")
            print()
            print(f"== delta, dBm, same rig minutes apart "
                  f"(the peer is on channel {args.tx_channel})")
            print(f"{'chan':>5} {'silent':>7} {'peer on':>8} {'delta':>7}")
            best = None
            for ch in channels:
                if ch in quiet and ch in busy:
                    d = busy[ch] - quiet[ch]
                    mark = "  <== peer here" if ch == args.tx_channel else ""
                    print(f"{ch:>5} {quiet[ch]:>7} {busy[ch]:>8} {d:>+7}{mark}")
                    if best is None or d > best[1]:
                        best = (ch, d)
            if best is not None:
                print(f"== largest rise: channel {best[0]}, {best[1]:+d} dB")
                print(f"== peer channel rose {busy[args.tx_channel] - quiet.get(args.tx_channel, 0):+d} dB "
                      f"where the peer is; a scan that cannot tell those two apart is not measuring")
            return 0 if code == 0 and code2 == 0 else 2

        return sweepRssi(rx, channels, args.dwell, tx, args)
    finally:
        rx.close()
        if tx is not None:
            tx.close()


def pumpRx(node, seconds, tx, psdu, txChannel, txInterval):
    """Poll `node` for `seconds`, transmitting on `tx` every txInterval ms while waiting."""
    seen = []
    deadline = time.time() + seconds
    nextTx = time.time()
    while time.time() < deadline:
        if tx is not None and time.time() >= nextTx:
            tx.transmit(psdu, txChannel)
            nextTx = time.time() + txInterval / 1000.0
        before = len(node.frames)
        node.poll(0.05)
        for entry in node.frames[before:]:
            parsed = rxFrame({"value": entry})
            if parsed is not None:
                seen.append(parsed)
    return seen


def sweepRssi(rx, channels, dwellMs, tx, args):
    psdu = bytes.fromhex(args.tx_frame.replace(" ", "")) if tx else None
    failures = 0
    print(f"== received-frame RSSI profile, {dwellMs} ms per channel, promiscuous "
          f"{args.promisc}, pan {args.pan}")
    print(f"{'chan':>5} {'frames':>7} {'min':>5} {'med':>5} {'max':>5} {'maxLQI':>7}  note")
    profile = {}
    for ch in channels:
        if not rx.set_checked(P_PHY_CHAN, bytes([ch]), bytes([ch])):
            print(f"{ch:>5} {'-':>7}   retune refused: the channel write did not take")
            failures += 1
            continue  # the retune is the whole experiment for this channel
        rx.drain()
        seen = pumpRx(rx, dwellMs / 1000.0, tx, psdu, args.tx_channel, args.tx_interval)
        if not seen:
            print(f"{ch:>5} {0:>7} {'-':>5} {'-':>5} {'-':>5} {'-':>7}  nothing heard")
            profile[ch] = None
            continue
        rssi = [s[1] for s in seen]
        lqi = max(s[2] for s in seen)
        loud = "  <== transmitter here" if ch == args.tx_channel else ""
        print(f"{ch:>5} {len(seen):>7} {min(rssi):>5} "
              f"{int(statistics.median(rssi)):>5} {max(rssi):>5} {lqi:>7}  "
              f"span {max(rssi) - min(rssi)} dB{loud}")
        profile[ch] = max(rssi)

    if profile:
        top = max((v for v in profile.values() if v is not None), default=None)
        if top is None:
            print("== the sweep heard nothing on any channel: check the transmitter and the ports")
            failures += 1
        else:
            loud = [c for c, v in profile.items() if v is not None and v >= top - 5]
            print(f"== loudest channels (within 5 dB of {top} dBm): {loud}")
    return 1 if failures else 0


def sweepScan(rx, channels, periodMs, tx, args, collect=None, label=""):
    """One dwell per channel: SET mask, SET period, SET state = 2, then the result and the end.

    The peer transmits *during* the waits, because the gate this harness exists for is that the channel
    the peer is on comes back loudest: a scan measured against an empty room proves the plumbing, not
    the measurement."""
    psdu = bytes.fromhex(args.tx_frame.replace(" ", "")) if tx is not None else None
    print(f"== energy scan, one dwell per channel at MAC_SCAN_PERIOD {periodMs} ms, "
          f"{label or 'peer silent'} (docs/protocol.md 10.1)")
    print(f"{'chan':>5} {'answer':>18} {'dBm':>6}  note")
    results = {}
    refused = {}
    completed = 0
    for ch in channels:
        rx.drain()
        tidMask = rx.request(sc.CMD_PROP_VALUE_SET, P_SCAN_MASK, bytes([1, ch]))
        rx.poll(0.2)
        tidPeriod = rx.request(sc.CMD_PROP_VALUE_SET, P_SCAN_PERIOD,
                               bytes([periodMs & 0xFF, (periodMs >> 8) & 0xFF]))
        rx.poll(0.2)
        tidState = rx.request(sc.CMD_PROP_VALUE_SET, P_SCAN_STATE, b"\x02")

        bad = None
        for tid, name in ((tidMask, "MASK"), (tidPeriod, "PERIOD")):
            if rx.by_tid.get(tid):
                bad = (name, rx.by_tid[tid][0])
                print(f"{ch:>5} {name + ' refused':>18} {'':>6}  LAST_STATUS "
                      f"{SPINEL_STATUS_NAMES.get(bad[1], bad[1])}")
                refused[ch] = bad[1]
                break

        if bad is None:
            result = None
            echo = None
            ended = False
            nextTx = 0.0
            deadline = time.time() + periodMs / 1000.0 + 3.0
            while time.time() < deadline:
                if tx is not None and time.time() >= nextTx:
                    tx.transmit(psdu, args.tx_channel)
                    nextTx = time.time() + args.tx_interval / 1000.0
                rx.poll(0.05)
                if P_SCAN_RESULT in rx.values:
                    result = scanResult(rx.values.pop(P_SCAN_RESULT))
                if P_SCAN_STATE in rx.values:
                    reported = rx.values[P_SCAN_STATE]
                    if reported == b"\x00":
                        ended = True
                    elif echo is None:
                        echo = reported
                if tidState in rx.by_tid:
                    code = rx.by_tid[tidState][0]
                    print(f"{ch:>5} {'refused':>18} {'':>6}  LAST_STATUS "
                          f"{SPINEL_STATUS_NAMES.get(code, code)}")
                    refused[ch] = code
                    break
                if result is not None and ended:
                    break

            # A dwell that ran is answered three times: the echo of the start, the result, and the
            # completion. The echo and the completion share a property id, and a single read can carry
            # both, so the completion counts as evidence too - 0x30 = 0 cannot appear unless a scan was
            # running to end.
            if ch not in refused:
                if echo not in (b"\x02", None) and not ended and result is None:
                    print(f"{ch:>5} {'bad echo':>18} {'':>6}  SET 0x30 answered "
                          f"{echo.hex()}, expected 02")
                    refused[ch] = None
                elif result is None:
                    print(f"{ch:>5} {'no result':>18} {'':>6}  "
                          f"echo {echo.hex() if echo else '-'} ended {int(ended)}, "
                          f"no 0x39 within {periodMs + 3000} ms")
                    refused[ch] = None
                else:
                    resCh, dbm = result
                    completed += 1 if ended else 0
                    note = f"channel {resCh}" + ("" if ended else ", no completion seen")
                    print(f"{ch:>5} {'VALUE_IS 0x39':>18} {dbm:>6}  {note}")
                    if dbm != 127:
                        results[ch] = dbm

    if collect is not None:
        collect.update(results)

    if results:
        top = max(results.values())
        floor = min(results.values())
        print(f"== {len(results)} of {len(channels)} channels measured; loudest {top} dBm, "
              f"quietest {floor} dBm, spread {top - floor} dB")
        print(f"== loudest (within 5 dB of the top): "
              f"{[c for c, v in results.items() if v >= top - 5]}")
        print(f"== completions seen (VALUE_IS 0x30 = 0): {completed} of {len(results)}")
    if refused:
        print(f"== {len(refused)} channel(s) refused or unanswered: {sorted(refused)} ("
              f"{', '.join(SPINEL_STATUS_NAMES.get(v, 'no answer') for v in refused.values())})")
    return 0 if not refused else 2


if __name__ == "__main__":
    sys.exit(main())
