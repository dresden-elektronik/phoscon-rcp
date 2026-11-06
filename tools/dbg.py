#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""Decode the Phoscon RCP binary debug records into text.

The firmware carries no debug strings (docs/coding-rules.md §5): every
diagnostic is one 8-byte little-endian record

    u16 magic 0x0000 | u16 component | u16 line | u16 reason

and src/hal/dbg.h says outright that "The host-side string table lives in
tools/dbg.py" - this file is it.  The tables are not duplicated here: they are
parsed out of src/hal/dbg.h every run, so a new reason code becomes readable by
editing the header, and a forgotten one shows up as UNKNOWN instead of a wrong
name.  A record's `line` field is __LINE__ baked into the image, so a record can
also be resolved to the source text that emitted it; when that text no longer
looks like a DBG() call the record is older than the tree, and this says so.

Where records come from:
  --file PATH / '-'      a raw byte stream (a CDC capture, a hex-dumped blob)
  --log PATH             a QEMU / harness log: decodes every
                         "samr21-usb-host: EP1 IN n bytes: <hex ...>" payload,
                         and every "peek 0x0003e000: <hex ...>" scratch row
  --port PATH            live CDC serial (pyserial), records as they arrive
  --scratch HEX          one scratch row / 16 hex bytes, printed as is

Examples:
  tools/dbg.py --list
  tools/dbg.py --log /tmp/cb2emu16/frz.records.log
  .venv/bin/python tools/dbg.py --port /dev/cu.usbmodem31432201 --follow
  tools/dbg.py --file capture.bin --json
  tools/dbg.py --scratch "08 a0 01 00 01 b0 04 00 f3 00 ff ff ff ff ff ff"
"""

import argparse
import json
import os
import re
import sys
import time

RECORD_BYTES = 8
MAGIC = 0x0000
STAGE_MIN = 0x01
STAGE_MAX = 0x09
MARK_BIT = 0x80          # a stage byte >= 0x80 is a DBG reason mark (AGENTS.md)


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# --------------------------------------------------------------------------
# The tables, read from the source
# --------------------------------------------------------------------------

def parse_dbg_header(root):
    """components, and reason codes **per component**, read out of the tree.

    Two sources, both real: dbg.h declares the shared blocks as enums, and the
    modules extend them locally with `#define DBG_CLOCK_RSN_PC16_MUX 13u`
    (samr21_clock.c:133-136) or `#define DBG_USB_RSN_SYNC_TIMEOUT 4u`
    (samr21_usbc.c:857).  Reading only dbg.h would call those UNKNOWN.

    Reason codes are module-local ("u16 reason  module-local reason code"), so
    value 1 is DBG_CORE_RSN_EXCEPTION and DBG_CLOCK_RSN_48MHZ_READY at the same
    time: a decode is always (component, reason), and the component's own block
    decides the name.  Blocks and defines are matched to their component by the
    naming - DBG_COMP_CLOCK <-> DBG_CLOCK_RSN_*.
    """
    components = {}
    reasons = {}          # component name -> {value: [(name, note, origin)]}
    order = []

    for path, rel in source_files(root):
        text = open(path, encoding="utf-8").read()
        take(text, rel, components, reasons, order)
        take_defines(text, rel, reasons, order)

    for comp in reasons:
        for value in reasons[comp]:
            reasons[comp][value].sort(key=lambda e: (not e[2].startswith("src/hal/dbg.h"),
                                                     e[0]))
    order.sort(key=lambda e: (e[0], e[1], not e[3].startswith("src/hal/dbg.h"), e[2]))
    return components, reasons, order


def source_files(root):
    for dirpath, dirs, files in os.walk(os.path.join(root, "src")):
        dirs.sort()
        for fn in sorted(files):
            if fn.endswith((".c", ".h")):
                path = os.path.join(dirpath, fn)
                yield path, os.path.relpath(path, root)


# One C comment, then an enum block, optionally named.  A permissive
# /\*.*?\*/ would reach past the end of a comment into a later one and swallow the
# enum in between - it did, and the component enum vanished.
ONE_COMMENT = r"/\*[^*]*(?:\*(?!/)[^*]*)*\*/"
ENUM_RE = re.compile(r"(%s\n\s*)?(typedef\s+)?enum\s*\{(.*?)\}(?:\s*(\w+)\s*)?;"
                     % ONE_COMMENT, re.S)
# A module-local reason code, and its comment may run onto the following
# lines (samr21_clock.c:136-137 does exactly that) - a pattern anchored at
# end-of-line silently drops those, which showed up as UNKNOWN(0x0011) for a
# code that was declared. The comment is matched as one balanced C comment.
ONE_COMMENT = r"/\*[^*]*(?:\*(?!/)[^*]*)*\*/"
DEFINE_RE = re.compile(r"#define\s+(DBG_(\w+)_RSN_(\w+))\s+((?:0x)?[0-9A-Fa-f]+)[uU]?\s*"
                       r"(%s)?" % ONE_COMMENT, re.M)


def take(text, rel, components, reasons, order):
    for m in ENUM_RE.finditer(text):
        note, _typedef, body, name = m.groups()
        entries = enum_entries(body)
        if not entries:
            continue
        note_text = comment_text(note) if note else ""
        if name == "Dbg_Component":
            if not components:
                components.update({v: k for k, v in entries})
            continue
        for rname, value in entries:
            mm = re.match(r"DBG_(\w+?)_RSN_(.+)", rname)
            if not mm:
                continue
            comp = "DBG_COMP_" + mm.group(1)
            reasons.setdefault(comp, {}).setdefault(value, []).append(
                (rname, note_text, rel))
            order.append((comp, value, rname, rel, note_text))


def take_defines(text, rel, reasons, order):
    for m in DEFINE_RE.finditer(text):
        rname, comp, _short, value, note = m.groups()
        note = comment_text(note) if note else ""
        value = int(value, 0)
        comp = "DBG_COMP_" + comp
        reasons.setdefault(comp, {}).setdefault(value, []).append(
            (rname, note or "", rel))
        order.append((comp, value, rname, rel, note or ""))


def enum_entries(body):
    """(name, value) pairs honouring C's implicit increment and inline comments.

    Comments come off **before** splitting on commas: a comment inside an enum can
    contain commas, and cutting first leaves a fragment whose unterminated comment
    makes the first stray word look like the entry (it did: the RF block's
    DBG_RF_RSN_WAKE_FAIL came out as 'so').
    """
    out = []
    value = 0
    body = re.sub(r"/\*.*?\*/", " ", body, flags=re.S)
    for raw in body.split(","):
        chunk = raw.strip()
        if not chunk:
            continue
        m = re.match(r"([A-Za-z_]\w*)\s*(?:=\s*([^;]+))?", chunk)
        if not m:
            continue
        name = m.group(1)
        if m.group(2) is not None:
            try:
                value = int(m.group(2).split()[0], 0)
            except ValueError:
                pass
        out.append((name, value))
        value += 1
    return out


def comment_text(note):
    lines = note.strip().splitlines()
    lines = [re.sub(r"^\s*/?\*+", "", l).strip() for l in lines]
    lines = [l for l in lines if l and not l.startswith("*/") and not l.startswith("----")]
    return " ".join(lines).strip().rstrip("*/ ").rstrip("*")


def component_files(root, components):
    """component name -> the .c files that emit it (a `line` is only meaningful
    inside one of those)."""
    found = {c: [] for c in components.values()}
    mentioned = {}
    for dirpath, _dirs, files in os.walk(os.path.join(root, "src")):
        for fn in files:
            if not fn.endswith((".c", ".h")):
                continue
            path = os.path.join(dirpath, fn)
            try:
                text = open(path, encoding="utf-8").read()
            except OSError:
                continue
            for comp in found:
                if re.search(r"\bDBG\(\s*%s\b|\bDebug_Emit\(\s*%s\b" % (comp, comp), text):
                    found[comp].append(os.path.relpath(path, root))
                elif re.search(r"\b%s\b" % comp, text):
                    mentioned.setdefault(comp, []).append(os.path.relpath(path, root))
    for comp in found:
        if not found[comp]:
            # No call site: a file that only names the component (the DBGQ marker
            # lives in dbg.c, the CORE one in the panic handler) is still the only
            # place its `line` field can mean anything.
            found[comp] = mentioned.get(comp, [])
        found[comp].sort()
    return found


def source_line(root, relpath, line):
    try:
        with open(os.path.join(root, relpath), encoding="utf-8") as f:
            for i, text in enumerate(f, 1):
                if i == line:
                    return text.rstrip("\n")
    except OSError:
        return None
    return None


# --------------------------------------------------------------------------
# Decoding
# --------------------------------------------------------------------------

# --------------------------------------------------------------------------
# The ConBee III's bring-up witnesses, decoded
# --------------------------------------------------------------------------
# A witness record's line field is one 16-bit register value, named by its reason
# (src/hal/dbg.h). On the MG21 the registers whose bits a bring-up actually reads are wider than
# 16 bits, so hal/mg21/mg21_clock.c and hal/mg21/mg21_uart.c send them as a pair: the low half
# under the reason, the high half under the same name with _HI. The field tables below decode the
# half each record carries, in the part's own field names.

_MODE_LABELS = {0: "disabled", 1: "input", 2: "input-pull", 4: "push-pull", 8: "wired-and",
                14: "wired-and+pull-up"}

MG21_WITNESS_FIELDS = {
    "DBG_WITNESS_RSN_MG21_SYSCLKCTRL": [
        ("CLKSEL", 0x7, 0, {0: "none", 1: "FSRCO", 2: "HFRCODPLL", 3: "HFXO"}),
        ("PCLKPRESC", 0x400, 10, {0: "/1", 1: "/2"}),
        ("HCLKPRESC", 0x3000, 12, {0: "/1", 1: "/2", 3: "/4"})],
    "DBG_WITNESS_RSN_MG21_EM01GRPACLK": [
        ("CLKSEL", 0x7, 0, {0: "none", 1: "HFRCODPLL", 2: "HFXO", 4: "FSRCO"})],
    "DBG_WITNESS_RSN_MG21_EM23GRPACLK": [
        ("CLKSEL", 0x3, 0, {0: "none", 1: "LFRCO", 2: "LFXO", 3: "ULFRCO"})],
    "DBG_WITNESS_RSN_MG21_WDOG_CLKSEL": [
        ("CLKSEL", 0x7, 0, {0: "none", 1: "LFRCO", 2: "LFXO", 3: "ULFRCO", 4: "HCLK/1024"})],
    "DBG_WITNESS_RSN_MG21_HFXO_STATUS": [("RDY", 0x1, 0, None)],
    "DBG_WITNESS_RSN_MG21_HFXO_STATUS_HI": [("ENS", 0x1, 0, None), ("HWREQ", 0x2, 1, None)],
    "DBG_WITNESS_RSN_MG21_HFXO_CTRL": [("FORCEEN", 0x1, 0, None), ("DISONDEMAND", 0x2, 1, None)],
    "DBG_WITNESS_RSN_MG21_LFRCO_STATUS": [("RDY", 0x1, 0, None)],
    "DBG_WITNESS_RSN_MG21_LFRCO_STATUS_HI": [("ENS", 0x1, 0, None)],
    "DBG_WITNESS_RSN_MG21_WDOG_CFG": [
        ("CLRSRC", 0x1, 0, {0: "CMD clears", 1: "PRS clears"}),
        ("EM2RUN", 0x2, 1, None), ("EM3RUN", 0x4, 2, None), ("EM4BLOCK", 0x8, 3, None),
        ("DEBUGRUN", 0x10, 4, None),
        ("WDOGRSTDIS", 0x100, 8, {0: "timeout resets", 1: "interrupt only"})],
    "DBG_WITNESS_RSN_MG21_WDOG_CFG_HI": [("PERSEL", 0xF, 0, None)],
    "DBG_WITNESS_RSN_MG21_WDOG_EN": [("EN", 0x1, 0, None)],
    "DBG_WITNESS_RSN_MG21_SYSTICK_CTRL": [
        ("ENABLE", 0x1, 0, None), ("TICKINT", 0x2, 1, None),
        ("CLKSOURCE", 0x4, 2, {0: "external clock", 1: "processor clock"})],
    "DBG_WITNESS_RSN_MG21_SYSTIC_CFG": [("SYSTICEXTCLKEN", 0x1, 0, None)],
    "DBG_WITNESS_RSN_MG21_USART_EN": [("EN", 0x1, 0, None)],
    "DBG_WITNESS_RSN_MG21_USART_CTRL": [
        ("SYNC", 0x1, 0, {0: "async", 1: "sync"}),
        ("OVS", 0x60, 5, {0: "X16", 1: "X8", 2: "X6", 3: "X4"})],
    "DBG_WITNESS_RSN_MG21_USART_FRAME": [
        ("DATABITS", 0xF, 0, {5: "8"}),
        ("PARITY", 0x300, 8, {0: "none", 1: "even", 2: "odd"}),
        ("STOPBITS", 0x3000, 12, {1: "one", 2: "two"})],
    "DBG_WITNESS_RSN_MG21_USART_CLKDIV": [("DIV[12:3]", 0x7F8, 3, None)],
    "DBG_WITNESS_RSN_MG21_USART_CLKDIV_HI": [("DIV[22:16]", 0x7F, 0, None)],
    "DBG_WITNESS_RSN_MG21_USART_STATUS": [
        ("RXENS", 0x1, 0, None), ("TXENS", 0x2, 1, None), ("TXC", 0x20, 5, None),
        ("TXBL", 0x40, 6, None), ("RXDATAV", 0x80, 7, None), ("TXIDLE", 0x2000, 13, None)],
    "DBG_WITNESS_RSN_MG21_USART_IEN": [
        ("TXC", 0x1, 0, None), ("TXBL", 0x2, 1, None), ("RXDATAV", 0x4, 2, None),
        ("RXOF", 0x10, 4, None), ("FERR", 0x200, 9, None)],
    "DBG_WITNESS_RSN_MG21_GPIO_ROUTEEN": [("RXPEN", 0x4, 2, None), ("TXPEN", 0x10, 4, None)],
    "DBG_WITNESS_RSN_MG21_GPIO_TXROUTE": [("PORT", 0x3, 0, {0: "A", 1: "B"})],
    "DBG_WITNESS_RSN_MG21_GPIO_TXROUTE_HI": [("PIN", 0xF, 0, None)],
    "DBG_WITNESS_RSN_MG21_GPIO_RXROUTE": [("PORT", 0x3, 0, {0: "A", 1: "B"})],
    "DBG_WITNESS_RSN_MG21_GPIO_RXROUTE_HI": [("PIN", 0xF, 0, None)],
    "DBG_WITNESS_RSN_MG21_GPIO_MODEL": [("MODE0", 0xF, 0, _MODE_LABELS),
                                        ("MODE1", 0xF0, 4, _MODE_LABELS)],
}

MG21_WITNESS_MEANING = {
    "DBG_WITNESS_RSN_MG21_TICK_PERIOD": "milliseconds one SysTick period is worth",
    "DBG_WITNESS_RSN_MG21_CYCCNT": "DWT CYCCNT low half: proof the cycle counter counts",
    "DBG_WITNESS_RSN_MG21_CHIPREV": "SYSCFG CHIPREV low half: family and revision words",
    "DBG_WITNESS_RSN_MG21_USART_RATE": "achieved line rate, low half (the _HI record carries the rest)",
    "DBG_WITNESS_RSN_MG21_USART_RATE_HI": "achieved line rate, high half",
    "DBG_WITNESS_RSN_MG21_UART_VECTORS": "link vector entries since boot",
    "DBG_WITNESS_RSN_MG21_UART_RX_OF": "octets the receiver dropped as it overflowed",
    "DBG_WITNESS_RSN_MG21_UART_RX_FERR": "octets delivered as framed wrong",
    "DBG_WITNESS_RSN_MG21_FAULT_COMPONENT": "the component a previous run panicked in",
    "DBG_WITNESS_RSN_MG21_FAULT_DETAIL": "the reason a previous run panicked with",
    "DBG_WITNESS_RSN_MG21_SYSTICK_LOAD": "SysTick LOAD low half (the pair is repeated for both halves)",
    "DBG_WITNESS_RSN_MG21_HEARTBEAT": "seconds since boot: the live link report follows",
    "DBG_WITNESS_RSN_MG21_UART_RX_VECTORS": "receive vector entries since boot",
    "DBG_WITNESS_RSN_MG21_UART_RX_OCTETS": "octets handed to the link since boot",
    "DBG_WITNESS_RSN_MG21_UART_RX_LAST": "RXDATAX of the last octet read: data [8:0], FERR 15",
    "DBG_WITNESS_RSN_MG21_USART_IF": "USART0 IF: TXC 0, TXBL 1, RXDATAV 2, RXOF 4, FERR 9",
    "DBG_WITNESS_RSN_MG21_GPIO_DIN": "link port input levels: RX on PB0 idles at 1",
    "DBG_WITNESS_RSN_MG21_NVIC_ISER0": "enabled IRQs 0..15: USART0 RX 11, TX 12",
}


def mg21WitnessFields(reason, value):
    fields = MG21_WITNESS_FIELDS.get(reason)
    if fields is None:
        return None
    out = []
    for name, mask, shift, labels in fields:
        v = (value & mask) >> shift
        if labels is None:
            out.append("%s=%d" % (name, v))
        else:
            out.append("%s=%s" % (name, labels.get(v, str(v))))
    return " ".join(out)



class Decoder:
    def __init__(self, root, components, reasons, comp_files):
        self.root = root
        self.components = components
        self.reasons = reasons
        self.comp_files = comp_files

    def name(self, comp):
        return self.components.get(comp, "UNKNOWN(%d)" % comp)

    def explain(self, rec):
        comp, line, reason = rec
        cname = self.name(comp)
        entries = self.reasons.get(cname, {}).get(reason)
        if not entries:
            return cname, "UNKNOWN(0x%04x)" % reason, "", None
        name, note, origin = entries[0]
        if len(entries) > 1:
            name += " (also " + ", ".join(e[0] for e in entries[1:]) + ")"
        return cname, name, note, origin

    def resolve(self, rec):
        """([file:line text...], emitted_here?) for the record's line field."""
        cname, _rname, _note, _origin = self.explain(rec)
        hits = []
        for rel in self.comp_files.get(cname, []):
            text = source_line(self.root, rel, rec[1])
            if text is not None:
                hits.append((rel, text))
        if not hits:
            return None
        emits = [h for h in hits if "DBG(" in h[1] or "Debug_Emit" in h[1]]
        return hits, emits

    def record(self, rec, ts=None):
        cname, rname, note, origin = self.explain(rec)
        out = {"component": cname, "line": rec[1], "reason": rname,
               "reason_value": rec[2]}
        if cname == "DBG_COMP_WITNESS":
            # dbg.h: a witness record's line field is the 16-bit value of the
            # register named by its reason - not a source line.
            out["register_value"] = rec[1]
            out["line_meaning"] = ("witness: the line field is the value 0x%04x "
                                   "read from the register named by the reason, "
                                   "not a source line" % rec[1])
            decoded = mg21WitnessFields(rname, rec[1])
            if decoded is not None:
                out["mg21_fields"] = decoded
            elif rname in MG21_WITNESS_MEANING:
                out["mg21_fields"] = MG21_WITNESS_MEANING[rname]
            if note:
                out["note"] = note
            if origin:
                out["declared_in"] = origin
            if ts is not None:
                out["t"] = ts
            out["suspicious"] = False
            return out
        if rname in ("DBG_APP_RSN_FRAMES_RX", "DBG_APP_RSN_FRAMES_FILTERED"):
            # dbg.h: app data-path counters - the line field is the count.
            out["line_meaning"] = ("counter: the line field is %d, not a source line"
                                   % rec[1])
            if ts is not None:
                out["t"] = ts
            out["suspicious"] = False
            return out
        if cname == "DBG_COMP_DBGQ" and rname == "DBG_DBGQ_RSN_REPLAY":
            # dbg.h: a replay marker's line field is how many records follow, all
            # of them byte-identical copies of a set the host was already offered.
            out["replayed"] = rec[1]
            out["line_meaning"] = ("replay: the next %d records are a re-run of the "
                                   "boot set, not a new boot (their frames are "
                                   "identical either way)" % rec[1])
            if ts is not None:
                out["t"] = ts
            out["suspicious"] = False
            return out
        if cname == "DBG_COMP_USB" and rname == "DBG_USB_RSN_SETUP_COUNT":
            # dbg.h: the line field is how many distinct SETUP packets the host
            # sent before this configuration - a count, not a source line.
            out["count"] = rec[1]
            out["line_meaning"] = ("the host latched %d distinct SETUP packets "
                                   "before this SET_CONFIGURATION; the line field "
                                   "is that count, not a source line" % rec[1])
            if ts is not None:
                out["t"] = ts
            out["suspicious"] = False
            return out
        if cname == "DBG_COMP_TIME" and rname == "DBG_TIME_RSN_CADENCE":
            # dbg.h: the cadence probe's line field is the raw SysTick period
            # counter, not cycles/4096 - reading it as cycles would multiply a
            # tick count by 4096 and report a duration 48000/4096 too small.
            out["ticks"] = rec[1]
            out["cycles"] = rec[1] * 48000
            out["line_meaning"] = ("cadence probe: the line field is %d SysTick "
                                   "periods since SysTick started (a period is "
                                   "LOAD+1 = 48000 processor clocks), not a source "
                                   "line and not cycles/4096" % rec[1])
            if ts is not None:
                out["t"] = ts
            out["suspicious"] = False
            return out
        if cname == "DBG_COMP_TIME" and rname == "DBG_TIME_RSN_TICK_MS":
            out["tick_ms"] = rec[1]
            out["line_meaning"] = ("stopwatch: the line field is Tick_GetMs() "
                                   "truncated to 16 bits at SOF_END - SysTick "
                                   "ticks, not a source line")
            if ts is not None:
                out["t"] = ts
            out["suspicious"] = False
            return out
        if cname == "DBG_COMP_TIME" and rname in ("DBG_TIME_RSN_SOF_FRAMES",
                                                  "DBG_TIME_RSN_SOF_FLAGS"):
            # dbg.h: this milestone's line field is the module's own host-frame
            # count (FNUM) between SOF_START and SOF_END - the divisor of the
            # clock proof, not a cycle count and not a source line.
            what = ("host frames counted by USBC FNUM" if rname.endswith("FRAMES")
                    else "SOF flags our USB vector acknowledged")
            out["frames"] = rec[1]
            out["line_meaning"] = ("stopwatch: the line field is %d %s between "
                                   "SOF_START and SOF_END, not a source line"
                                   % (rec[1], what))
            if ts is not None:
                out["t"] = ts
            out["suspicious"] = False
            return out
        if cname == "DBG_COMP_TIME":
            # dbg.h: a stopwatch record's line field is the elapsed processor
            # cycles since Clock_BringupClockStart() in units of 4096 - the same
            # repurposing the DBGQ marker does with its drop count.  Reading it
            # as a source line would report a location that does not exist.
            units = rec[1]
            cycles = units * 4096
            out["cycles"] = cycles
            out["line_meaning"] = ("stopwatch: the line field is %d x 4096 = %d "
                                   "cycles since Clock_BringupClockStart(), not a "
                                   "source line" % (units, cycles))
            if units == 0xFFFF:
                out["cycles_at_least"] = True
            out["ms"] = {"8MHz": round(cycles / 8e6 * 1e3, 3),
                         "48MHz": round(cycles / 48e6 * 1e3, 3)}
            if ts is not None:
                out["t"] = ts
            out["suspicious"] = False
            return out
        if cname == "DBG_COMP_DBGQ":
            # dbg.h: an overflow marker's line field is "not a source line but the
            # number of records dropped so far".  Decoding it as a line number would
            # report a source location that does not exist.
            out["dropped_records"] = rec[1]
            out["line_meaning"] = ("DBGQ marker: the line field is the number of "
                                   "records dropped by the queue, not a source line")
            if ts is not None:
                out["t"] = ts
            out["suspicious"] = False
            return out
        if note:
            out["note"] = note
        if origin:
            out["declared_in"] = origin
        resolved = self.resolve(rec)
        suspicious = False
        if resolved:
            hits, emits = resolved
            out["where"] = ["%s:%d  %s" % (f, rec[1], t.strip()) for f, t in hits]
            if any("_RSN_" not in t for _f, t in emits):
                out["variable_reason"] = ("the reason is passed as a variable at this "
                                          "line, so only the number identifies it")
            if not emits:
                out["stale"] = ("line %d exists but holds no DBG() call - the image is "
                                "older than this source tree" % rec[1])
                suspicious = True
        else:
            out["stale"] = ("no file that emits %s has a line %d - the image is older "
                            "than this source tree" % (cname, rec[1]))
            suspicious = True
        if ts is not None:
            out["t"] = ts
        out["suspicious"] = suspicious
        return out

    def text(self, rec, ts=None, prefix=""):
        d = self.record(rec, ts)
        if "cycles" in d:
            field = ("value 0x%04x" % d["register_value"]) if "register_value" in d else \
                ("%dk cyc" % (d["cycles"] // 1000)
                 if "cycles" in d and d["cycles"] >= 1000 else
                 ("%d cyc" % d["cycles"] if "cycles" in d else "line %-5d" % d["line"]))
        elif "dropped_records" in d:
            field = "dropped %-5d" % d["line"]
        else:
            field = "line %-5d" % d["line"]
        head = "%s%s  %s reason=%s%s" % (
            prefix, d["component"], field, d["reason"],
            "  [STALE]" if d.get("stale") else "")
        extra = []
        if d.get("ms"):
            extra.append("  = %s ms at 8 MHz, %s ms at 48 MHz%s"
                         % (d["ms"]["8MHz"], d["ms"]["48MHz"],
                            "  (saturated: at least this many)"
                            if d.get("cycles_at_least") else ""))
        if d.get("line_meaning"):
            extra.append(d["line_meaning"])
        if d.get("note"):
            extra.append(d["note"])
        if d.get("variable_reason"):
            extra.append(d["variable_reason"])
        extra.extend(d.get("where", []))
        if d.get("stale"):
            extra.append(d["stale"])
        return head, extra


def iter_records(data, decoder=None, strict_magic=True):
    """Yield (component, line, reason) from a byte string, 8 bytes at a time."""
    for off in range(0, len(data) - RECORD_BYTES + 1, RECORD_BYTES):
        chunk = data[off:off + RECORD_BYTES]
        magic, comp, line, reason = (int.from_bytes(chunk[i:i + 2], "little")
                                     for i in (0, 2, 4, 6))
        if strict_magic and magic != MAGIC:
            yield None
            continue
        yield (comp, line, reason)


EP_IN_RE = re.compile(r"EP(\d+) IN (\d+) bytes?: ((?:[0-9A-Fa-f]{2}[\s]*)+)", re.I)
PEEK_RE = re.compile(r"peek t=(\d+) (0x[0-9a-f]+): ((?:[0-9A-Fa-f]{2}[\s]*)+)", re.I)
PEEK2_RE = re.compile(r"at (0x[0-9a-f]+): ((?:[0-9A-Fa-f]{2}[\s]*)+)")


def hexbytes(tok):
    return bytes(int(b, 16) for b in tok.split())


def ascii_hint(payload):
    """Most non-record CDC traffic on this board is text (the vendor flasher
    protocol answers with "unlock! ", "VALID CRC 0x… " and friends), so say so
    instead of only calling it malformed."""
    printable = bytes(b if 0x20 <= b < 0x7F else 0x2E for b in payload)
    return printable.decode("ascii")


def decode_log(path, decoder, emit, endpoints, note_bad):
    stats = {"ep_in": 0, "skipped_ep": 0, "records": 0, "bad": 0, "scratch": 0}
    for raw in open(path, "rb"):
        line = raw.decode("utf-8", "replace").strip()
        m = EP_IN_RE.search(line)
        if m:
            ep = int(m.group(1))
            payload = hexbytes(m.group(3))
            if ep not in endpoints:
                stats["skipped_ep"] += 1
                continue
            stats["ep_in"] += 1
            if not payload:
                continue
            if len(payload) % RECORD_BYTES:
                note_bad(ep, len(payload), "not a whole number of 8-byte records",
                         ascii_hint(payload))
                stats["bad"] += 1
                continue
            for rec in iter_records(payload):
                if rec is None:
                    stats["bad"] += 1
                    continue
                stats["records"] += 1
                emit("record", decoder.record(rec),
                     decoder.text(rec, prefix="  EP%d " % ep))
            if any(int.from_bytes(payload[i:i + 2], "little") != MAGIC
                   for i in range(0, len(payload), RECORD_BYTES)):
                note_bad(ep, len(payload), "no 0x0000 magic field",
                         ascii_hint(payload))
            continue
        m = PEEK_RE.search(line)
        if m:
            addr, data = m.group(2), hexbytes(m.group(3))
        else:
            m = PEEK2_RE.search(line)
            if not m:
                continue
            addr, data = m.group(1), hexbytes(m.group(2))
        if len(data) >= 8:
            stats["scratch"] += 1
            d = scratch_row(data)
            d["addr"] = addr
            emit("scratch", d, scratch_text(d))
    return stats


def scratch_row(data):
    """Post-mortem scratch row 0x3E000 (src/hal/samr21/samr21_nvram.c:318-343):
    word0 = 0xA000 | stage (milestones only), word1 = seconds since boot,
    word2 = 0xB000 | reason, word3 = component, word4 = line of the last record,
    words 5.. unused (0xFF on an erased row)."""
    words = [int.from_bytes(data[i:i + 2], "little") for i in range(0, min(len(data), 16), 2)]
    out = {"words": ["%04x" % w for w in words]}
    if not words:
        return out
    stage = words[0] & 0xFF
    out["stage"] = "%02x" % stage
    out["stage_marker"] = "%04x" % (words[0] & 0xFF00)
    if stage == 0xFF or stage == 0x00:
        out["stage_note"] = "no milestone recorded (erased row, or nothing written yet)"
    elif stage & MARK_BIT:
        out["stage_note"] = ("0x80 mark in the milestone word - that word is for "
                             "milestones only, so this is a pre-fix image")
    else:
        out["stage_note"] = STAGES.get(stage, "unknown stage")
    if len(words) > 1:
        out["seconds"] = words[1]
    if len(words) >= 5:
        reason_word, comp, line = words[2], words[3], words[4]
        if comp or (reason_word & 0xFF00) == 0xB000:
            out["last_record"] = {"word": "%04x" % reason_word, "component": comp,
                                  "line": line, "reason": reason_word & 0xFF,
                                  "marker_ok": (reason_word & 0xFF00) == 0xB000}
    return out


STAGES = {
    0x01: "entry (Reset_Handler reached our code)",
    0x02: "generators not up (GCLKIN / gen1 probe failed = dead CLKM)",
    0x03: "generators up, DFLL reference or lock died",
    0x04: "closed-loop lock obtained, but the CPU switch / WDT channel / "
          "peripheral-channel steps did not finish",
    0x05: "clocks done",
    0x06: "USBCRM failover ran",
    0x07: "host link init done (CDC or SERCOM USART, per board)",
    0x08: "full bring-up on the closed-loop clocks",
    0x09: "full bring-up on the USBCRM failover",
}


def scratch_text(d):
    head = "scratch %s %s" % (d.get("addr", ""), " ".join(d["words"]))
    extra = []
    if "stage_note" in d:
        extra.append("stage 0x%s: %s" % (d["stage"], d["stage_note"]))
    if "seconds" in d:
        extra.append("liveness: %d s since boot (word1 - the primary liveness evidence)"
                     % d["seconds"])
    if "last_record" in d:
        r = d["last_record"]
        extra.append("last record: word %s, component %d, line %d, reason 0x%02x%s"
                     % (r["word"], r["component"], r["line"], r["reason"],
                        "" if r["marker_ok"] else "  [no 0xB0 marker - pre-fix row]"))
    return head, extra


def decode_stream(data, decoder, emit, no_bad=False):
    """A raw byte stream (a CDC capture): frames are 8 bytes, no framing, no
    checksum - so a stream that mixed in the bootloader's text has to be reported
    as such instead of being called malformed 90 times."""
    n = bad = 0
    samples = []
    for index, rec in enumerate(iter_records(data)):
        if rec is None:
            bad += 1
            if len(samples) < 3:
                at = index * RECORD_BYTES
                samples.append((at, data[at:at + RECORD_BYTES]))
            continue
        n += 1
        emit("record", decoder.record(rec), decoder.text(rec))
    if bad and not no_bad:
        for at, chunk in samples:
            emit("bad", {"frame": at, "bytes": chunk.hex(" "),
                         "why": "no 0x0000 magic field",
                         "ascii": ascii_hint(chunk)})
        if bad > len(samples):
            emit("bad", {"bytes": 0, "why": "%d further non-record frame(s) not shown"
                                        % (bad - len(samples)), "ascii": ""})
    return n, bad, len(data)


def follow_port(port, baud, decoder, emit, stop_after=None):
    try:
        import serial                                  # tools/.venv has pyserial
    except ImportError:
        sys.exit("tools/dbg.py --port needs pyserial: .venv/bin/python tools/dbg.py ...")
    buf = bytearray()
    n = 0
    with serial.Serial(port, baud, timeout=0.2) as ser:
        emit("info", {"port": port, "baud": baud})
        while True:
            chunk = ser.read(64)
            if chunk:
                buf += chunk
                while len(buf) >= RECORD_BYTES:
                    body = bytes(buf[:RECORD_BYTES])
                    del buf[:RECORD_BYTES]
                    rec = next(iter_records(body))
                    if rec is None:
                        emit("bad", {"why": "magic field is not 0x0000",
                                     "bytes": body.hex(" ")})
                        continue
                    n += 1
                    ts = time.strftime("%H:%M:%S")
                    emit("record", decoder.record(rec, ts),
                         decoder.text(rec, ts, prefix="%s  " % ts))
                    if stop_after and n >= stop_after:
                        return n
            else:
                time.sleep(0.01)


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    root = repo_root()
    ap.add_argument("--src", default=root, help="repo root holding src/hal/dbg.h")
    ap.add_argument("--file", help="raw record bytes ('-' for stdin)")
    ap.add_argument("--log", help="QEMU / harness log (EP1 IN payloads + peek rows)")
    ap.add_argument("--port", help="live CDC serial device")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--follow", action="store_true", help="keep reading --port")
    ap.add_argument("--limit", type=int, help="stop after N records (--port)")
    ap.add_argument("--scratch", help="one scratch row as hex bytes")
    ap.add_argument("--ep", action="append", type=int,
                    help="which IN endpoints to decode from a --log (default 1 and 3, "
                         "the CDC data pipes; EP0 is descriptor/string traffic)")
    ap.add_argument("--quiet", action="store_true", help="no per-record output")
    ap.add_argument("--no-bad", action="store_true",
                    help="do not report payloads that are not record frames")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--timeline", action="store_true",
                    help="print only the DBG_COMP_TIME stopwatch records, in order, "
                         "with the cycles each stage cost (dbg.h: the line field is "
                         "cycles/4096 since Clock_BringupClockStart)")
    ap.add_argument("--list", action="store_true", help="print the parsed tables")
    ap.add_argument("--explain", help="explain one code: <COMP>.<RSN> or a number")
    ap.add_argument("--strict-framing", action="store_true",
                    help="raw streams: insist on the fixed 8-byte framing instead of "
                         "resynchronising after foreign bytes (the dbg.h contract; use "
                         "it when the capture is known to be our app alone)")
    ap.add_argument("--strict", action="store_true",
                    help="exit 1 when any component or reason is unknown")
    args = ap.parse_args()

    header = os.path.join(args.src, "src", "hal", "dbg.h")
    if not os.path.exists(header):
        sys.exit("dbg.h not found at %s (run from the repo or pass --src)" % header)
    components, reasons, order = parse_dbg_header(args.src)
    comp_files = component_files(args.src, components)
    decoder = Decoder(args.src, components, reasons, comp_files)

    unknown = []
    timeline = []

    def emit(kind, data, textual=None):
        if kind == "record":
            if data["component"].startswith("UNKNOWN") or data["reason"].startswith("UNKNOWN"):
                unknown.append(data)
            if data["component"] == "DBG_COMP_TIME":
                timeline.append((data["reason"], data.get("cycles"),
                                 data.get("cycles_at_least", False),
                                 data.get("frames"), data.get("tick_ms")))
        if args.json:
            print(json.dumps({"kind": kind, **data}, ensure_ascii=False))
            return
        if kind == "bad":
            if not args.quiet:
                where = ("EP%d" % data["ep"]) if "ep" in data else (
                    "frame at +%d" % data["frame"] if "frame" in data else "stream")
                size = data.get("bytes", "")
                if isinstance(size, bytes):
                    size = len(size)
                print("  %-16s %-4s not a record: %-34s %s"
                      % (where, size, data["why"],
                         "as text: %r" % data["ascii"] if data.get("ascii") else ""))
            return
        if textual:
            # --timeline is a summary view: the other records would only bury it.
            if not args.quiet and not (args.timeline
                                       and data.get("component") != "DBG_COMP_TIME"):
                head, extra = textual
                print(head)
                for e in extra:
                    print("      " + e)
            return
        print(json.dumps({"kind": kind, **data}, ensure_ascii=False))

    if args.list:
        print("components (src/hal/dbg.h):")
        for value in sorted(components):
            print("  %-4d %-16s emitted by: %s" % (
                value, components[value],
                ", ".join(comp_files.get(components[value], [])) or "-"))
        print("reason codes (module-local - the same number means different things "
              "in different components):")
        for comp, value, rname, origin, note in order:
            print("  %-16s 0x%04x %-30s %s" % (comp, value, rname, origin))
        return 0

    if args.explain:
        token = args.explain
        hits = []
        if re.match(r"^(0x)?[0-9A-Fa-f]+$", token):
            want = int(token, 0)
            hits = [o for o in order if o[1] == want]
        else:
            hits = [o for o in order if o[2].upper().endswith(token.upper())]
        if not hits:
            sys.exit("no reason code matches %s" % token)
        for comp, value, rname, origin, note in hits:
            print("%-16s 0x%04x %-30s %-32s %s" % (comp, value, rname, origin, note[:64]))
        return 0

    if args.scratch:
        d = scratch_row(hexbytes(args.scratch))
        emit("scratch", d, scratch_text(d))
        if "last_record" in d and d["last_record"]["component"]:
            r = d["last_record"]
            rec = (r["component"], r["line"], r["reason"])
            emit("record", decoder.record(rec), decoder.text(rec, prefix="  scratch row: "))
        return 0

    bad_seen = {}

    def note_bad(ep, size, why, ascii_text):
        # The vendor flasher protocol answers with one short text line per page
        # ("GET5 .;", "unlock! ", "VALID CRC 0xC9 "), so the same kind of payload
        # recurs dozens of times and the varying character is noise.  Report one
        # line per (endpoint, size, reason) with a sample and a count.
        key = (ep, size, why)
        seen = bad_seen.get(key)
        if seen is None:
            bad_seen[key] = [1, ascii_text]
        else:
            seen[0] += 1

    did = False
    if args.log:
        did = True
        stats = decode_log(args.log, decoder, emit, set(args.ep or (1, 3)), note_bad)
        if not args.no_bad:
            for (ep, size, why), (count, sample) in sorted(bad_seen.items(),
                                                            key=lambda k: -k[1][0]):
                line = "  EP%d  %3d bytes x%-3d not a record stream: %-34s e.g. %r" % (
                    ep, size, count, why, sample)
                emit("bad", {"ep": ep, "bytes": size, "why": why, "ascii": sample,
                             "count": count}, (line, []))
        print("log: %d record(s), %d non-record payload(s), %d scratch row(s), "
              "%d payload(s) on endpoints not decoded (use --ep)"
              % (stats["records"], stats["bad"], stats["scratch"],
                 stats["skipped_ep"]))
    if args.file:
        did = True
        data = sys.stdin.buffer.read() if args.file == "-" else open(args.file, "rb").read()
        # A stream that IS a clean record stream must be decoded strictly. The
        # resynchronising path below is for captures that mix in the vendor
        # bootloader's text, and on a clean stream it LOST records: 880 bytes of 110
        # back-to-back records came back as 65 records plus "360 non-record bytes"
        # (measured 2026-10-21, and this is the decoder every analysis in this repo
        # goes through). dbg.h guarantees a drain is a
        # whole number of 8-byte records, so the test is exact: every frame carries the
        # magic, or the stream is mixed.
        clean = (len(data) % RECORD_BYTES == 0)
        if clean:
            for chunk in range(0, len(data), RECORD_BYTES):
                if int.from_bytes(data[chunk:chunk + 2], "little") != MAGIC:
                    clean = False
                    break
        if args.strict_framing or clean:
            nrec, nbad, nbytes = decode_stream(data, decoder, emit, args.no_bad)
        else:
            # A capture from a board is not a clean record stream: the vendor
            # bootloader talks on the same pipe.  Resynchronise with the same
            # rules the live watchers use (tools/cdcview.py) - one framing
            # implementation, not two that can disagree.  --strict-framing gives
            # the byte-exact contract back (dbg.h: every drain is a multiple of 8).
            import cdcview
            got = []

            def on_record(rec):
                got.append(rec)      # keep the raw tuple; decode it once, below

            view = cdcview.CdcView("stream", decoder=decoder, echo=False,
                                   on_record=on_record)
            lines = view.feed(data) + view.flush()
            nrec, nbad, nbytes = view.records, len(data) - view.records * RECORD_BYTES, len(data)
            if not args.quiet or args.json:
                for rec in got:
                    emit("record", decoder.record(rec), decoder.text(rec))
            if not args.no_bad:
                # cdcview already words these well ("stream text '...' (581 byte(s),
                # not records)"); re-wrapping them made the report worse.
                for line in lines:
                    if not args.json and not args.quiet:
                        print("  " + line)
        print("stream: %d byte(s): %d record(s), %d non-record byte(s) (%s)"
              % (nbytes, nrec, nbad,
                 "strict 8-byte framing" if args.strict_framing
                 else "resynchronised, same rules as tools/cdcview.py"))
    if args.port:
        did = True
        follow_port(args.port, args.baud, decoder, emit,
                    None if args.follow else args.limit or 1)

    if args.timeline:
        if not timeline:
            print("timeline: no DBG_COMP_TIME record in the input")
        else:
            print("bring-up stopwatch (cycles since Clock_BringupClockStart; "
                  "the pump credits at most one SysTick period per pump, so each "
                  "delta is a lower bound):")
            print("  %-14s %11s %11s  %9s %9s" %
                  ("milestone", "cycles", "stage cyc", "ms @48M", "ms @8M"))
            prev = 0
            stamps = {}
            for rname, cycles, at_least, frames, tick_ms in timeline:
                short = rname.replace("DBG_TIME_RSN_", "")
                if cycles is None:
                    n = frames if frames is not None else tick_ms
                    what = ("hardware frames (FNUM)" if short.endswith("FRAMES") else
                            "SOF flags our vector saw" if short.endswith("FLAGS") else
                            "SysTick ticks")
                    print("  %-14s %11s  %s %s" % (short, "-", n, what))
                    stamps[short] = ("count", n)
                    continue
                print("  %-14s %11d %11d  %9.2f %9.2f%s" %
                      (short, cycles, cycles - prev,
                       cycles / 48e6 * 1e3, cycles / 8e6 * 1e3,
                       "  (saturated)" if at_least else ""))
                prev = cycles
                stamps[short] = ("cycles", cycles)
            # The clock proof, derived: USBC advances FNUM once per 1 ms host
            # frame, so cycles per frame x 1000 is the CPU clock in Hz.
            if all(k in stamps for k in ("SOF_START", "SOF_END", "SOF_FRAMES")):
                d = stamps["SOF_END"][1] - stamps["SOF_START"][1]
                n = stamps["SOF_FRAMES"][1]
                flags = stamps.get("SOF_FLAGS", ("", None))[1]
                ms = stamps.get("TICK_MS", ("", None))[1]
                if ms is None:
                    ms = 0
                if n:
                    cpf = float(d) / n
                    print("\nclock proof: %d cycles across %d hardware frame(s) "
                          "(FNUM) = %.0f cycles/frame -> CPU = %.2f MHz  "
                          "[48 MHz would be 48 000, 8 MHz 8 000, 1.1 MHz 1 100]%s"
                          % (d, n, cpf, cpf / 1000.0,
                             "" if flags is None
                             else "  (our vector acknowledged %d of those frames)"
                                  % flags))
                    if ms is not None and ms:
                        print("  cross-check against SysTick ticks: %d tick(s) in "
                              "the window -> %d cycles/tick (LOAD+1 = 48 000 if the "
                              "stopwatch is accounting every tick) and %.2f frame(s) "
                              "per tick (%.1f would mean FNUM ticks per 1 ms frame "
                              "at 48 MHz)"
                              % (ms, d // ms, float(n) / ms, 1.0))
                else:
                    print("\nclock proof: SOF_END - SOF_START = %d cycles but FNUM "
                          "counted 0 frames - the SOF flags we counted were not "
                          "host frames" % d)
        return 0

    if not did:
        ap.error("nothing to do: give --file, --log, --port or --scratch")
    if args.strict and unknown:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
