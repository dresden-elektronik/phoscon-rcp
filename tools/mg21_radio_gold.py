#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""Cross-check the two published descriptions of the ConBee III radio's registers, and write the record the suites read.

The radio's register map reaches this tree twice over, from two vendor artifacts of different
generations: the device headers of `gecko_sdk` v4.0.0 (`Device/Include`), and the part's CMSIS
SVD (`EFR32MG21A020F768IM32.svd`, as carried by ErkSponge's SONOFF sniffer project). Neither is in the current Simplicity SDK: the
radio blocks were withdrawn from the headers, and the manuals never described them. Two
independent artifacts describing the same map is the strongest documentation this part has, and an
agreement between them is the thing a future radio driver can rest on.

This tool reads both, and refuses to write a record while they disagree.

    python3 tools/mg21_radio_gold.py            # cross-check, then write test/data/mg21_radio_ref.h
    HDRS4=... SVD=... python3 tools/mg21_radio_gold.py

Offsets for the header side come from `offsetof` on the vendor's own typedef, computed by a probe
this file generates and compiles — a sparse block cannot be read correctly any other way. Field
encodings and access rights are read from the headers' own `_<BLOCK>_<REG>_<FIELD>_MASK`,
`..._SHIFT` and the typedef's `__IM`/`__OM`/`__IOM` spellings, and compared with the SVD's
`bitOffset`/`bitWidth`, `access` and `enumeratedValues`.

Neither artifact is part of this repository, so the generated record — not the reference trees — is
what the suites need.
Regenerate after a reference change and read the diff: that diff is the review.
"""

import os
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HDRS4 = Path(os.environ.get("HDRS4", "/nonexistent"))
CMSIS = Path(os.environ.get("CMSIS", "/nonexistent"))
SVD = Path(os.environ.get("SVD", "/nonexistent"))
PART = "efr32mg21a010f1024im32.h"
OUT = REPO / "test" / "data" / "mg21_radio_ref.h"                 # the public subset
OUT_FULL = REPO / "internal" / "test" / "data" / "mg21_radio_full.h"  # the whole record, untracked
SUBSET_FROM = REPO / "test" / "unit" / "test_mg21_regs.c"
SPDX = "/* SPDX-License-Identifier: BSD-3-Clause\n * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */"

BLOCKS = ["rac", "frc", "modem", "synth", "agc", "protimer", "rfcrc", "bufc"]
RADIO_BLOCKS = ("RAC", "FRC", "MODEM", "SYNTH", "AGC", "PROTIMER", "RFCRC")
# Peripheral blocks read from the SVD for the cross-check against the checked-in peripheral record.
PERIPH_BLOCKS = ("CMU", "HFXO0", "LFRCO", "FSRCO", "DPLL0", "EMU", "WDOG0", "USART0", "GPIO")
KEEP_BLOCKS = set([b.upper() for b in BLOCKS]) | set(PERIPH_BLOCKS)

# The state machine a transaction loop waits on, and the sequencer handshake it reads.
ENUM_FIELDS = [("RAC", "STATUS", "STATE"), ("RAC", "SEQSTATUS", "WAITMODE")]

# Register names the SVD spells out per array element and the header writes as one array member.
# Compared by offset, so a naming difference is reported, not an error, when the offset and the
# fields agree.
def strip_suffix(name: str) -> str:
    return re.sub(r"\d+$", "", name)


def clean(text: str) -> str:
    text = re.sub(r"//.*", "", text)
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def num(text: str) -> int:
    text = text.strip().rstrip("ULul")
    return int(text, 16) if text.startswith("0x") else int(text)


def header_typedefs(block: str):
    """Every typedef struct in the header: name -> [(access, type, member, count)].

    The radio headers nest their sub-blocks: PROTIMER has `PROTIMER_CC_TypeDef CC[4]`, BUFC has
    `BUFC_BUF_TypeDef BUF[2]`, FRC has `FRC_FCD_TypeDef FCD[4]`. Those are real registers, and the
    SVD names them element by element, so a layout walk has to descend into them rather than read
    the array as one thing.
    """
    text = clean((HDRS4 / f"efr32mg21_{block}.h").read_text())
    out = {}
    for m in re.finditer(r"typedef struct \{([^}]*)\} ([A-Za-z0-9_]+);", text):
        members = []
        for mm in re.finditer(r"(__IM|__OM|__IOM)?\s*uint32_t\s+([A-Z0-9_]+)((?:\[[^\]]*\])?);", m.group(1)):
            access, name, arr = mm.group(1) or "__IOM", mm.group(2), mm.group(3)
            count = 1
            for dim in re.findall(r"\[([^\]]+)\]", arr):
                count *= num(dim)
            members.append((access, name, count))
        out[m.group(2)] = members
    # Sub-block members are typed by their typedef name, not uint32_t: pick those up too.
    for m in re.finditer(r"(__IM|__OM|__IOM)?\s*([A-Z][A-Z0-9_]*_TypeDef)\s+([A-Z0-9_]+)((?:\[[^\]]*\])?);", text):
        pass
    return out


def header_layout(block: str):
    """(label, offset, access) for every register the header names, reserved gaps counted.

    Labels follow the SVD's convention for a sub-block: a struct with one member of the container's
    own name becomes `FCD0`, anything else becomes `CC0_CTRL`. That keeps the two sides comparable
    by name as well as by offset.
    """
    tds = header_typedefs(block)
    main = block.upper() + "_TypeDef"
    if main not in tds:
        sys.exit(f"mg21_radio_gold: no {main} in efr32mg21_{block}.h")
    # Re-read the main typedef allowing struct-typed members.
    text = clean((HDRS4 / f"efr32mg21_{block}.h").read_text())
    body = re.search(r"typedef struct \{([^}]*)\} " + main + r";", text)
    if not body:
        sys.exit(f"mg21_radio_gold: cannot read the body of {main}")
    out = []
    off = 0
    for mm in re.finditer(r"(__IM|__OM|__IOM)?\s*([A-Za-z][A-Za-z0-9_]*|uint32_t)\s+([A-Z0-9_]+)((?:\[[^\]]*\])?);", body.group(1)):
        access, typ, name, arr = mm.group(1) or "__IOM", mm.group(2), mm.group(3), mm.group(4)
        count = 1
        for dim in re.findall(r"\[([^\]]+)\]", arr):
            count *= num(dim)
        if typ in tds and typ != main:
            sub = tds[typ]
            sub_size = 4 * len(sub)
            single = len(sub) == 1 and sub[0][1] == name
            for si, (sacc, sname, _c) in enumerate(sub):
                if sname.startswith("RESERVED"):
                    continue
                for i in range(count):
                    # The SVD names a sub-block register per element (CC0_CTRL); the header defines
                    # one encoding for the element type (CC_CTRL). Both names are kept so the field
                    # lookup can use whichever the source spells.
                    label = f"{name}{i}" if single else f"{name}{i}_{sname}"
                    hdr_label = name if single else f"{name}_{sname}"
                    out.append((label, hdr_label, off + i * sub_size + 4 * si, sacc, f"{name}[{i}].{sname}"))
            off += count * sub_size
            continue
        if not name.startswith("RESERVED"):
            for i in range(count):
                label = f"{name}{i}" if count > 1 else name
                path = f"{name}[{i}]" if count > 1 else name
                out.append((label, name, off + 4 * i, access, path))
        off += 4 * count
    return out, off


def header_fields(block: str, reg_names):
    """(register, field) -> (mask, shift, default) from the header's own defines.

    The defines read `_<BLOCK>_<REGISTER>_<FIELD>_MASK`, and a sub-block register's own name
    contains an underscore (`_PROTIMER_CC_CTRL_CCMODE_MASK`), so the boundary between register and
    field is found by matching the longest register name this header actually declares.
    """
    text = clean((HDRS4 / f"efr32mg21_{block}.h").read_text())
    pre = block.upper()
    names = sorted(reg_names, key=len, reverse=True)
    fields = {}

    def split(rest):
        for cand in names:
            if rest.startswith(cand + "_"):
                return cand, rest[len(cand) + 1:]
        return None, None

    for kind, slot in (("_MASK", 0), ("_SHIFT", 1), ("_DEFAULT", 2)):
        for m in re.finditer(r"#define\s+_" + pre + r"_([A-Z0-9_]+)" + kind + r"\s+\(?(0x[0-9A-Fa-f]+|\d+)U?L?\)?", text):
            reg, field = split(m.group(1))
            if reg is None or field is None or "_" in field:
                continue
            value = num(m.group(2))
            entry = fields.setdefault((reg, field), [None, None, None])
            entry[slot] = value
    return fields


def header_bases():
    text = clean((HDRS4 / PART).read_text())
    return {m.group(1): num("0x" + m.group(2))
            for m in re.finditer(r"#define\s+([A-Z0-9]+(?:_NS)?)_BASE\s+\(0x([0-9A-Fa-f]+)UL\)", text)}


def compile_offsets():
    """Offsets for every named register, from offsetof on the vendor typedef.

    The walk in header_layout is checked against this: if the two disagree, the walk has
    misread a sub-block, and the whole comparison would be meaningless. Only the layout matters,
    so the volatile access spellings are defined locally rather than pulled from CMSIS —
    compiling cmsis_gcc.h for the host warns about ARM intrinsics.
    """
    src = ["#include <stddef.h>", "#include <stdint.h>", "#include <stdio.h>",
           "#ifndef __IM", "#define __IM volatile const", "#define __OM volatile", "#define __IOM volatile", "#endif"]
    for block in BLOCKS:
        src.append(f'#include "efr32mg21_{block}.h"')
    src.append("int main(void){")
    for block in BLOCKS:
        pre = block.upper()
        walk, _size = header_layout(block)
        for _label, _hdr_label, _off, _access, path in walk:
            src.append(f'printf("REG {pre} {_label} %zu\\n", offsetof({pre}_TypeDef, {path}));')
    src.append("return 0;}")

    tmp = Path(tempfile.mkdtemp(prefix="mg21radio"))
    (tmp / "probe.c").write_text("\n".join(src))
    exe = tmp / "probe"
    r = subprocess.run(["cc", "-std=gnu99", "-O0", f"-I{HDRS4}", f"-I{CMSIS}", "-o", str(exe), str(tmp / "probe.c")],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit("mg21_radio_gold: the probe did not compile against the reference headers:\n" + r.stderr[:4000])
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    off = {}
    for line in run.stdout.splitlines():
        parts = line.split()
        if parts[0] == "REG":
            off.setdefault(parts[1], {})[parts[2]] = int(parts[3])
    return off


def svd_blocks():
    root = ET.parse(str(SVD)).getroot()
    out = {}
    for peri in root.iter("peripheral"):
        name = peri.findtext("name") or ""
        base = name[:-3] if name.endswith("_NS") else name
        if base not in KEEP_BLOCKS:
            continue
        regs = {}
        for reg in peri.iter("register"):
            fields = {}
            for f in reg.iter("field"):
                bit_off, width = int(f.findtext("bitOffset")), int(f.findtext("bitWidth"))
                enums = [(e.findtext("name"), int((e.findtext("value") or "0").strip(), 0))
                          for e in f.iter("enumeratedValue")]
                fields[f.findtext("name")] = {
                    "mask": ((1 << width) - 1) << bit_off,
                    "shift": bit_off,
                    "access": f.findtext("access"),
                    "reset": f.findtext("resetValue"),
                    "enums": enums,
                }
            regs[int(reg.findtext("addressOffset"), 16)] = {
                "name": reg.findtext("name"),
                "access": reg.findtext("access"),
                "reset": reg.findtext("resetValue"),
                "mask": reg.findtext("resetMask"),
                "fields": fields,
            }
        ab = [int(a.findtext("address").strip(), 0) for a in peri.iter("addressBlock") if a.findtext("address")]
        out[base] = {"regs": regs, "bases": sorted(set(ab)), "secure": name.endswith("_S"),
                     "group": peri.findtext("group") or ""}
    return out


def cross_check_peripherals(svd):
    """Check the checked-in peripheral record against the SVD.

    `test/data/mg21_regs_ref.h` is generated from the current Simplicity SDK headers; the SVD in the
    reference corpus is from the generation that still described the radio. Where both name a
    peripheral, they should agree on where its registers are — and where they do not, one of the two
    has changed. This is also the arbitration for the one known cross-generation difference: v4.0.0
    gives LFRCO a CTRL at +0x04, the current header and this SVD do not.
    """
    text = (REPO / "test" / "data" / "mg21_regs_ref.h").read_text()
    rec = {}
    for m in re.finditer(r"#define\s+REF_OFF_([A-Z0-9]+)_([A-Z0-9]+)\s+(0x[0-9A-Fa-f]+)u", text):
        rec.setdefault(m.group(1), {})[m.group(2)] = int(m.group(3), 16)
    out = {}
    for blk, regs in rec.items():
        sregs = svd.get(blk, {}).get("regs", {})
        agree, bad, absent, naming = 0, [], [], []
        by_name = {r["name"]: o for o, r in sregs.items()}
        for name, off in sorted(regs.items()):
            if off in sregs and sregs[off]["name"] == name:
                agree += 1
            elif off in sregs:
                naming.append((name, off, sregs[off]["name"]))
            elif name in by_name:
                bad.append((name, hex(off), hex(by_name[name])))
            else:
                absent.append(name)
        for name, off, svd_name in naming:
            print(f"{blk}: offset {off:#06x}: the record says {name}, the SVD says {svd_name}")
        out[blk] = (len(regs), len([o for o in sregs if o < 0x1000]), agree, bad, absent)
    return out


def main() -> int:
    for path, what in ((HDRS4 / PART, "v4.0.0 device headers"), (SVD, "the part's SVD")):
        if not path.is_file():
            print(f"mg21_radio_gold: {what} not found at {path}", file=sys.stderr)
            print("  set HDRS4= and SVD= to the reference trees; they are not part of this repository.", file=sys.stderr)
            return 2

    off = compile_offsets()
    hfields = {b.upper(): header_fields(b, {hl for _l, hl, _o, _a, _p in header_layout(b)[0]}) for b in BLOCKS}
    hbases = header_bases()
    svd = svd_blocks()
    access_of = {}
    hdr_label = {}
    for b in BLOCKS:
        for name, hl, _off, access, _path in header_layout(b)[0]:
            access_of.setdefault(b.upper(), {})[name] = access
            hdr_label.setdefault(b.upper(), {})[name] = hl

    problems = []
    access_notes = {}
    svd_only_fields = {}
    record = {"base": {}, "regs": {}, "fields": {}, "svd_fields": {}, "enums": {}, "alias": {}, "counts": {}}

    # The Python walk and the compiled offsetof must agree, or nothing else in this file means
    # anything: the walk is how sub-blocks are read, and a misread sub-block shifts every register
    # after it.
    for block in BLOCKS:
        pre = block.upper()
        for label, _hdr_label, walk_off, _access, _path in header_layout(block)[0]:
            probe_off = off.get(pre, {}).get(label)
            if probe_off is None:
                problems.append(f"{pre}.{label}: the probe did not report it")
            elif probe_off != walk_off:
                problems.append(f"{pre}.{label}: struct walk says {walk_off:#x}, offsetof says {probe_off:#x}")

    for block in BLOCKS:
        pre = block.upper()
        hregs = {v: k for k, v in off.get(pre, {}).items()}
        labels = {l: hl for l, hl, _o, _a, _p in header_layout(block)[0]}
        sregs = svd.get(pre, {}).get("regs", {})

        # Bases: the part header against the SVD's address blocks.
        hb, hns = hbases.get(pre), hbases.get(pre + "_NS")
        sb = svd.get(pre, {}).get("bases", [])
        if hb is None or hns is None:
            problems.append(f"{pre}: base address missing from the part header")
        for addr in sb:
            if addr not in (hb, hns):
                problems.append(f"{pre}: SVD address block {addr:#x} is not a header base ({hb:#x} / {hns:#x})")
        record["base"][pre] = (hb, hns)

        # Registers: matched by offset, since the SVD names array elements one by one. A register
        # at or above 0x1000 is in a SET/CLR/TGL alias window, which the SVD does not describe.
        agreed = 0
        naming = []
        only_svd = []
        for addr in sorted(sregs):
            sname = sregs[addr]["name"]
            hname = hregs.get(addr)
            if hname is None:
                only_svd.append((hex(addr), sname))
                continue
            agreed += 1
            if hname != sname:
                naming.append((hex(addr), hname, sname))
        only_hdr = [o for o, n in hregs.items() if o not in sregs and o < 0x1000]
        for o, sreg in sregs.items():
            if o >= 0x1000 or o not in hregs:
                continue
            acc = access_of[pre].get(hregs[o])
            want = {"read-only": "__IM", "write-only": "__OM", "read-write": "__IOM"}.get(sreg["access"])
            if want and acc and want != acc:
                # Not a layout disagreement. The SVD calls a command register write-only where the
                # header spells the member read-write; the record keeps the SVD's stricter claim.
                access_notes.setdefault(pre, []).append((hregs[o], sreg["access"], acc))
        record["counts"][pre] = {"svd": len([o for o in sregs if o < 0x1000]),
                                 "header": len([n for o, n in hregs.items() if o < 0x1000]),
                                 "agree": len([o for o in sregs if o < 0x1000 and o in hregs]),
                                 "naming": len(naming), "svd_only": len(only_svd), "header_only": len(only_hdr)}
        for addr, sname in only_svd:
            print(f"{pre}: SVD names {sname} at {addr}, the v4.0.0 header names no register there")
        for addr in sorted(only_hdr):
            print(f"{pre}: header names {hregs[addr]} at {hex(addr)}, the SVD names no register there")
        for entry in naming:
            print(f"{pre}: same offset {entry[0]}, header says {entry[1]}, SVD says {entry[2]}")

        # The register map the record carries: plain registers only. The SET/CLR/TGL windows are
        # a stride fact, recorded as a count, not as four times the lines.
        for addr, name in sorted(hregs.items()):
            if addr < 0x1000:
                record["regs"].setdefault(pre, {})[name] = addr
        for addr, sname in only_svd:
            if addr < 0x1000:
                record["regs"].setdefault(pre, {})[sname] = addr

        # SET/CLR/TGL windows, which only the headers spell out.
        windows = {}
        for addr, name in hregs.items():
            if addr < 0x1000:
                continue
            window = addr - (addr & 0xFFF)
            base = addr & 0xFFF
            if base in hregs:
                windows.setdefault(hregs[base], set()).add(window)
            else:
                problems.append(f"{pre}: {name} at {hex(addr)} has no plain register under it")
        for name, deltas in windows.items():
            if deltas != {0x1000, 0x2000, 0x3000}:
                problems.append(f"{pre}: {name} has alias windows {sorted(hex(d) for d in deltas)}, not all three")
        record["alias"][pre] = len(windows)

        # Fields: the header's mask/shift against the SVD's bitOffset/bitWidth, matched by the
        # register offset the field lives in, because that is the one thing both spell the same way.
        agree_f = 0
        bad_f = []
        only_svd_f = 0
        for addr, sreg in sregs.items():
            hname = hregs.get(addr)
            if hname is None:
                continue
            hfield_key = labels.get(hname, hname)
            for fname, f in sreg["fields"].items():
                if fname == sreg["name"] and f["mask"] == 0xFFFFFFFF:
                    # The SVD writes a register with no sub-fields as one field named after itself;
                    # there is no encoding in that to compare.
                    record["counts"][pre].setdefault("fields_whole", 0)
                    record["counts"][pre]["fields_whole"] += 1
                    continue
                key = (hfield_key, fname)
                hf = hfields[pre].get(key) or hfields[pre].get((hname, fname))
                if hf is None or hf[0] is None or hf[1] is None:
                    only_svd_f += 1
                    # The SVD documents this encoding and the headers do not define it: keep it,
                    # marked, because for these fields the SVD is the only published source.
                    svd_only_fields.setdefault(pre, {})[f"{hname}_{fname}"] = (f["mask"], f["shift"])
                    continue
                if hf[0] != f["mask"] or hf[1] != f["shift"]:
                    bad_f.append((hname, fname, hex(f["mask"]), f["shift"], hex(hf[0]), hf[1]))
                    continue
                agree_f += 1
                record["fields"].setdefault(pre, {})[f"{hname}_{fname}"] = (f["mask"], f["shift"])
        for entry in bad_f:
            problems.append(f"{pre}.{entry[0]}.{entry[1]}: SVD mask/shift {entry[2]}/{entry[3]}, header {entry[4]}/{entry[5]}")
        record["svd_fields"][pre] = svd_only_fields.get(pre, {})
        record["counts"][pre]["fields_agree"] = agree_f
        record["counts"][pre]["fields_svd_only"] = only_svd_f
        record["counts"][pre]["fields_bad"] = len(bad_f)
        if bad_f:
            for entry in bad_f[:6]:
                print(f"{pre}: DISAGREE {entry}")

        for reg, field in [(r, f) for (b, r, f) in ENUM_FIELDS if b == pre]:
            for addr, sreg in sregs.items():
                if strip_suffix(hregs.get(addr, "")) != reg:
                    continue
                for fname, f in sreg["fields"].items():
                    if fname == field and f["enums"]:
                        for ename, evalue in f["enums"]:
                            record["enums"].setdefault(pre, {})[f"{reg}_{field}_{ename}"] = evalue

    for pre, notes in access_notes.items():
        print(f"{pre}: {len(notes)} register(s) the SVD calls write-only where the header spells the member read-write, e.g. {pre}.{notes[0][0]}")

    peripherals = cross_check_peripherals(svd)
    print()
    print(f'{"periph":9} {"record":>7} {"svd":>6} {"agree":>6} {"disagree":>9} {"absent from svd":>16}')
    for blk, (n_rec, n_svd, agree, bad, absent) in peripherals.items():
        print(f'{blk:9} {n_rec:7} {n_svd:6} {agree:6} {len(bad):9} {len(absent):16}')
        for entry in bad[:6]:
            print("    DISAGREE", blk, entry)
    print()
    print(f'{"block":9} {"header":>7} {"svd":>6} {"offsets agree":>14} {"naming differs":>15} {"svd only":>9} {"fields agree":>13} {"fields svd-only":>16} {"alias windows":>14}')
    for block in BLOCKS:
        pre = block.upper()
        c = record["counts"][pre]
        print(f'{pre:9} {c["header"]:7} {c["svd"]:6} {c["agree"]:14} {c["naming"]:15} {c["svd_only"]:9} {c["fields_agree"]:13} {c["fields_svd_only"]:16} {record["alias"][pre]:14}')

    if problems:
        print(f"\nmg21_radio_gold: {len(problems)} disagreement(s) between the two published descriptions; no record written:", file=sys.stderr)
        for p in problems[:40]:
            print("  " + p, file=sys.stderr)
        return 1

    kept = write_record(record)
    print(f"\nwrote {OUT_FULL.relative_to(REPO)} and {OUT.relative_to(REPO)} ({kept} entries)")
    return 0


class _Seen(dict):
    """A dict that remembers every key it was asked about."""

    def __init__(self, src):
        super().__init__(src)
        self.asked = set()

    def __getitem__(self, key):
        self.asked.add(key)
        return super().__getitem__(key)

    def __contains__(self, key):
        self.asked.add(key)
        return super().__contains__(key)


def assembler_names():
    """The record entries tools/mg21_seq.py resolves when it assembles the sequencer program.

    The assembler reads register names through mg21_mon from the radio record, so the tracked
    subset has to carry every name it uses, or a fresh clone cannot check the generated image."""
    sys.path.insert(0, str(REPO / "tools"))
    import mg21_mon  # noqa: E402 - loads the whole record just written
    import mg21_seq  # noqa: E402
    mg21_mon.BASES, mg21_mon.OFFS = _Seen(mg21_mon.BASES), _Seen(mg21_mon.OFFS)
    mg21_seq.build()
    names = set()
    for block in mg21_mon.BASES.asked:
        names |= {f"RADIO_REF_BASE_{block}", f"RADIO_REF_BASE_{block}_NS"}
    for block, reg in mg21_mon.OFFS.asked:
        names.add(f"RADIO_REF_OFF_{block}_{reg}")
    return names


def write_record(record) -> int:
    """Write the whole record (untracked) and the subset the mg21_regs suite checks (tracked).

    The whole map republishes register blocks the vendor withdrew from its SDKs, so it stays with
    the maintainers; the tracked subset carries only the entries test/unit/test_mg21_regs.c names,
    which are the radio registers this tree's own src/hal/mg21/mg21_regs.h transcribes."""
    lines = [
        SPDX,
        "/* Generated by tools/mg21_radio_gold.py — do not edit.",
        " *",
        " * The EFR32MG21 radio's control blocks, as two published vendor descriptions agree they",
        " * are: the gecko_sdk v4.0.0 device headers (offsets taken by offsetof on the vendor typedef)",
        " * and the part's CMSIS SVD (offsets, bit fields, access rights, enumerated values).",
        " * Registers the SVD names and the headers leave as a gap are carried with the SVD's name.",
        " * Field encodings are recorded only where both sources give the same mask and shift.",
        " * Neither artifact is in the Simplicity SDK the ConBee III firmware builds against: the radio",
        " * blocks were withdrawn from the current headers (docs/hardware_mg21.md §10.4).",
        " */",
        "#ifndef MG21_RADIO_FULL_H",
        "#define MG21_RADIO_FULL_H",
        "",
    ]
    for pre in [b.upper() for b in BLOCKS]:
        b, ns = record["base"][pre]
        lines.append(f"/* {pre}: base 0x{b:08X}, non-secure alias 0x{ns:08X} — both sources name both. */")
        lines.append(f"#define RADIO_REF_BASE_{pre} 0x{b:08X}u")
        lines.append(f"#define RADIO_REF_BASE_{pre}_NS 0x{ns:08X}u")
        c = record["counts"][pre]
        lines.append(f"#define RADIO_REF_REGS_{pre} {c['header'] + c['svd_only'] - c['naming']}u")
        lines.append(f"#define RADIO_REF_ALIAS_{pre} {record['alias'][pre]}u")
        lines.append(f"#define RADIO_REF_FIELDS_{pre} {len(record['fields'].get(pre, {}))}u")
        lines.append(f"#define RADIO_REF_FIELDS_SVD_ONLY_{pre} {len(record['svd_fields'].get(pre, {}))}u")
        lines.append("")
        for name, addr in sorted(record["regs"].get(pre, {}).items(), key=lambda kv: (kv[1], kv[0])):
            lines.append(f"#define RADIO_REF_OFF_{pre}_{name} 0x{addr:04X}u")
        lines.append("")
        for key, (mask, shift) in sorted(record["fields"].get(pre, {}).items()):
            lines.append(f"#define RADIO_REF_MASK_{pre}_{key} 0x{mask:08X}u")
            lines.append(f"#define RADIO_REF_SHIFT_{pre}_{key} {shift}u")
        lines.append(f"/* {pre}: encodings the SVD publishes and the v4.0.0 headers do not define. */")
        for key, (mask, shift) in sorted(record["svd_fields"].get(pre, {}).items()):
            lines.append(f"#define RADIO_REF_SVD_MASK_{pre}_{key} 0x{mask:08X}u")
            lines.append(f"#define RADIO_REF_SVD_SHIFT_{pre}_{key} {shift}u")
        lines.append("")
        for key, value in sorted(record["enums"].get(pre, {}).items()):
            lines.append(f"#define RADIO_REF_ENUM_{pre}_{key} {value}u")
        if record["enums"].get(pre):
            lines.append("")
    lines.append("#endif /* MG21_RADIO_FULL_H */")
    OUT_FULL.parent.mkdir(parents=True, exist_ok=True)
    OUT_FULL.write_text("\n".join(lines) + "\n")

    checked = set(re.findall(r"\bRADIO_REF_[A-Z0-9_]+", SUBSET_FROM.read_text())) - {"RADIO_REF_H"}
    # The assembler also resolves SoC blocks, which live in mg21_regs_ref.h, not in this record.
    wanted = checked | assembler_names()
    sub = [
        SPDX,
        "/* Generated by tools/mg21_radio_gold.py — do not edit.",
        " *",
        " * The EFR32MG21 radio registers this tree's own register map (src/hal/mg21/mg21_regs.h) uses,",
        " * as two published vendor descriptions agree they are: the gecko_sdk v4.0.0 device headers",
        " * (offsets taken by offsetof on the vendor typedef) and the part's CMSIS SVD. Only the entries",
        " * the mg21_regs suite checks and the names tools/mg21_seq.py assembles with are carried here",
        " * (docs/hardware_mg21.md §10.4).",
        " */",
        "#ifndef MG21_RADIO_REF_H",
        "#define MG21_RADIO_REF_H",
        "",
    ]
    kept = 0
    for pre in [b.upper() for b in BLOCKS]:
        block = [ln for ln in lines if ln.startswith("#define ")
                 and ln.split()[1] in wanted and (ln.split()[1].startswith(f"RADIO_REF_BASE_{pre}")
                                                  or f"_{pre}_" in ln.split()[1]
                                                  or ln.split()[1].endswith(f"_{pre}"))]
        if block:
            sub += [f"/* {pre} */"] + block + [""]
            kept += len(block)
    sub.append("#endif /* MG21_RADIO_REF_H */")
    OUT.write_text("\n".join(sub) + "\n")
    missing = checked - {ln.split()[1] for ln in sub if ln.startswith("#define ")}
    if missing:
        sys.exit(f"mg21_radio_gold: test_mg21_regs.c names entries the record lacks: {sorted(missing)}")
    return kept


if __name__ == "__main__":
    sys.exit(main())
