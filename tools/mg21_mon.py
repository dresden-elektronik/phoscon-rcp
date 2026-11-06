#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""mg21_mon.py - drive the ConBee III bench register monitor (docs/hardware_mg21.md §10.5).

The cb3 probe image takes register commands on its link (src/app/mon_proto.h) and answers with
DBG_COMP_MON records in its debug stream. This tool frames the commands (HDLC, the same FCS as
Spinel), reads the stream, skips everything that is not a reply to its own request, and prints the
values. It is also importable: `Monitor(port).read(addr, n)` and friends, for experiment scripts.

    tools/mg21_mon.py --port <cb3-port> read RAC.STATUS
    tools/mg21_mon.py --port <port> read SEQRAM+0x1F00 8
    tools/mg21_mon.py --port <port> write RAC.CMD 0x80
    tools/mg21_mon.py --port <port> trace RAC.CMD 0x80 RAC.STATUS 0x0F000000 --count 8 --cycles 384000

Addresses are numbers or names: BLOCK.REGISTER from test/data/mg21_radio_ref.h and mg21_regs_ref.h
(non-secure alias by default, BLOCK_S.REGISTER for the secure one), with an optional .SET/.CLR/.TGL
suffix, SEQRAM/FRCRAM (non-secure), and +offset on any of them. The board's region table decides what
may be read and written; a refused command answers DENIED and touches nothing.
"""
import argparse
import re
import sys
import time
from pathlib import Path

try:
    import serial
except ImportError:  # the register names work without it; only a port needs it (Monitor)
    serial = None

REPO = Path(__file__).resolve().parent.parent
DBG_H = REPO / "src" / "hal" / "dbg.h"
# The whole radio record where the maintainers have it, else the tracked subset.
RADIO_FULL = REPO / "internal" / "test" / "data" / "mg21_radio_full.h"
RADIO_REF = RADIO_FULL if RADIO_FULL.exists() else REPO / "test" / "data" / "mg21_radio_ref.h"
REGS_REF = REPO / "test" / "data" / "mg21_regs_ref.h"

OP_READ, OP_WRITE, OP_TRACE = 1, 2, 3
READ_MAX, WRITE_MAX, TRACE_MAX = 16, 32, 8
TRACE_CYCLES_MAX = 19_200_000
STATUS = ["OK", "BAD_OP", "BAD_LEN", "BAD_COUNT", "BAD_ALIGN", "BAD_CYCLES", "DENIED", "BUSY",
          "TIMEOUT"]
MEMORIES = {"SEQRAM": 0xB0000000, "SEQRAM_S": 0xA0000000, "FRCRAM": 0xB0002000,
            "FRCRAM_S": 0xA0002000}
ALIASES = {"SET": 0x1000, "CLR": 0x2000, "TGL": 0x3000}


def components():
    """Component numbers in dbg.h's enum order, so the tool follows the header, not a copy."""
    text = DBG_H.read_text()
    body = re.search(r"typedef enum\s*\{(.*?)\}\s*Dbg_Component;", text, re.S).group(1)
    names = re.findall(r"\b(DBG_COMP_[A-Z0-9_]+)", body)
    return {name: i + 1 for i, name in enumerate(names)}


def enum_values(first):
    """The members of the anonymous enum that starts with `first` = 1."""
    text = DBG_H.read_text()
    m = re.search(r"enum\s*\{\s*" + first + r"\s*=\s*1,(.*?)\};", text, re.S)
    names = [first] + re.findall(r"\b(DBG_[A-Z0-9_]+)\b", m.group(1))
    return {name: i + 1 for i, name in enumerate(names)}


COMP = components()
MON = enum_values("DBG_MON_RSN_BEGIN")
DBGQ = enum_values("DBG_DBGQ_RSN_OVERFLOW")
COMP_MON, COMP_DBGQ = COMP["DBG_COMP_MON"], COMP["DBG_COMP_DBGQ"]


def load_names():
    """BLOCK -> (secure base, non-secure base), (BLOCK, REG) -> offset, from the checked-in records."""
    bases, offs = {}, {}
    for path, prefix in ((RADIO_REF, "RADIO_REF"), (REGS_REF, "REF")):
        if not path.exists():
            continue
        for line in path.read_text().splitlines():
            m = re.match(rf"#define {prefix}_BASE_([A-Z0-9]+?)(_NS)?\s+(0x[0-9A-Fa-f]+)u", line)
            if m:
                pair = bases.setdefault(m.group(1), [None, None])
                pair[1 if m.group(2) else 0] = int(m.group(3), 16)
                continue
            m = re.match(rf"#define {prefix}_OFF_([A-Z0-9]+)_([A-Z0-9_]+)\s+(0x[0-9A-Fa-f]+)u", line)
            if m:
                offs[(m.group(1), m.group(2))] = int(m.group(3), 16)
    return bases, offs


BASES, OFFS = load_names()


def resolve(text):
    """A number, or NAME[.REG][.SET|.CLR|.TGL][+offset]."""
    text = text.strip()
    extra = 0
    if "+" in text:
        text, plus = text.split("+", 1)
        extra = int(plus, 0)
    try:
        return int(text, 0) + extra
    except ValueError:
        pass
    parts = text.upper().split(".")
    alias = 0
    if parts[-1] in ALIASES:
        alias = ALIASES[parts.pop()]
    head = parts[0]
    if head in MEMORIES and len(parts) == 1:
        return MEMORIES[head] + alias + extra
    secure = head.endswith("_S")
    block = head[:-2] if secure else head
    if block not in BASES:
        raise ValueError(f"unknown block {block!r}")
    base = BASES[block][0 if secure else 1] or BASES[block][0]
    if len(parts) == 1:
        return base + alias + extra
    key = (block, parts[1])
    if key not in OFFS:
        raise ValueError(f"unknown register {block}.{parts[1]}")
    return base + OFFS[key] + alias + extra


def crc16(data):
    """Reflected CRC-16/X.25, the FCS rcp/hdlc.c checks (docs/protocol.md §3)."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x8408 if crc & 1 else 0)
    return crc ^ 0xFFFF


def hdlc(payload):
    crc = crc16(payload)
    body = bytes(payload) + bytes([crc & 0xFF, crc >> 8])
    out = bytearray([0x7E])
    for byte in body:
        if byte in (0x7E, 0x7D):
            out += bytes([0x7D, byte ^ 0x20])
        else:
            out.append(byte)
    out.append(0x7E)
    return bytes(out)


def u32(v):
    return (v & 0xFFFFFFFF).to_bytes(4, "little")


class MonitorError(Exception):
    pass


class Monitor:
    """One open link to the probe image; every request waits for its own reply."""

    def __init__(self, port, baud=115200, timeout=2.0):
        if serial is None:
            sys.exit("mg21_mon: needs pyserial (use .venv/bin/python)")
        self.s = serial.Serial(port, baud, timeout=0.02)
        self.timeout = timeout
        self.buf = bytearray()
        self.tag = 0

    def close(self):
        self.s.close()

    def _records(self):
        """Whole 8-byte records from the stream, resynchronising on a bad header."""
        while len(self.buf) >= 8:
            magic = self.buf[0] | self.buf[1] << 8
            comp = self.buf[2] | self.buf[3] << 8
            if magic != 0 or not 1 <= comp <= len(COMP):
                del self.buf[0]
                continue
            rec = bytes(self.buf[:8])
            del self.buf[:8]
            yield comp, rec[4] | rec[5] << 8, rec[6] | rec[7] << 8

    def _request(self, payload):
        self.tag = (self.tag + 1) & 0xFF
        payload = bytes([payload[0], self.tag]) + payload[1:]
        self.s.reset_input_buffer()
        self.buf.clear()
        self.s.write(hdlc(payload))
        self.s.flush()
        deadline = time.time() + self.timeout
        replay_skip = 0
        inside = False
        words, status, items = [], None, 0
        while time.time() < deadline:
            self.buf += self.s.read(4096)
            for comp, line, rsn in self._records():
                if replay_skip:
                    replay_skip -= 1
                    continue
                if comp == COMP_DBGQ and rsn == DBGQ["DBG_DBGQ_RSN_REPLAY"]:
                    replay_skip = line
                    continue
                if comp != COMP_MON:
                    continue
                if rsn == MON["DBG_MON_RSN_BEGIN"]:
                    inside = line == self.tag
                    words = []
                elif inside and rsn in (MON["DBG_MON_RSN_VALUE_LO"], MON["DBG_MON_RSN_CYCLES_LO"]):
                    words.append(line)
                elif inside and rsn in (MON["DBG_MON_RSN_VALUE_HI"], MON["DBG_MON_RSN_CYCLES_HI"]):
                    if words:
                        words[-1] |= line << 16
                elif inside and rsn == MON["DBG_MON_RSN_END"]:
                    status, items = line & 0xFF, line >> 8
                    return status, items, words
        raise MonitorError(f"no reply to tag {self.tag} within {self.timeout} s")

    def _ok(self, status, allow=("OK",)):
        name = STATUS[status] if status < len(STATUS) else f"status {status}"
        if name not in allow:
            raise MonitorError(name)
        return name

    def read(self, addr, count=1, retries=3):
        out = []
        while count:
            n = min(count, READ_MAX)
            # A reply whose word count does not match is a late answer to an earlier, timed-out
            # request that reused the 8-bit tag; it is retried like a timeout, not accepted.
            for attempt in range(retries):
                try:
                    st, items, words = self._request(bytes([OP_READ]) + u32(addr) + bytes([n]))
                    self._ok(st)
                    if items != n or len(words) != n:
                        raise MonitorError(f"asked {n} words, got {len(words)} (reply said {items})")
                    break
                except MonitorError:
                    if attempt == retries - 1:
                        raise
            out += words
            addr += 4 * n
            count -= n
        return out

    def write(self, addr, values):
        values = list(values)
        while values:
            chunk, values = values[:WRITE_MAX], values[WRITE_MAX:]
            payload = bytes([OP_WRITE]) + u32(addr) + bytes([len(chunk)])
            payload += b"".join(u32(v) for v in chunk)
            st, _, _ = self._request(payload)
            self._ok(st)
            addr += 4 * len(chunk)

    def trace(self, waddr, wvalue, taddr, mask, count=TRACE_MAX, cycles=384_000):
        """Write, then (value, cycles) for the traced word's first value and each change."""
        payload = (bytes([OP_TRACE]) + u32(waddr) + u32(wvalue) + u32(taddr) + u32(mask)
                   + bytes([count]) + u32(cycles))
        st, _, words = self._request(payload)
        name = self._ok(st, ("OK", "TIMEOUT"))
        return [(words[i], words[i + 1]) for i in range(0, len(words) - 1, 2)], name


def main():
    ap = argparse.ArgumentParser(description="ConBee III bench register monitor")
    ap.add_argument("--port", required=True, help="the stick's port, given explicitly")
    ap.add_argument("--baud", type=int, default=115200)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("read")
    r.add_argument("addr")
    r.add_argument("count", nargs="?", default="1")
    w = sub.add_parser("write")
    w.add_argument("addr")
    w.add_argument("values", nargs="+")
    t = sub.add_parser("trace")
    t.add_argument("waddr")
    t.add_argument("wvalue")
    t.add_argument("taddr")
    t.add_argument("mask")
    t.add_argument("--count", type=int, default=TRACE_MAX)
    t.add_argument("--cycles", type=lambda x: int(x, 0), default=384_000,
                   help="budget in core cycles (38.4 per us; max %d)" % TRACE_CYCLES_MAX)
    a = ap.parse_args()

    mon = Monitor(a.port, a.baud)
    try:
        if a.cmd == "read":
            addr = resolve(a.addr)
            for i, v in enumerate(mon.read(addr, int(a.count, 0))):
                print(f"0x{addr + 4 * i:08X}  0x{v:08X}")
        elif a.cmd == "write":
            mon.write(resolve(a.addr), [int(v, 0) for v in a.values])
            print("ok")
        else:
            samples, end = mon.trace(resolve(a.waddr), int(a.wvalue, 0), resolve(a.taddr),
                                     int(a.mask, 0), a.count, a.cycles)
            for value, cyc in samples:
                print(f"{cyc:>10} cycles  {cyc / 38.4:>10.2f} us  0x{value:08X}")
            print(end)
    except (MonitorError, ValueError) as e:
        sys.exit(f"mg21_mon: {e}")
    finally:
        mon.close()


if __name__ == "__main__":
    main()
