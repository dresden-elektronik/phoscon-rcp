#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""cb3_probe.py - read-only audit of a ConBee III running the vendor firmware.

The ConBee III in the lab is not running this tree's firmware: it runs deCONZ's Zigbee
coordinator, which speaks deCONZ's own SLIP protocol on the link (docs/hardware_mg21.md §10.9). Before a
cb3 image is written anywhere, the useful measurements are what that board actually answers, at
what rate, with what framing - and after a cb3 image exists, the same probe is how the board is
recognised as having been taken over.

This tool sends read commands only. The frame builder knows four command bytes: version,
features, device state, and read parameter. There is no code path here that writes a parameter,
changes network state, resets the network, uploads an image, or touches the bootloader, and the
port is opened with DTR and RTS released so opening the link cannot be what decides whether a
board is in its bootloader.

    tools/cb3_probe.py --port <cb3-port> --listen 2
    tools/cb3_probe.py --port <cb3-port>
    tools/cb3_probe.py --port <port> --params 0x01 0x05 0x1C --quiet

Framing, as the firmware's own `deconzProtocol.c` lays it out: SLIP with 0xC0 delimiters and
0xDB/0xDC/0xDD escaping; inside, a five-byte header (command, sequence, status, u16 frame length
little-endian) where the frame length counts the header plus the two-byte payload length plus the
payload and excludes the checksum; then u16 payload length, the payload, and an additive 16-bit
checksum stored little-endian as its own two's complement.
"""

import argparse
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("cb3_probe: needs pyserial (use .venv/bin/python)")

SLIP_END = 0xC0
SLIP_ESC = 0xDB
SLIP_ESC_END = 0xDC
SLIP_ESC_ESC = 0xDD

CMD_DEVICE_STATE = 0x07
CMD_READ_PARAMETER = 0x0A
CMD_VERSION = 0x0D
CMD_FEATURES = 0x11

READ_ONLY_COMMANDS = {
    CMD_VERSION: "version",
    CMD_FEATURES: "features",
    CMD_DEVICE_STATE: "device state",
    CMD_READ_PARAMETER: "read parameter",
}

# Parameter ids this tool will read. The network key ids (0x18, 0x31) are deliberately absent:
# a link audit has no need to print a network's key, and a tool that can is a tool someone will
# eventually run in front of others.
PARAMETERS = {
    0x01: "MAC address",
    0x05: "PAN id",
    0x09: "designated coordinator",
    0x0A: "channel mask",
    0x1C: "current channel",
    0x21: "permit join",
    0x24: "network update id",
    0x26: "watchdog TTL",
    0x29: "log level",
}

# The bootloader's dialog is a different shape from the application's: no five-byte header,
# just the magic 0x81, the command, the payload, and the same additive 16-bit checksum - as the
# bootloader's own protocol.c computes it, and as GCFFlasher 4.12.2 sends it (BTL_MAGIC then the
# command byte). Reading the bootloader's ID is the only question here that is worth asking,
# because its answer says whether the board is sitting in DFU.
BTL_MAGIC = 0x81
BTL_ID_REQUEST = 0x02
BTL_ID_RESPONSE = 0x82


def build_bootloader(command: int) -> bytes:
    body = bytes([BTL_MAGIC, command])
    return slip_encode(body + checksum(body))


def describe_bootloader(frame: bytes) -> str:
    if len(frame) < 4 or frame[0] != BTL_MAGIC:
        return "not a bootloader frame: %s" % frame.hex()
    cmd, body = frame[1], frame[2:-2]
    given, want = frame[-2:], checksum(frame[:-2])
    mark = "checksum ok" if given == want else "checksum %s want %s" % (given.hex(), want.hex())
    if cmd == BTL_ID_RESPONSE and len(body) >= 8:
        major = int.from_bytes(body[0:2], "little")
        minor = int.from_bytes(body[2:4], "little")
        crc = int.from_bytes(body[4:8], "little")
        return ("bootloader id: version %d.%d, app CRC-32 0x%08X [%s]" % (major, minor, crc, mark))
    return "bootloader command 0x%02X, payload %s [%s]" % (cmd, body.hex(), mark)


# Commands that exist on this link and that this tool refuses to send, named so that a refusal
# is legible rather than mysterious.
FORBIDDEN = {
    0x08: "change network state",
    0x0B: "write parameter",
    0x12: "APS data request 2",
    0x03: "APS data request",
    0x21: "update bootloader",
    0x23: "factory reset",
    0x26: "log message",
}


def checksum(body: bytes) -> bytes:
    total = 0
    for b in body:
        total = (total + b) & 0xFFFF
    return bytes([(-total) & 0xFF, ((-total) >> 8) & 0xFF])


def slip_encode(payload: bytes) -> bytes:
    out = bytearray([SLIP_END])
    for b in payload:
        if b == SLIP_END:
            out += bytes([SLIP_ESC, SLIP_ESC_END])
        elif b == SLIP_ESC:
            out += bytes([SLIP_ESC, SLIP_ESC_ESC])
        else:
            out.append(b)
    out.append(SLIP_END)
    return bytes(out)


def slip_split(buf: bytes):
    frames, cur, escaped = [], bytearray(), False
    trailing = b""
    for b in buf:
        if escaped:
            cur.append(SLIP_ESC_END if b == SLIP_ESC_END else (SLIP_ESC_ESC if b == SLIP_ESC else b))
            escaped = False
        elif b == SLIP_ESC:
            escaped = True
        elif b == SLIP_END:
            if cur:
                frames.append(bytes(cur))
                cur = bytearray()
        else:
            cur.append(b)
    trailing = bytes(cur)
    return frames, trailing


def build(command: int, sequence: int, payload: bytes = b"") -> bytes:
    if command not in READ_ONLY_COMMANDS:
        raise ValueError("cb3_probe refuses command 0x%02X (%s)"
                         % (command, FORBIDDEN.get(command, "not a read command")))
    frame_len = 5 + 2 + len(payload)
    body = bytes([command, sequence, 0x00, frame_len & 0xFF, (frame_len >> 8) & 0xFF])
    body += bytes([len(payload) & 0xFF, (len(payload) >> 8) & 0xFF])
    body += payload
    return slip_encode(body + checksum(body))


def describe(frame: bytes) -> str:
    if len(frame) < 7:
        return "short frame: %s" % frame.hex()
    cmd, seq, status = frame[0], frame[1], frame[2]
    declared = int.from_bytes(frame[3:5], "little")
    payload_len = int.from_bytes(frame[5:7], "little")
    body = frame[7:-2]
    given, want = frame[-2:], checksum(frame[:-2])
    name = READ_ONLY_COMMANDS.get(cmd, "command 0x%02X" % cmd)
    mark = "checksum ok" if given == want else "checksum %s want %s" % (given.hex(), want.hex())
    extra = ""
    if cmd == CMD_VERSION and len(body) >= 4:
        extra = " platform 0x%02X version %02X.%02X" % (body[1], body[3], body[2])
    elif cmd == CMD_FEATURES:
        extra = " payload %s" % body[2:].hex()
    elif cmd == CMD_DEVICE_STATE and len(body) > 2:
        extra = " state 0x%02X" % body[2]
    elif cmd == CMD_READ_PARAMETER and len(body) > 1:
        # The payload is the parameter id, then its value (little-endian, as the firmware stores it);
        # the MAC is shown in display order too, most significant byte first.
        pid, value = body[0], body[1:]
        extra = " %s (0x%02X) = %s" % (PARAMETERS.get(pid, "parameter 0x%02X" % pid), pid, value.hex())
        if pid == 0x01 and len(value) == 8:
            extra += " (%s)" % ":".join("%02X" % b for b in value[::-1])
    return ("%s: seq %d status 0x%02X declared %d payload %d [%s]%s"
            % (name, seq, status, declared, payload_len, mark, extra))


class Link:
    """One opened port. Read and write at the frame level; nothing else."""

    def __init__(self, port: str, baud: int, dtr: bool = False, rts: bool = False):
        self.port = port
        self.baud = baud
        self.dtr = dtr
        self.rts = rts

    def __enter__(self):
        self.s = serial.Serial(self.port, self.baud, timeout=0.1, dsrdtr=False)
        self.s.dtr = self.dtr
        self.s.rts = self.rts
        self.s.reset_input_buffer()
        return self

    def __exit__(self, *exc):
        self.s.close()
        return False

    def lines(self) -> str:
        """The bridge's modem lines, read. A passive observation: nothing here drives a pin."""
        s = self.s
        return ("DTR=%d RTS=%d CTS=%d DSR=%d RI=%d CD=%d"
                % (bool(s.dtr), bool(s.rts), bool(s.cts), bool(s.dsr), bool(s.ri), bool(s.cd)))

    def read_frames(self, seconds: float, want_cmd=None, trailing=b""):
        out, buf = [], trailing
        t0 = time.time()
        while time.time() - t0 < seconds:
            chunk = self.s.read(256)
            if not chunk:
                continue
            frames, buf = slip_split(buf + chunk)
            for f in frames:
                if want_cmd is None or (len(f) > 0 and f[0] == want_cmd):
                    out.append(f)
        return out, buf

    def request(self, command: int, sequence: int, payload: bytes = b"", wait: float = 1.5):
        return self.request_raw(build(command, sequence, payload), wait)

    def request_raw(self, frame_bytes: bytes, wait: float = 1.5):
        self.s.write(frame_bytes)
        self.s.flush()
        return self.read_frames(wait)


def main() -> int:
    ap = argparse.ArgumentParser(description="read-only ConBee III link audit")
    ap.add_argument("--port", required=True, help="the port, given explicitly (no guessing)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--listen", type=float, default=0.0, help="listen this many seconds first, sending nothing")
    ap.add_argument("--params", nargs="*", default=[], help="parameter ids to read, hex (0x01 0x1C ...)")
    ap.add_argument("--dtr", action="store_true", help="leave DTR asserted while probing")
    ap.add_argument("--rts", action="store_true", help="leave RTS asserted while probing")
    ap.add_argument("--bootloader-id", action="store_true",
                    help="also ask the bootloader for its ID (0x81 0x02): a read, answered only in DFU")
    ap.add_argument("--verbose", action="store_true", help="show the frames this tool writes")
    ap.add_argument("--lines", action="store_true", help="report the bridge's modem lines")
    ap.add_argument("--allow-cdc", action="store_true",
                    help="also run against a CDC board (ConBee II or RaspBee II): the same protocol, "
                         "used to prove this tool's framing against a board that is known to answer")
    args = ap.parse_args()

    if "usbmodem" in args.port and not args.allow_cdc:
        print("cb3_probe: %s is a CDC board (ConBee II or RaspBee II); this probe is for the "
              "ConBee III's USART link; pass --allow-cdc to use it as a framing check" % args.port,
              file=sys.stderr)
        return 2

    ids = [int(x, 16) for x in args.params]
    for pid in ids:
        if pid not in PARAMETERS:
            print("cb3_probe: parameter 0x%02X is not on this tool's read list" % pid, file=sys.stderr)
            return 2

    seq = 1
    total = 0
    with Link(args.port, args.baud, args.dtr, args.rts) as link:
        if args.lines:
            print("modem lines after open: %s" % link.lines())
            time.sleep(0.2)
            print("modem lines settled:   %s" % link.lines())

        if args.listen > 0:
            print("listening, sending nothing, for %.1f s:" % args.listen)
            frames, _ = link.read_frames(args.listen)
            for f in frames:
                total += 1
                print("   unsolicited %s" % describe(f))
            if not frames:
                print("   nothing on the link")

        for cmd in (CMD_VERSION, CMD_FEATURES, CMD_DEVICE_STATE):
            frames, _ = link.request(cmd, seq)
            if args.verbose:
                print("   wrote %s" % build(cmd, seq).hex())
            print("%s (seq %d):" % (READ_ONLY_COMMANDS[cmd], seq))
            seq += 1
            if not frames:
                print("   no answer")
            for f in frames:
                total += 1
                print("   %s" % describe(f))

        if args.bootloader_id:
            frames, _ = link.request_raw(build_bootloader(BTL_ID_REQUEST), 1.5)
            print("bootloader id request (%s):" % build_bootloader(BTL_ID_REQUEST).hex())
            if not frames:
                print("   no answer")
            for f in frames:
                total += 1
                print("   %s" % describe_bootloader(f))

        for pid in ids:
            frames, _ = link.request(CMD_READ_PARAMETER, seq, bytes([pid]))
            print("read parameter 0x%02X (%s), seq %d:" % (pid, PARAMETERS[pid], seq))
            seq += 1
            if not frames:
                print("   no answer")
            for f in frames:
                total += 1
                print("   %s" % describe(f))

    print("%d frame(s) read from %s at %d" % (total, args.port, args.baud))
    return 0


if __name__ == "__main__":
    sys.exit(main())
