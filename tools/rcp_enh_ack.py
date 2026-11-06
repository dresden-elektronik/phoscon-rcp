#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""rcp_enh_ack.py — Enhanced Acks and timed frames on the air, against a second RCP.

Two sticks, raw Spinel on each. A runs this firmware; B is a reference RCP, such as the official
ConBee III Thread firmware. Every frame and Ack is checked byte for byte with an independent AES-CCM, so
neither side's crypto marks its own work:

  - B sends secured version 2 frames, and A's Enhanced Ack is what B's completion hands back.
  - A sends secured version 2 frames, encrypted by A, and B's Enhanced Ack comes back through A.
  - A sends timed frames, and B's receive stamps measure them.

  python3 tools/rcp_enh_ack.py --a /dev/ttyUSB0@115200 --b /dev/ttyUSB1@115200 --channel 26

Needs pyserial and cryptography, and transmits: use a quiet channel and a private PAN.
"""
import argparse
import struct
import sys
import time

try:
    import serial
    from cryptography.exceptions import InvalidTag
    from cryptography.hazmat.primitives.ciphers.aead import AESCCM
except ImportError as e:
    sys.exit(f"missing: {e.name}")

PROP_LAST_STATUS, PROP_PHY_ENABLED, PROP_PHY_CHAN = 0x00, 0x20, 0x21
PROP_LADDR, PROP_SADDR, PROP_PANID, PROP_RAW, PROP_PROMISCUOUS = 0x34, 0x35, 0x36, 0x37, 0x38
PROP_STREAM_RAW = 0x71
PROP_SRC_ENABLED, PROP_SRC_SHORT = 0x1303, 0x1304
PROP_MAC_KEY, PROP_MAC_COUNTER, PROP_PROBING, PROP_TIMESTAMP = 0x800, 0x801, 0x803, 0x802
CMD_SET, CMD_GET, CMD_INSERT, CMD_IS = 3, 2, 4, 6
MD_ACKED_FP, MD_ACKED_SEC = 0x10, 0x20

KEYS = (b"\x11" * 16, bytes(range(16)), b"\x22" * 16)
KEY_INDEX = 5
PAN = 0xBEEF
SCF = 0x0D  # level 5, key ID mode 1


def packed(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def unpacked(b, off=0):
    v = s = 0
    while True:
        x = b[off]
        off += 1
        v |= (x & 0x7F) << s
        s += 7
        if not x & 0x80:
            return v, off


def crc(data):
    c = 0xFFFF
    for x in data:
        c ^= x
        for _ in range(8):
            c = (c >> 1) ^ 0x8408 if c & 1 else c >> 1
    return ~c & 0xFFFF


def hdlc(payload):
    out = bytearray([0x7E])
    for x in payload + crc(payload).to_bytes(2, "little"):
        out += bytes([0x7D, x ^ 0x20]) if x in (0x7E, 0x7D, 0x11, 0x13, 0xF8) else bytes([x])
    return bytes(out + b"\x7e")


class Stick:
    def __init__(self, spec, ext, short):
        path, _, baud = spec.partition("@")
        self.s = serial.Serial(path, int(baud or 115200), timeout=0.002)
        self.buf = b""
        self.ext, self.short = ext, short
        self.rx = []  # unsolicited STREAM_RAW values
        self.s.write(b"\x7e")
        time.sleep(0.1)
        self.s.reset_input_buffer()

    def frames(self):
        self.buf += self.s.read(max(1, self.s.in_waiting))
        parts = self.buf.split(b"\x7e")
        self.buf = parts[-1]
        for raw in parts[:-1]:
            u, esc = bytearray(), False
            for x in raw:
                if x == 0x7D:
                    esc = True
                    continue
                u.append(x ^ 0x20 if esc else x)
                esc = False
            if len(u) >= 4 and crc(bytes(u[:-2])) == int.from_bytes(u[-2:], "little"):
                yield bytes(u[:-2])

    def pump(self, secs):
        end = time.time() + secs
        while time.time() < end:
            for f in self.frames():
                self.note(f)

    def note(self, f):
        prop, off = unpacked(f, 2)
        if f[0] == 0x80 and prop == PROP_STREAM_RAW:
            self.rx.append(f[off:])

    def rpc(self, tid, cmd, prop, value=b"", wait=1.5):
        self.s.write(hdlc(bytes([0x80 | tid, cmd]) + packed(prop) + value))
        end = time.time() + wait
        while time.time() < end:
            for f in self.frames():
                self.note(f)
                if f[0] == (0x80 | tid):
                    p, off = unpacked(f, 2)
                    return p, f[off:]
        return None, None

    def set(self, prop, value):
        p, v = self.rpc(1, CMD_SET, prop, value)
        if p == PROP_LAST_STATUS and v and v[0] != 0:
            raise RuntimeError(f"SET {prop:#x}: status {v[0]}")
        if p is None:
            raise RuntimeError(f"SET {prop:#x}: no answer")

    def setup(self, channel):
        self.set(PROP_PHY_ENABLED, b"\x01")
        self.set(PROP_PHY_CHAN, bytes([channel]))
        self.set(PROP_PANID, struct.pack("<H", PAN))
        self.set(PROP_SADDR, struct.pack("<H", self.short))
        self.set(PROP_LADDR, self.ext)
        self.set(PROP_MAC_KEY, bytes([1 << 3, KEY_INDEX]) + b"".join(b"\x10\x00" + k for k in KEYS))
        self.set(PROP_MAC_COUNTER, struct.pack("<I", 0x1000) + b"\x00")
        self.set(PROP_SRC_ENABLED, b"\x00")
        self.set(PROP_RAW, b"\x01")
        self.set(PROP_PROMISCUOUS, b"\x00")  # a sniffer before us left it on, and a promiscuous node does not Ack

    def now(self):
        p, v = self.rpc(2, CMD_GET, PROP_TIMESTAMP, bytes(8))
        return struct.unpack("<Q", v[:8])[0]

    def tx(self, psdu, channel, updated=0, processed=0, delay=0, base=0, cca=1, retries=0):
        trailer = bytes([channel, 4, retries, cca, updated, 0, processed]) + struct.pack("<II", delay, base) + b"\x00\x7f"
        p, v = self.rpc(3, CMD_SET, PROP_STREAM_RAW, struct.pack("<H", len(psdu)) + psdu + trailer, wait=2.5)
        if p != PROP_LAST_STATUS:
            return None
        status, off = unpacked(v)
        out = {"status": status, "fp": v[off], "updated": v[off + 1], "ack": b""}
        off += 2
        if status == 0 and off + 2 <= len(v):
            n = struct.unpack_from("<H", v, off)[0]
            out["ack"] = v[off + 2 : off + 2 + n]
        return out


def rx_parse(v):
    n = struct.unpack_from("<H", v, 0)[0]
    psdu = v[2 : 2 + n]
    off = 2 + n
    rssi, _, flags = struct.unpack_from("<bbH", v, off)
    off += 4
    plen = struct.unpack_from("<H", v, off)[0]
    chan, lqi, stamp = struct.unpack_from("<BBQ", v, off + 2)
    off += 2 + plen
    vlen = struct.unpack_from("<H", v, off)[0]
    off += 2 + vlen
    mlen = struct.unpack_from("<H", v, off)[0]
    key, counter = struct.unpack_from("<BI", v, off + 2) if mlen >= 5 else (0, 0)
    return {"psdu": psdu, "flags": flags, "stamp": stamp, "key": key, "counter": counter, "rssi": rssi}


def key_for(index):
    return {KEY_INDEX - 1: KEYS[0], KEY_INDEX: KEYS[1], KEY_INDEX + 1: KEYS[2]}[index]


def nonce(ext, counter):
    return ext + struct.pack(">I", counter) + b"\x05"


def build_v2(seq, sender, dest_short, payload, counter, index=KEY_INDEX, ftype=1, ext_src=False):
    """A secured version 2 frame, encrypted here: what a host that does its own security would send."""
    fcf = ftype | 0x08 | 0x20 | 0x40 | 0x2000 | 0x0800 | (0xC000 if ext_src else 0x8000)
    hdr = struct.pack("<HBHH", fcf, seq, PAN, dest_short)
    hdr += bytes(reversed(sender.ext)) if ext_src else struct.pack("<H", sender.short)
    aux = bytes([SCF]) + struct.pack("<I", counter) + bytes([index])
    a = hdr + aux
    return a + AESCCM(key_for(index), tag_length=4).encrypt(nonce(sender.ext, counter), payload, a) + b"\0\0"


def check_ack(ack, seq, acker, dest_short=None, dest_ext=None, want_fp=None):
    """Parse a secured Enhanced Ack, verify its MIC with AES-CCM. Returns (ok, why, counter, index)."""
    if len(ack) < 3:
        return False, f"too short: {ack.hex()}", 0, 0
    fcf = struct.unpack_from("<H", ack, 0)[0]
    if fcf & 7 != 2 or (fcf >> 12) & 3 != 2:
        return False, f"not an Enhanced Ack: fcf {fcf:#06x}", 0, 0
    if ack[2] != seq:
        return False, f"sequence {ack[2]} != {seq}", 0, 0
    off = 3
    if not fcf & 0x40:
        pan = struct.unpack_from("<H", ack, off)[0]
        off += 2
        if pan != PAN:
            return False, f"PAN {pan:#x}", 0, 0
    if (fcf >> 10) & 3 == 2:
        got = struct.unpack_from("<H", ack, off)[0]
        off += 2
        if dest_short is not None and got != dest_short:
            return False, f"destination {got:#x}", 0, 0
    else:
        got = ack[off : off + 8]
        off += 8
        if dest_ext is not None and got != bytes(reversed(dest_ext)):
            return False, f"destination {got.hex()}", 0, 0
    if want_fp is not None and bool(fcf & 0x10) != bool(want_fp):
        return False, f"Frame Pending {bool(fcf & 0x10)}, wanted {bool(want_fp)}", 0, 0
    if not fcf & 0x08:
        return True, "unsecured", 0, 0
    scf, counter, index = struct.unpack_from("<BIB", ack, off)
    off += 6
    if scf != SCF:
        return False, f"security control {scf:#x}", 0, 0
    # A Header IE may stand between the auxiliary header and the MIC, and is authenticated with it.
    a, tag = ack[:-6], ack[-6:-2]
    try:
        AESCCM(key_for(index), tag_length=4).decrypt(nonce(acker.ext, counter), tag, a)
    except InvalidTag:
        return False, f"MIC does not verify: {ack.hex()}", counter, index
    return True, "", counter, index


FAILS = []


def verdict(name, ok, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}  {name:58s} {detail}")
    if not ok:
        FAILS.append(name)


def a_answers(a, b, ch):
    print("A answers B's secured version 2 frames")
    # (name, frame type, source table on and holding B, key index, extended source, wanted Frame Pending)
    cases = (
        ("data, table off: nothing to learn", 1, None, KEY_INDEX, False, 0),
        ("poll, table off: claims pending", 3, None, KEY_INDEX, False, 1),
        ("poll, table on and empty", 3, False, KEY_INDEX, False, 0),
        ("poll, table on and holding B", 3, True, KEY_INDEX, False, 1),
        ("data, table on and holding B", 1, True, KEY_INDEX, False, 1),
        ("extended source: Ack addressed to it", 1, None, KEY_INDEX, True, 0),
        ("under the previous key", 1, None, KEY_INDEX - 1, False, 0),
        ("under the next key", 1, None, KEY_INDEX + 1, False, 0),
    )
    seq = 0x40
    counter = 0x2000
    last = {}
    for name, ftype, table, index, ext_src, want in cases:
        seq += 1
        counter += 1
        a.set(PROP_SRC_ENABLED, b"\x00" if table is None else b"\x01")
        a.set(PROP_SRC_SHORT, struct.pack("<H", b.short) if table else b"")
        payload = bytes([0x04]) if ftype == 3 else b"\xde\xad\xbe\xef"
        psdu = build_v2(seq, b, a.short, payload, counter, index, ftype, ext_src)
        a.rx.clear()
        done = b.tx(psdu, ch, updated=1, processed=1, retries=3)
        a.pump(0.05)
        if done is None or done["status"] != 0:
            verdict(name, False, f"B's completion {done}")
            continue
        ok, why, got_counter, got_index = check_ack(done["ack"], seq, a, None if ext_src else b.short, b.ext if ext_src else None, want)
        if ok and got_index != index:
            ok, why = False, f"key index {got_index}, wanted {index}"
        heard = [rx_parse(v) for v in a.rx]
        heard = [h for h in heard if len(h["psdu"]) > 2 and h["psdu"][2] == seq]
        if ok and not heard:
            ok, why = False, "A's host never heard the frame"
        elif ok:
            h = heard[0]
            if not h["flags"] & MD_ACKED_SEC:
                ok, why = False, f"MD_FLAG {h['flags']:#x} lacks ACKED_SEC"
            elif bool(h["flags"] & MD_ACKED_FP) != bool(want):
                ok, why = False, f"MD_FLAG {h['flags']:#x} disagrees on Frame Pending"
            elif h["counter"] != got_counter or h["key"] != got_index:
                ok, why = False, f"metadata key {h['key']} counter {h['counter']}, Ack {got_index} {got_counter}"
        if ok and index == KEY_INDEX:
            if last.get(index) is not None and got_counter <= last[index]:
                ok, why = False, f"counter {got_counter} did not advance from {last[index]}"
            last[index] = got_counter
        if ok and index == KEY_INDEX + 1 and got_counter != 0:
            ok, why = False, f"next key's counter {got_counter}"
        verdict(name, ok, why or f"counter {got_counter}")
    a.set(PROP_SRC_ENABLED, b"\x00")

    # Enhanced-Ack based Link Metrics probing: B asks A for LQI and RSSI in its Acks.
    a.set(PROP_PROBING, struct.pack("<H", b.short) + b.ext + b"\x0a")
    seq += 1
    counter += 1
    done = b.tx(build_v2(seq, b, a.short, b"\x01\x02\x03\x04", counter), ch, updated=1, processed=1, retries=3)
    ack = done["ack"] if done else b""
    ok, why, got_counter, _ = check_ack(ack, seq, a, b.short) if done and done["status"] == 0 else (False, f"{done}", 0, 0)
    fcf = struct.unpack_from("<H", ack, 0)[0] if len(ack) >= 2 else 0
    if ok and not fcf & 0x0200:
        ok, why = False, f"no IE in fcf {fcf:#06x}"
    if ok:
        # FCF, sequence, PAN, short destination, auxiliary header: then the IE.
        ie = ack[13:21]
        if ie[0:6] != bytes([6, 0, 0x9B, 0xB8, 0xEA, 0]):
            ok, why = False, f"IE {ie.hex()}"
        else:
            why = f"LQI {ie[6]}, RSSI byte {ie[7]}"
    verdict("probing: LQI and RSSI inside the secured Ack", ok, why)
    a.set(PROP_PROBING, struct.pack("<H", b.short) + b.ext + b"\x00")
    seq += 1
    counter += 1
    done = b.tx(build_v2(seq, b, a.short, b"\x01\x02\x03\x04", counter), ch, updated=1, processed=1, retries=3)
    ack = done["ack"] if done else b""
    fcf = struct.unpack_from("<H", ack, 0)[0] if len(ack) >= 2 else 0
    verdict("probing ended: no IE", bool(done and done["status"] == 0 and not fcf & 0x0200), f"fcf {fcf:#06x}")


def b_answers(a, b, ch, frames):
    print("A sends secured version 2 frames, encrypted by A; B answers")
    good = bad = 0
    why = ""
    for i in range(frames):
        seq = 0x80 + i
        # Key index and counter left for A to fill: the shape a host hands over.
        fcf = 1 | 0x08 | 0x20 | 0x40 | 0x2000 | 0x0800 | 0x8000
        aux = bytes([SCF]) + bytes(4) + b"\x00"
        psdu = struct.pack("<HBHHH", fcf, seq, PAN, b.short, a.short) + aux + bytes([i & 0xFF]) * 8 + bytes(4) + b"\0\0"
        b.rx.clear()
        done = a.tx(psdu, ch, retries=3)
        b.pump(0.03)
        if done is None or done["status"] != 0:
            bad += 1
            why = f"A's completion {done}"
            continue
        ok, w, counter, index = check_ack(done["ack"], seq, b, a.short)
        heard = [rx_parse(v) for v in b.rx]
        heard = [h for h in heard if len(h["psdu"]) > 2 and h["psdu"][2] == seq]
        if ok and heard:
            p = heard[0]["psdu"]
            ctr = struct.unpack_from("<I", p, 10)[0]
            try:
                clear = AESCCM(key_for(p[14]), tag_length=4).decrypt(nonce(a.ext, ctr), p[15:-2], p[:15])
                if clear != bytes([i & 0xFF]) * 8:
                    ok, w = False, f"decrypted to {clear.hex()}"
            except InvalidTag:
                ok, w = False, "A's frame does not verify at B"
        elif ok:
            ok, w = False, "B's host never heard it"
        good += ok
        bad += not ok
        why = w or why
    verdict(f"{frames} frames: Ack verifies, and A's frame decrypts at B", bad == 0, f"{good}/{frames}" + (f"; {why}" if bad else ""))


def timed(a, b, ch, count, period):
    print(f"A's timed frames, stamped by B: {count}, one every {period} us")
    psdu = struct.pack("<HBHHH", 0x8841, 0, PAN, 0xFFFF, a.short) + bytes(4) + b"\0\0"
    b.rx.clear()
    base = a.now() & 0xFFFFFFFF
    first = 60000
    misses = 0
    for k in range(count):
        p = bytearray(psdu)
        p[2] = k & 0xFF
        done = a.tx(bytes(p), ch, delay=first + k * period, base=base, cca=0)
        if done is None or done["status"] != 0:
            misses += 1
    b.pump(0.1)
    stamps = {}
    for v in b.rx:
        h = rx_parse(v)
        if len(h["psdu"]) > 2 and h["psdu"][0:2] == psdu[0:2]:
            stamps[h["psdu"][2]] = h["stamp"]
    pairs = [(stamps[k + 1] - stamps[k]) for k in sorted(stamps) if k + 1 in stamps]
    if not pairs:
        verdict("B heard the timed frames", False, f"{len(stamps)} heard, {misses} missed")
        return
    errs = [d - period for d in pairs]
    mean = sum(errs) / len(errs)
    print(f"        {len(stamps)}/{count} heard, {misses} missed; interval error min {min(errs)} max {max(errs)} mean {mean:.2f} us ({mean / period * 1e6:.1f} ppm)")
    verdict("interval error within +-10 us of its mean", all(abs(e - mean) <= 10 for e in errs), f"spread {max(errs) - min(errs)} us")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--a", required=True, help="this firmware's port, path@baud")
    ap.add_argument("--b", required=True, help="the reference RCP's port, path@baud")
    ap.add_argument("--channel", type=int, default=26)
    ap.add_argument("--frames", type=int, default=30)
    ap.add_argument("--period", type=int, default=30000, help="microseconds between timed frames")
    args = ap.parse_args()
    a = Stick(args.a, bytes.fromhex("AA00000000000A0A"), 0x0A0A)
    b = Stick(args.b, bytes.fromhex("BB00000000000B0B"), 0x0B0B)
    a.setup(args.channel)
    b.setup(args.channel)
    a_answers(a, b, args.channel)
    b_answers(a, b, args.channel, args.frames)
    timed(a, b, args.channel, args.frames, args.period)
    print(f"== {len(FAILS)} failure(s)")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
