#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""spinel_console.py — drive this RCP over CDC and decode what comes back.

A small, in-tree way to (a) arm the radio with the arming sequence of docs/protocol.md §6,
(b) print every frame that arrives with its stream decoded, and (c) do it without depending on
another repo being present. It is also the only reader that decodes STREAM_DEBUG (0x70), which is
how this firmware's boot records come out when the build is DS_MODE=rcp.

  .venv/bin/python tools/spinel_console.py --port <cb2-port> \
      --channel 15 --promisc full --duration 20 [--no-arm] [--hex]

Pass --no-arm to just listen, and --allow-tx to enable the (default-off) TX step.

On a UART link (the RaspBee II's) pass --baud: a CDC pipe's rate is the bus's, a serial
line's is ours to set.
"""
import argparse
import os
import sys
import termios
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial missing; use .venv/bin/python")

FLAG = 0x7E
ESC = 0x7D
ESC_XOR = 0x20

CMD_PROP_VALUE_GET = 2
CMD_PROP_VALUE_SET = 3
CMD_PROP_VALUE_IS = 6
CMD_PROP_VALUE_INSERTED = 7
CMD_PROP_VALUE_REMOVED = 8

TYPE_COMMAND = 0x80
TYPE_RESPONSE = 0x40

# docs/protocol.md §10 (id table corrected against spinel.h).
# Ids come from src/rcp/props.h and OpenThread's src/lib/spinel/spinel.h,
# NOT from docs/protocol.md §10: the PHY block is 0x20.. (not 0x00..) and the address
# properties are 0x34.. (not 0x30..), which the implementation corrected. The
# first draft of this file used the spec's numbers and promptly got PROP_NOT_FOUND for
# its own arming - the firmware was right and this table was wrong.
# The ids this firmware answers: exactly the property table of src/rcp/props.c, plus the two
# stream properties that live outside it.
PROPS = {
    0x00: "LAST_STATUS",
    0x01: "PROTOCOL_VERSION",
    0x02: "NCP_VERSION",
    0x03: "INTERFACE_TYPE",
    0x05: "CAPS",
    0x08: "HWADDR",
    0x20: "PHY_ENABLED",
    0x21: "PHY_CHAN",
    0x24: "PHY_CCA_THRESHOLD",
    0x25: "PHY_TX_POWER",
    0x26: "PHY_RSSI",
    0x27: "PHY_RX_SENSITIVITY",
    0x34: "MAC_15_4_LADDR",
    0x35: "MAC_15_4_SADDR",
    0x36: "MAC_15_4_PANID",
    0x37: "MAC_RAW_STREAM_ENABLED",
    0x38: "MAC_PROMISCUOUS_MODE",
    0x3B: "MAC_RX_ON_WHEN_IDLE",
    0x70: "STREAM_DEBUG",
    0x71: "STREAM_RAW",
    0xB0: "RCP_API_VERSION",
    0xB1: "RCP_MIN_HOST_API_VERSION",
    0xB2: "RCP_LOG_CRASH_DUMP",
    0x120B: "RADIO_CAPS",
}

# Ids spinel.h defines that this firmware does not implement. Naming them keeps a
# PROP_NOT_FOUND verdict readable instead of looking like a dead link.
UNIMPLEMENTED = {
    0x04: "VENDOR_ID",
    0x22: "PHY_CHAN_SUPPORTED",
    0x23: "PHY_FREQ",
    0x30: "MAC_SCAN_STATE",
    0x31: "MAC_SCAN_MASK",
    0x32: "MAC_SCAN_PERIOD",
    0x33: "MAC_SCAN_BEACON",
    0x39: "MAC_ENERGY_SCAN_RESULT",
}

# Fixed value widths of the properties above, so a SET can send the right size. 0 means the
# value is variable-length (a string, a packed-uint list) and is sized by the caller.
WIDTHS = {0x00: 1, 0x01: 0, 0x02: 0, 0x03: 1, 0x05: 0, 0x08: 8, 0x20: 1, 0x21: 1,
          0x24: 1, 0x25: 1, 0x26: 1, 0x27: 1, 0x34: 8, 0x35: 2, 0x36: 2, 0x37: 1,
          0x38: 1, 0x3B: 1, 0x70: 0, 0x71: 0, 0xB0: 1, 0xB1: 1, 0xB2: 1,
          0x120B: 0}


def crc16(data):
    """Reflected CRC-16/X.25 (poly 0x8408, init 0xFFFF, final xor 0xFFFF). The spec's
    prose said 'no reflection'; the captures and the sibling decoder say otherwise
    (docs/protocol.md §3)."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x8408 if crc & 1 else 0)
    return crc ^ 0xFFFF


def escape(payload):
    out = bytearray()
    for byte in payload:
        if byte in (FLAG, ESC):
            out += bytes([ESC, byte ^ ESC_XOR])
        else:
            out.append(byte)
    return bytes(out)


def frame(payload):
    crc = crc16(payload)
    return bytes([FLAG]) + escape(bytes(payload) + bytes([crc & 0xFF, crc >> 8])) + bytes([FLAG])


def pack_uint(value):
    out = bytearray()
    while value >= 0x80:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    out.append(value)
    return bytes(out)


def unpack_uint(buf, pos):
    value, shift = 0, 0
    while True:
        byte = buf[pos]
        pos += 1
        value |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return value, pos
        shift += 7


def decode_command(byte):
    if byte & TYPE_COMMAND:
        kind = "command"
    elif byte & TYPE_RESPONSE:
        kind = "response"
    else:
        return None, None
    return kind, byte & 0x0F


def classify(buf):
    """Decode one frame body. Returns a dict or None when it is not a Spinel frame."""
    if len(buf) < 3:
        return None
    kind, tid = decode_command(buf[0])
    if kind is None:
        return None
    cmd = buf[1] & 0x0F
    try:
        prop, pos = unpack_uint(buf, 2)
    except IndexError:
        return None
    value = buf[pos:]
    out = {"kind": kind, "tid": tid, "cmd": cmd, "prop": prop, "value": value}
    # Only property 0 carries a status. Reading the first byte of every VALUE_IS as one
    # printed "LAST_STATUS 68" for the NCP_VERSION string and "LAST_STATUS 4" for the
    # PROTOCOL_VERSION pair - the value, not an error.
    if cmd == CMD_PROP_VALUE_IS and prop == 0 and value:
        try:
            status, _ = unpack_uint(value, 0)
            out["last_status"] = status
        except IndexError:
            pass
    return out


def describe(entry):
    prop = entry["prop"]
    name = PROPS.get(prop) or UNIMPLEMENTED.get(prop) or f"prop_{prop:#04x}"
    cmd = {2: "GET", 3: "SET", 6: "VALUE_IS", 7: "INSERTED", 8: "REMOVED"}.get(entry["cmd"], entry["cmd"])
    text = f"[{entry['kind']} tid={entry['tid']} {cmd} {name}] {entry['value'].hex(' ')}"
    if "last_status" in entry:
        text += f"  -> LAST_STATUS {entry['last_status']}"
    return text


class Decoder:
    """Classic HDLC deframer. It reports frames and counts what it had to throw away,
    because a silently-dropped frame is how a sniffer loses a whole capture."""

    def __init__(self):
        self.buf = bytearray()
        self.escaped = False
        self.in_frame = False
        self.bad_fcs = 0
        self.truncated = 0

    def feed(self, data):
        frames = []
        for byte in data:
            if byte == FLAG:
                if self.escaped:                # frame ended on a dangling escape
                    self.escaped = False
                    self.buf.clear()
                    self.truncated += 1
                    self.in_frame = True
                    continue
                if self.in_frame and len(self.buf) == 0:
                    # Two flags in a row: our own encoder writes FLAG at both ends, so a
                    # frame boundary always produces one of these. Not an error.
                    self.in_frame = True
                    continue
                if self.in_frame and len(self.buf) >= 2:
                    crc = crc16(self.buf[:-2])
                    if crc == (self.buf[-2] | (self.buf[-1] << 8)):
                        frames.append(bytes(self.buf[:-2]))
                    else:
                        self.bad_fcs += 1
                        frames.append(bytes(self.buf[:-2]))    # keep it, note it
                elif self.in_frame:
                    self.truncated += 1
                self.buf.clear()
                self.in_frame = True
                continue
            if not self.in_frame:
                continue
            if self.escaped:
                self.buf.append(byte ^ ESC_XOR)
                self.escaped = False
            elif byte == ESC:
                self.escaped = True
            else:
                self.buf.append(byte)
        return frames


def configure_serial(fd, baud):
    """Raw mode plus the line rate, on a byte pipe that is a real UART.

    Echo, canonical mode, signal characters and flow control all off: this console reads
    and writes HDLC octets, and a line discipline that echoes them back reads as the peer
    answering itself. """
    speed = getattr(termios, f"B{baud}", None)
    if speed is None:
        sys.exit(f"spinel_console: baud {baud} is not supported by this system")

    attrs = termios.tcgetattr(fd)
    attrs[0] = 0                                          # iflag
    attrs[1] = 0                                          # oflag
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL  # cflag
    attrs[3] = 0                                          # lflag
    attrs[4] = speed
    attrs[5] = speed
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--channel", type=int, default=15)
    ap.add_argument("--promisc", choices=("off", "network", "node", "full"), default="full",
                    help="MAC_PROMISCUOUS_MODE: off = filter to our PAN+address, "
                         "node = every frame on our PAN, full = everything")
    # Addresses for the filtered modes. Safe to point at a live network's PAN: this
    # firmware sets CSMA_SEED_1.AACK_DIS_ACK in every mode (radio.h), so a listener with
    # a fabricated short address receives passively and answers nothing.
    ap.add_argument("--panid", type=lambda x: int(x, 0), default=None,
                    help="MAC_15_4_PANID to programme into the frame filter")
    ap.add_argument("--saddr", type=lambda x: int(x, 0), default=None,
                    help="MAC_15_4_SADDR to programme into the frame filter")
    ap.add_argument("--duration", type=float, default=15.0)
    ap.add_argument("--no-arm", action="store_true")
    ap.add_argument("--get", action="append", default=[], metavar="PROP",
                    help="GET a property (repeatable); the answer is decoded like any "
                         "other frame, e.g. --get 0x08 for HWADDR")
    ap.add_argument("--allow-tx", action="store_true",
                    help="send one minimal frame AFTER arming - this TRANSMITS")
    ap.add_argument("--tx-channel", type=int, default=26,
                    help="channel for --allow-tx; 26 is the quiet one, and 15 is a live "
                         "network you should not transmit into while anyone is watching")
    ap.add_argument("--tx-frame", default="01 00 42",
                    help="PSDU hex for --allow-tx: FCF 0x0001 (data, no ACK request) + "
                         "seq, i.e. deliberately nobody's frame")
    ap.add_argument("--hex", action="store_true")
    ap.add_argument("--baud", type=int, default=None,
                    help="set the line rate on a UART link (115200 for the RaspBee II and ConBee III, "
                         "57600 for the ConBee I / RaspBee I)")
    args = ap.parse_args(argv)

    fd = open(args.port, "r+b", buffering=0)
    if args.baud:
        configure_serial(fd, args.baud)
    os.set_blocking(fd.fileno(), False)

    def send(payload):
        fd.write(frame(payload))

    def request(tid, cmd, prop, value=b""):
        return bytes([TYPE_COMMAND | tid, cmd]) + pack_uint(prop) + value

    tid = 1

    if not args.no_arm:
        steps = [
            ("PHY_ENABLED=true", request(tid, CMD_PROP_VALUE_SET, 0x20, b"\x01")),
        ]
        tid += 1
        steps.append((f"PHY_CHAN={args.channel}",
                      request(tid, CMD_PROP_VALUE_SET, 0x21, bytes([args.channel]))))
        tid += 1
        if args.panid is not None:
            steps.append((f"MAC_15_4_PANID={args.panid:#06x}",
                          request(tid, CMD_PROP_VALUE_SET, 0x36,
                                  bytes([args.panid & 0xFF, (args.panid >> 8) & 0xFF]))))
            tid += 1
        if args.saddr is not None:
            steps.append((f"MAC_15_4_SADDR={args.saddr:#06x}",
                          request(tid, CMD_PROP_VALUE_SET, 0x35,
                                  bytes([args.saddr & 0xFF, (args.saddr >> 8) & 0xFF]))))
            tid += 1
        steps.append((f"MAC_PROMISCUOUS_MODE={args.promisc}",
                      request(tid, CMD_PROP_VALUE_SET, 0x38,
                              bytes([{"off": 0, "network": 1, "node": 1,
                                      "full": 2}[args.promisc]]))))
        tid += 1
        steps.append(("MAC_RAW_STREAM_ENABLED=true",
                      request(tid, CMD_PROP_VALUE_SET, 0x37, b"\x01")))
        tid += 1
        for label, payload in steps:
            send(payload)
            print(f"  -> {label}")
            time.sleep(0.15)
        send(request(tid, CMD_PROP_VALUE_GET, 0x38))
        tid += 1
        time.sleep(0.2)

    for prop in (int(p, 0) for p in args.get):
        # A GET carries no value, only the packed property id; the answer arrives in the
        # listener below and is decoded with everything else.
        print(f"  -> GET {PROPS.get(prop, f'prop_{prop:#04x}')}")
        send(request(tid, CMD_PROP_VALUE_GET, prop, b""))
        tid += 1
        time.sleep(0.15)

    if args.allow_tx:
        # docs/protocol.md §7.1: length, PSDU, then the 16-byte trailer (channel, csma backoffs, retries,
        # enableCSMACA, isHeaderUpdated, isRetransmission, isSecurityProcessed, txDelay
        # 4B, txDelayBase 4B, rxChannelAfterTx). CSMA off and no ACK is requested, so
        # the frame leaves immediately and completes with LAST_STATUS OK rather than
        # waiting on an acknowledgement nobody is there to send.
        psdu = bytes.fromhex(args.tx_frame)
        trailer = bytes([args.tx_channel, 0, 0, 0, 0, 0, 0,
                         0, 0, 0, 0,      # txDelay
                         0, 0, 0, 0,      # txDelayBase
                         0])              # rxChannelAfterTx: keep
        payload = len(psdu).to_bytes(2, "little") + psdu + trailer
        print(f"  -> TX {len(psdu)} B on channel {args.tx_channel} (no ACK request, CSMA "
              f"off): {psdu.hex(' ')}")
        send(request(tid, CMD_PROP_VALUE_SET, 0x71, payload))
        tid += 1
        time.sleep(0.5)

    decoder = Decoder()
    # A replayed record is byte-identical to a live one, so a snapshot from before the
    # mode change reads exactly like a fresh measurement ("the receiver is in TRX_OFF",
    # "the promiscuous bit is clear"). The queue says when a replay starts:
    # DBG_COMP_DBGQ/DBG_DBGQ_RSN_REPLAY carries the number of shadow frames that follow,
    # so they can be labelled instead of believed.
    replay_left = 0
    deadline = time.time() + args.duration
    counts = {}
    raw_frames = 0
    debug_lines = []
    while time.time() < deadline:
        try:
            data = fd.read(4096) or b""
        except (BlockingIOError, OSError):
            data = b""
        if not data:
            time.sleep(0.005)
            continue
        for body in decoder.feed(data):
            entry = classify(body)
            if entry is None:
                counts["non-spinel"] = counts.get("non-spinel", 0) + 1
                if args.hex:
                    print(f"  ? {body.hex(' ')}")
                continue
            prop = entry["prop"]
            if prop == 0x70 and entry["value"]:
                pass
            if prop == 0x70 and entry["value"]:
                # STREAM_DEBUG: 16 hex digits + newline, one boot record per line.
                text = entry["value"].split(b"\n")[0]
                try:
                    record = bytes.fromhex(text.decode("ascii", "ignore"))
                except ValueError:
                    record = b""
                # A record is 8 bytes: magic, component, line, reason (dbg.h), and the
                # magic is 0x0000 - not skipping it shifts every field by one slot and
                # prints values that look plausible and mean nothing, which is exactly how
                # this decoder misled a whole debugging session. tools/dbg.py is the
                # authoritative decoder for captures; this is the streaming one, and it
                # now agrees with it.
                if len(record) == 8 and (record[0] | (record[1] << 8)) == 0x0000:
                    comp = record[2] | (record[3] << 8)
                    line = record[4] | (record[5] << 8)
                    reason = record[6] | (record[7] << 8)
                    text = (f"  [{time.strftime('%H:%M:%S')}] RDBG comp={comp} line={line}"
                            f" reason={reason} raw={record.hex()}")
                    print(text)
                    debug_lines.append(text)
                continue
            if prop == 0x71:
                raw_frames += 1
                value = entry["value"]
                if len(value) >= 2:
                    flen = value[0] | (value[1] << 8)
                    psdu = value[2:2 + flen]
                    meta = value[2 + flen:]
                    # `meta` begins at MD_POWER, i.e. at the spec's offset n+2 - so the
                    # docs/protocol.md §7.2 offsets n+8/n+9 for channel/LQI are meta[6]/meta[7] here.
                    rssi = (meta[1] - 256 if meta[1] > 127 else meta[1]) if len(meta) > 1 else None
                    # ... MD_POWER is meta[0], MD_NOISE meta[1], MD_FLAG meta[2:4],
                    # MD_PHY length meta[4:6], channel meta[6], LQI meta[7].
                    rssi = (meta[0] - 256 if meta[0] > 127 else meta[0]) if meta else None
                    chan = meta[6] if len(meta) > 6 else None
                    lqi = meta[7] if len(meta) > 7 else None
                    flags = (meta[2] | (meta[3] << 8)) if len(meta) > 3 else None
                    print(f"  RAW len={flen} chan={chan} rssi={rssi} lqi={lqi} "
                          f"flags={flags:#06x} {psdu.hex(' ')}")
                continue
            counts[PROPS.get(prop, f"prop_{prop:#04x}")] = counts.get(
                PROPS.get(prop, f"prop_{prop:#04x}"), 0) + 1
            print(describe(entry))

    print(f"== {raw_frames} STREAM_RAW frame(s), {len(debug_lines)} STREAM_DEBUG line(s), "
          f"bad_fcs={decoder.bad_fcs} truncated={decoder.truncated}, other={counts}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
