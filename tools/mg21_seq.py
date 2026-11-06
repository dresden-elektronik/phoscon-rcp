#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""The ConBee III radio sequencer program, and the assembler it is written with.

The EFR32MG21's radio controller (RAC) runs a small sequencer from SEQRAM; on every radio state
transition it enters that state's vector. No published source documents the sequencer's
instruction set: the encodings below were settled on the bench with single-step traces and test
programs written here, and this file is written against them only. It is this tree's own program -
no vendor sequence image is read, copied or linked (docs/hardware_mg21.md §10.3).

The machine: R7 is the program counter (a byte address in the secure alias 0xA0000000, 16-bit
halfword-aligned instructions), R6 a stack pointer growing upward, R0..R5 working registers, flags
NEG/POS/ZERO/CARRY in RAC.SEQSTATUS. Word n at VECTADDR + 4n (n = 0..13, one per RAC.STATUS.STATE)
is entered on a transition into state n. F800 ends a handler and sleeps until the next transition.
A wait tick is (RAC.PRESC + 1) = 8 crystal cycles, 208 ns.

Encodings ([15:11] operation, [10:8] target Rn, [7:0] operand; s a source register, i immediate):

    0000 11ii iiiiiiii     wait i ticks (STIMERCOMP := STIMER + i, WAITMASK := STCMP)
    0001 0nnn 0sss 0100    Rn := mem[Rs]                 0001 0nnn 0sss 1000   mem[Rn] := Rs
    0010 1nnn 0sss xxx1    Rn += Rs (bit 0 clear: flags only)
    0011 0nnn iiiiiiii     Rn &= i                       0011 1nnn xsss xxxx   Rn &= Rs
    0100 0nnn xsss xxx1    Rn |= Rs                      0100 1nnn iiiiiiii    Rn ^= i
    0101 0nnn 0sss xxx1    Rn ^= Rs (bit 0 clear: flags only)
    0101 1nnn 00000001     Rn := -Rn
    0110 Lnnn SxVa aaaa    shift (S 1) or rotate (S 0), left (L 1) or right, by a or by R[aaa] (V 1)
    0111 0ccc oooooooo     branch by the signed byte o if c: 0 always, 1 Z, 2 !Z, 3 C, 4 !C, 5 N, 6 !N
    0111 1nnn WISb bbbb    set (S 1) or clear bit b of mem[Rn] (I 0) or RAC word Rn (I 1); W 1 open
    1000 1000 0sss 0000    push Rs                       1001 0nnn 00000000    pop Rn
    9802 hhhh              jump to byte offset hhhh      9806 hhhh             call
    A000                   return
    1010 1nnn iiiiiiii     RAC word i := Rn              1011 0nnn iiiiiiii    Rn := RAC word i
    1011 1nnn iiiiiiii     Rn := i                       1100 xnnn llll hhhh   Rn := hhhh:llll
    1101 1nnn iiiiiiii     Z := (Rn & i) == 0
    EC mm                  WAITMASK := m (0x20 SYNTHRDY, 0x01 STCMP)
    F800                   end of handler

RAC word i is the RAC register at byte offset 4i. Open: condition 7, the 1101 0 form, the W 1 bit
form, and encodings no trace has exercised.

The image is fourteen vector words (word n = "9802 hhhh", a jump to the handler of RAC state n),
then the handlers. RXWARM brings the receiver up - the synthesiser enables in the stock order and
with the stock delays, the VCO search, SYNTHSTART, the LNA and the RX path - and RXPD takes it down
again. Every other state ends at once (F800).

    python3 tools/mg21_seq.py              # print the listing
    python3 tools/mg21_seq.py --words      # print the image words

tools/mg21_radio_gen.py turns the image into src/hal/mg21/own/mg21_seq_image.c.
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mg21_mon  # noqa: E402  (register names from the tracked gold records)

STATES = ["OFF", "RXWARM", "RXSEARCH", "RXFRAME", "RXPD", "RX2RX", "RXOVERFLOW", "RX2TX", "TXWARM",
          "TX", "TXPD", "TX2RX", "TX2TX", "SHUTDOWN"]
COND = {"": 0, "z": 1, "nz": 2, "c": 3, "nc": 4, "n": 5, "nn": 6}
HANDLERS = 0x40  # byte offset of the first handler, after the vector words
SEQRAM = 0xB0000000
SEQRAM_SIZE = 0x2000
STACK = 0x1F88  # R6, the call stack, grows upwards from here (the program makes no calls today)


def rac(name):
    """RAC word index of a register name, the operand of the RAC-word instructions."""
    return mg21_mon.OFFS[("RAC", name)] // 4


def addr(name):
    """Absolute address of BLOCK.REGISTER as the sequencer reaches it (the non-secure radio alias)."""
    return mg21_mon.resolve(name)


class Asm:
    """Emits 16-bit instruction words, in the encodings of the module docstring."""

    def __init__(self, origin):
        self.origin, self.hw, self.labels, self.fix = origin, [], {}, []
        self.lines = []

    @property
    def pc(self):
        return self.origin + 2 * len(self.hw)

    def label(self, name):
        self.labels[name] = self.pc
        self.lines.append((self.pc, [], f"{name}:"))

    def emit(self, text, *h):
        self.lines.append((self.pc, [x & 0xFFFF for x in h], text))
        self.hw += [x & 0xFFFF for x in h]

    # data movement
    def mov(self, n, imm8):
        assert 0 <= imm8 < 256
        self.emit(f"mov R{n}, #0x{imm8:X}", 0xB800 | n << 8 | imm8)

    def movi(self, n, imm32):
        self.emit(f"mov R{n}, #0x{imm32:08X}", 0xC000 | n << 8, imm32 & 0xFFFF, imm32 >> 16)

    def stw(self, reg, n):  # RAC word := Rn
        self.emit(f"stw RAC.{reg}, R{n}", 0xA800 | n << 8 | rac(reg))

    def stwi(self, idx, n):  # RAC word idx := Rn, for a word the published map does not name
        self.emit(f"stw RAC[0x{idx:X}], R{n}", 0xA800 | n << 8 | idx)

    def ld(self, n, s):  # Rn := mem[Rs]
        self.emit(f"ld R{n}, [R{s}]", 0x1004 | n << 8 | s << 4)

    def st(self, n, s):  # mem[Rn] := Rs
        self.emit(f"st [R{n}], R{s}", 0x1008 | n << 8 | s << 4)

    def setreg(self, reg, value, tmp=0):
        if value < 256:
            self.mov(tmp, value)
        else:
            self.movi(tmp, value)
        self.stw(reg, tmp)

    def store(self, name, value, ra=2, rv=0):
        self.storeAt(addr(name), value, ra, rv)

    def storeAt(self, address, value, ra=2, rv=0):
        """mem[address] := value, for a word the published map does not name."""
        self.movi(ra, address)
        if value < 256:
            self.mov(rv, value)
        else:
            self.movi(rv, value)
        self.st(ra, rv)

    # arithmetic
    def or_(self, n, s):
        self.emit(f"or R{n}, R{s}", 0x4001 | n << 8 | s << 4)

    def neg(self, n):
        self.emit(f"neg R{n}", 0x5801 | n << 8)

    def cmpadd(self, n, s):  # flags of Rn + Rs, nothing written
        self.emit(f"cmpadd R{n}, R{s}", 0x2800 | n << 8 | s << 4)

    def and_(self, n, s):  # Rn &= Rs
        self.emit(f"and R{n}, R{s}", 0x3801 | n << 8 | s << 4)

    def shl(self, n, k):
        self.emit(f"shl R{n}, #{k}", 0x6880 | n << 8 | k)

    def shr(self, n, k):
        self.emit(f"shr R{n}, #{k}", 0x6080 | n << 8 | k)

    # bit operations: on RAC word Rn (rac_) or on the word at address Rn
    def bset(self, n, b, rac_=True):
        self.emit(f"bset {'RAC' if rac_ else ''}[R{n}], {b}", 0x7820 | n << 8 | (0x40 if rac_ else 0) | b)

    def bclr(self, n, b, rac_=True):
        self.emit(f"bclr {'RAC' if rac_ else ''}[R{n}], {b}", 0x7800 | n << 8 | (0x40 if rac_ else 0) | b)

    def tst(self, n, imm8):  # Z when Rn & imm8 is 0
        self.emit(f"tst R{n}, #0x{imm8:X}", 0xD800 | n << 8 | imm8)

    # control
    def delay(self, ticks):  # ticks of (RAC.PRESC + 1) / f_xo: 208 ns at 38.4 MHz
        assert 0 <= ticks < 1024
        self.emit(f"delay {ticks}", 0x0C00 | ticks)

    def b(self, cond, target):
        self.fix.append((len(self.hw), cond, target))
        self.emit(f"b{cond} {target}", 0x7000 | COND[cond] << 8)

    def end(self):
        self.emit("end", 0xF800)

    def assemble(self):
        for i, cond, target in self.fix:
            off = self.labels[target] - (self.origin + 2 * i)
            assert -128 <= off < 128, (target, off)
            self.hw[i] = 0x7000 | COND[cond] << 8 | (off & 0xFF)
        return self.hw


# ---- the program ----

# The VCO search's rounds: (bit of SYNTH.VCOTUNING, offset added to the trial value, LOCNTCTRL with
# its NUMCYCLE). Bit 10 down to bit 0, a longer count as the step gets finer (docs/hardware_mg21.md
# §10.17).
VCO_ROUNDS = [(10, 0x10, 0x21), (9, 0x08, 0x21), (8, 0x08, 0x31), (7, 0x02, 0x31), (6, 0x02, 0x31),
              (5, 0x02, 0x31), (4, 0x01, 0x41), (3, 0x01, 0x41), (2, 0x00, 0x51), (1, 0x00, 0x51),
              (0, 0x00, 0x51)]
VCO_BASE = 0x8000   # VCAPSEL 16
VCO_SETTLE = 15     # ticks after each trial value, before the count
RAC_D0 = 0xD0 // 4  # a RAC word the published map does not name; the warm-up writes 0xF to it
RAC_90 = 0x90 // 4  # another: 0 for receive, bit 14 for transmit
SYNTH_54 = 0xB8018054  # an unnamed SYNTH word: 0xC8 for receive, 0x80C8 for transmit
MODEM_2C = 0xB801402C  # an unnamed MODEM word the transmit warm-up clears
AGC_30 = 0xB800C030    # an unnamed AGC word
# The AGC words a transmission overrides, with their receive values (docs/hardware_mg21.md §10.5).
AGC_RX = [("AGC.LNAMIXCODE0", 0x15724BBD), ("AGC.PNRFATT0", 0x06102040), (AGC_30, 0x22E)]
AGC_TX = [("AGC.PNRFATT0", 0x3F), ("AGC.LNAMIXCODE0", 0x0), (AGC_30, 0x1)]
# The PA calibration words the stock firmware leaves for its transmit warm-up (measured on the bench).
PATRIM1_TX, PATRIM2_TX = 0x40000980, 0x6BDFF74A
# The CPU's cells. PA_CELL: PACTRL[23:16] (PASELSLICE, PAPOWER) of the transmission, shifted into
# place. RX_AFTER_TX_CELL: non-zero when the receiver is to follow the transmission at once (an Ack is
# due, or the host keeps receiving) - TX then holds a receive source, so the RAC passes TX2RX.
PA_CELL = 0x1F00
RX_AFTER_TX_CELL = 0x1F04


def vcoSearch(a):
    """Successive approximation of SYNTH.VCOTUNING against the LO counter: a bit is kept when the
    count exceeds SYNTH.LOCNTTARGET (the VCO is still too fast)."""
    tag = f"vco{a.pc:04X}_"              # labels unique per inlined copy
    a.movi(2, addr("SYNTH.VCOTUNING"))
    a.movi(4, VCO_BASE)
    a.movi(5, addr("SYNTH.LOCNTCTRL"))
    for k, extra, ctrl in VCO_ROUNDS:
        a.movi(0, (1 << k) | extra)
        a.or_(0, 4)
        a.st(2, 0)
        a.delay(VCO_SETTLE)
        a.movi(3, addr("SYNTH.IF.CLR"))
        a.movi(1, 0x200)
        a.st(3, 1)                       # clear LOCNTDONE
        a.mov(1, ctrl)
        a.st(5, 1)                       # ENABLE, NUMCYCLE
        a.bset(5, 1, rac_=False)         # CLEAR
        a.bset(5, 2, rac_=False)         # RUN
        a.movi(3, addr("SYNTH.IF"))
        a.label(f"{tag}wait{k}")
        a.ld(1, 3)
        a.shr(1, 9)
        a.tst(1, 0x1)
        a.b("z", f"{tag}wait{k}")             # until LOCNTDONE
        a.bset(5, 3, rac_=False)         # READ
        a.movi(3, addr("SYNTH.LOCNTSTATUS"))
        a.ld(0, 3)
        a.bclr(5, 3, rac_=False)
        a.shl(0, 13)
        a.shr(0, 13)                     # LOCOUNT, bits 18:0
        a.movi(3, addr("SYNTH.LOCNTTARGET"))
        a.ld(1, 3)
        a.neg(0)
        a.cmpadd(1, 0)                   # N when the count exceeds the target
        a.b("n", f"{tag}keep{k}")
        a.b("", f"{tag}next{k}")
        a.label(f"{tag}keep{k}")
        a.movi(1, 1 << k)
        a.or_(4, 1)
        a.label(f"{tag}next{k}")
    a.st(2, 4)
    a.mov(0, 0)
    a.st(5, 0)                           # LO counter off


def agcWrite(a, pairs):
    for target, value in pairs:
        if isinstance(target, str):
            a.store(target, value, ra=3, rv=5)
        else:
            a.storeAt(target, value, ra=3, rv=5)


def synthFor(a, tx):
    """The synthesiser and LO settings that differ between receive and transmit."""
    a.store("SYNTH.CMD", 0x2)            # SYNTHSTOP
    a.store("SYNTH.CMD", 0x8 if tx else 0x4)  # DISABLEIF / ENABLEIF
    a.storeAt(SYNTH_54, 0x80C8 if tx else 0xC8)
    a.setreg("SYNTHENCTRL", 0x100000 if tx else 0x0)
    a.setreg("RX", 0x480 if tx else 0x3800)
    a.setreg("SYTRIM0", 0x302E29 if tx else 0x300029)
    a.movi(0, 0x4000 if tx else 0x0)
    a.stwi(RAC_90, 0)
    a.setreg("VCOCTRL", 0xAC if tx else 0x4C)
    a.setreg("SYTRIM1", 0x19FD8)


def rxwarm(a):
    synthFor(a, tx=False)
    agcWrite(a, AGC_RX)
    a.mov(4, rac("RADIOEN"))
    a.mov(1, rac("SYNTHENCTRL"))
    a.mov(0, rac("SYEN"))
    a.bset(0, 1)
    a.bset(4, 2)
    a.bset(4, 0)
    a.delay(29)
    a.bset(1, 1)
    for b in (7, 4, 11, 2, 5, 3, 0):
        a.bset(0, b)
    a.bset(4, 1)
    a.delay(2)
    a.bset(0, 8)
    a.delay(2)
    a.bset(0, 9)
    a.bset(0, 12)
    a.delay(2)
    a.movi(2, addr("SYNTH.VCDACCTRL"))
    for b in (7, 8, 6):
        a.bset(2, b, rac_=False)
    a.bset(0, 10)
    a.delay(5)
    a.bclr(1, 1)
    a.delay(10)
    vcoSearch(a)
    a.store("SYNTH.CMD", 0x1, ra=3, rv=5)  # SYNTHSTART
    a.delay(2)
    a.movi(2, addr("SYNTH.VCDACCTRL"))
    a.bclr(2, 8, rac_=False)
    a.delay(2)
    a.setreg("SYLOEN", 0x9)
    a.mov(3, rac("CMD"))
    a.bset(3, 14)                        # LNAENSET
    a.mov(3, rac("LNAMIXEN"))
    a.bset(3, 0)
    a.setreg("RX", 0x385E)
    a.setreg("RFPATHEN2", 0x2E)
    a.setreg("RFPATHEN1", 0x4)
    a.mov(0, 0xF)
    a.stwi(RAC_D0, 0)
    a.setreg("PGACTRL", 0x02515E6C)
    a.end()


def rxpd(a):
    a.mov(4, 0)
    for reg in ("RX", "RFPATHEN1", "RFPATHEN2"):
        a.stw(reg, 4)
    a.setreg("PGACTRL", 0x02510060)
    a.mov(3, rac("CMD"))
    a.bset(3, 15)                        # LNAENCLEAR
    a.store("SYNTH.CMD", 0x2)            # SYNTHSTOP
    a.mov(0, rac("SYEN"))
    a.bclr(0, 9)
    a.delay(2)
    a.bclr(0, 8)
    a.delay(2)
    a.mov(1, rac("RADIOEN"))
    a.bclr(1, 1)
    a.stw("SYEN", 4)
    a.stw("SYNTHENCTRL", 4)
    a.movi(2, addr("SYNTH.VCDACCTRL"))
    for b in (8, 6, 7):
        a.bclr(2, b, rac_=False)
    a.stw("RADIOEN", 4)
    a.end()


def synthUp(a):
    """RADIOEN, SYNTHENCTRL and SYEN bit by bit, with the delays the bench measured, then the VCO
    search and SYNTHSTART (the same in both directions)."""
    a.mov(4, rac("RADIOEN"))
    a.mov(1, rac("SYNTHENCTRL"))
    a.mov(0, rac("SYEN"))
    a.bset(4, 2)
    a.bset(4, 0)
    a.delay(29)
    a.bset(1, 1)
    for b in (7, 4, 11, 2, 5, 3, 0):
        a.bset(0, b)
    a.bset(4, 1)
    a.delay(2)
    a.bset(0, 8)
    a.delay(2)
    a.bset(0, 9)
    a.bset(0, 12)
    a.delay(2)
    a.movi(2, addr("SYNTH.VCDACCTRL"))
    for b in (7, 8, 6):
        a.bset(2, b, rac_=False)
    a.bset(0, 10)
    a.delay(5)
    a.bclr(1, 1)
    a.delay(10)
    vcoSearch(a)
    a.store("SYNTH.CMD", 0x1, ra=3, rv=5)  # SYNTHSTART
    a.delay(2)
    a.movi(2, addr("SYNTH.VCDACCTRL"))
    a.bclr(2, 8, rac_=False)
    a.delay(2)


def txwarm(a):
    """The vendor's transmit warm-up on the 10 dBm PA, the PA this part transmits on (§10.17)."""
    synthFor(a, tx=True)
    synthUp(a)
    a.setreg("PATRIM1", PATRIM1_TX)
    a.setreg("PATRIM2", PATRIM2_TX)
    a.setreg("SYTRIM1", 0x19FD3)
    a.storeAt(MODEM_2C, 0x0)
    a.store("FRC.FCD0", 0x4000)
    a.store("FRC.FCD1", 0x4CFF)
    a.mov(0, rac("CMD"))
    a.bset(0, 12)                        # PAENSET
    a.mov(0, rac("PATRIM2"))
    a.bclr(0, 2)
    a.bset(0, 3)
    a.bclr(0, 4)
    agcWrite(a, AGC_TX)
    a.mov(2, rac("RFPATHEN1"))
    a.bset(2, 4)
    a.bset(2, 2)
    a.mov(2, rac("RFPATHEN2"))
    a.bset(2, 4)
    a.bset(2, 2)
    a.bset(2, 1)
    a.mov(2, rac("RX"))
    a.bset(2, 4)
    a.bset(2, 5)
    a.mov(2, rac("LNAMIXEN"))
    a.bset(2, 0)
    # the PA: PASELSLICE/PAPOWER from the CPU's cell, then the 10 dBm stages in the vendor's order
    a.movi(3, SEQRAM + PA_CELL)
    a.ld(0, 3)
    a.mov(2, rac("PACTRL"))
    a.emit("ldw R1, RAC.PACTRL", 0xB100 | rac("PACTRL"))
    a.movi(4, 0xFF00FFFF)
    a.and_(1, 4)
    a.or_(0, 1)
    a.stw("PACTRL", 0)
    a.mov(0, rac("PACTRL"))
    a.bset(0, 8)
    a.bclr(0, 0)
    a.bclr(0, 1)
    a.mov(1, rac("TX"))
    a.bset(1, 31)
    a.bset(1, 30)
    a.bset(1, 27)
    a.delay(15)
    a.bset(1, 26)
    a.delay(20)
    a.bset(1, 16)
    a.bset(1, 18)
    a.mov(2, rac("RFPATHEN2"))
    a.bset(2, 5)                         # PAENANT2
    a.bset(0, 10)                        # PASLICERST, pulsed
    a.delay(2)
    a.bclr(0, 10)
    a.delay(7)
    a.bclr(1, 1)
    a.bclr(1, 0)
    a.delay(12)
    a.setreg("SYLOEN", 0x400, tmp=3)
    a.store("FRC.IF.CLR", 0x17D, ra=3, rv=5)
    a.movi(3, addr("FRC.BUFFERMODE"))
    a.bclr(3, 0, rac_=False)
    a.mov(3, rac("APC"))
    a.bset(3, 2)
    a.delay(30)
    a.bset(1, 28)                        # PAENPAOUT
    a.bset(0, 4)                         # PAENPOWERRAMPINGCLK
    a.delay(2)
    a.mov(4, rac("PAENCTRL"))
    a.bset(4, 8)                         # PARAMP
    a.delay(30)
    a.bclr(0, 4)
    a.bclr(3, 2)
    a.end()


def paDown(a):
    a.mov(4, rac("PAENCTRL"))
    a.bclr(4, 8)
    a.mov(4, 0)
    a.stw("TX", 4)
    a.setreg("PACTRL", 0x100)
    a.mov(3, rac("CMD"))
    a.bset(3, 13)                        # PAENCLEAR
    a.mov(4, 0)
    a.stw("SYLOEN", 4)
    agcWrite(a, AGC_RX)


def tx(a):
    """The frame goes out without the program; TX only decides what follows it, the way the vendor's TX
    does: with RXENSRCEN bit 1 held the RAC turns to receive (TX2RX), without it powers down (TXPD)."""
    a.movi(3, SEQRAM + RX_AFTER_TX_CELL)
    a.ld(0, 3)
    a.tst(0, 0xFF)
    a.b("z", "txDone")
    a.mov(3, rac("RXENSRCEN"))
    a.bset(3, 1)
    a.label("txDone")
    a.end()


def txpd(a):
    """The PA and the RF path down, the AGC back to its receive values, then rxpd's power-down."""
    paDown(a)
    rxpd(a)


def tx2rx(a):
    """Transmit straight into receive - entered when a receive source is held across the transmission,
    for the Ack of a frame that asked for one: the PA down, then the whole receive warm-up, since the
    RAC goes on to RXSEARCH without passing RXWARM."""
    paDown(a)
    rxwarm(a)


PROGRAM = {"RXWARM": rxwarm, "RXPD": rxpd, "TXWARM": txwarm, "TX": tx, "TXPD": txpd,
           "TX2RX": tx2rx}


def build(program=None):
    """(image words, handler entries, listing lines)."""
    a = Asm(HANDLERS)
    a.label("stub")
    a.end()
    entries = {}
    for name, fn in (program or PROGRAM).items():
        a.label(name)
        entries[name] = a.pc
        fn(a)
    hw = a.assemble()
    vec = []
    for s in STATES:
        vec += [0x9802, entries.get(s, HANDLERS)]
    vec += [0xF800] * ((HANDLERS - 2 * len(vec)) // 2)
    hw = vec + hw
    if len(hw) % 2:
        hw.append(0xF800)
    assert 2 * len(hw) <= STACK, "the image would reach the call stack"
    return [hw[i] | hw[i + 1] << 16 for i in range(0, len(hw), 2)], entries, a.lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--words", action="store_true")
    a = ap.parse_args()
    words, entries, lines = build()
    if a.words:
        for i, w in enumerate(words):
            print(f"{4 * i:04X} {w:08X}")
        return
    print(f"; {len(words)} words, " + ", ".join(f"{k} at 0x{v:04X}" for k, v in entries.items()))
    for pc, hw, text in lines:
        print(f"{pc:04X}  {' '.join(f'{h:04X}' for h in hw):<15} {text}")


if __name__ == "__main__":
    main()
