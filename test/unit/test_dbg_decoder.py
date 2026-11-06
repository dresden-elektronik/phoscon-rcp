#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""test_dbg_decoder.py — the witness field tables in tools/dbg.py, checked against the decoder.

The debug channel is the ConBee III's only witness, and `tools/dbg.py` decodes each witness
record's 16-bit `line` field into the part's register sub-fields through `(value & mask) >> shift`
(dbg.py `mg21WitnessFields`). Two failure modes are silent and board-invisible: a mask whose set
bits all sit *below* its shift decodes a constant 0 no matter what the register held (a field that
lies "clear" forever), and a label dictionary that names a value the field cannot hold. Neither is
reported anywhere, so the table is checked structurally: every field must be decodable, and every
label must be in the field's value range. A set of pinned raw values is decoded to their expected
text so a re-anchored mask or a relabelled value also turns red.

The invariant was written down because two fields shipped broken this way (`USART_STATUS.TXIDLE`
mask `0x1`/shift 13 and `USART_IF.FERR` mask `0x1`/shift 9 both decoded a constant 0).
"""
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "tools"))

import dbg  # noqa: E402

failures = 0


def check(cond, desc, detail=""):
    global failures
    if cond:
        print(f"  ok    {desc}")
    else:
        failures += 1
        print(f"  FAIL  {desc}" + (f"  [{detail}]" if detail else ""))


def main():
    fields = dbg.MG21_WITNESS_FIELDS
    check(len(fields) >= 20, f"witness table is populated ({len(fields)} reasons)",
          f"{len(fields)}")

    # Every field must decode a non-constant value: its mask must have at least one set bit at or
    # above its shift, else `(value & mask) >> shift` is 0 for every value the register can hold.
    bad_mask = []
    for reason, fl in sorted(fields.items()):
        for name, mask, shift, labels in fl:
            if mask == 0:
                bad_mask.append(f"{reason}.{name}: mask 0")
            elif (mask >> shift) == 0:
                bad_mask.append(f"{reason}.{name}: mask 0x{mask:X} below shift {shift} -> always 0")
    check(not bad_mask, "every field's mask reaches its shift (none decode a constant 0)",
          "; ".join(bad_mask))

    # Every label must name a value the field can actually hold.
    bad_label = []
    for reason, fl in sorted(fields.items()):
        for name, mask, shift, labels in fl:
            if labels is None:
                continue
            width = (mask >> shift).bit_length()
            for k in labels:
                if not (0 <= k < (1 << width)):
                    bad_label.append(f"{reason}.{name}: label {k} outside {width}-bit field")
    check(not bad_label, "every label names a value its field can hold", "; ".join(bad_label))

    # Pinned decodes: raw register half -> expected printed field. These are the part's own bits.
    cases = [
        ("DBG_WITNESS_RSN_MG21_USART_STATUS", 0x2000, "TXIDLE=1"),
        ("DBG_WITNESS_RSN_MG21_USART_IEN", 0x200, "FERR=1"),
        ("DBG_WITNESS_RSN_MG21_SYSCLKCTRL", 0x3000, "HCLKPRESC=/4"),
        ("DBG_WITNESS_RSN_MG21_SYSCLKCTRL", 0x400, "PCLKPRESC=/2"),
        ("DBG_WITNESS_RSN_MG21_SYSCLKCTRL", 0x3003, "HCLKPRESC=/4"),
        ("DBG_WITNESS_RSN_MG21_USART_CTRL", 0x20, "OVS=X8"),
        ("DBG_WITNESS_RSN_MG21_WDOG_EN", 0x1, "EN=1"),
    ]
    for reason, value, want in cases:
        got = dbg.mg21WitnessFields(reason, value)
        check(got is not None and want in got,
              f"{reason} 0x{value:04X} prints {want!r}",
              f"got {got!r}")

    if failures:
        print(f"dbg_decoder: {failures} failure(s)")
        return 1
    print(f"dbg_decoder: {len(fields)} witness reasons, every field decodes, every label in range")
    return 0


if __name__ == "__main__":
    sys.exit(main())
