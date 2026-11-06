/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* Minimal SAMR21 register map - only what the HAL uses. Offsets and bit fields
 * per the SAMR21G18A datasheet (DS40001946). */
#ifndef SAMR21_REGS_H
#define SAMR21_REGS_H

#include <stdint.h>

/* ---- SYSCTRL (base 0x40000800) ---- */
typedef struct
{
    volatile uint32_t pad0[2];  /* +0x00 INTENCLR, +0x04 INTENSET */
    volatile uint32_t INTFLAG;  /* +0x08  interrupt flags (write-1-to-clear) */
    volatile uint32_t PCLKSR;   /* +0x0C  power/clock status */
    volatile uint32_t pad1[2];  /* +0x10 XOSC, +0x14 XOSC32K */
    volatile uint32_t OSC32K;   /* +0x18  32 kHz internal oscillator */
    uint32_t pad2;              /* +0x1C OSCULP32K */
    volatile uint32_t OSC8M;    /* +0x20  8 MHz internal oscillator */
    volatile uint32_t DFLLCTRL; /* +0x24  DFLL48M control */
    volatile uint32_t DFLLVAL;  /* +0x28  DFLL48M coarse value */
    volatile uint32_t DFLLMUL;  /* +0x2C  DFLL48M multiplier */
} Samr21Sysctrl;

#define SYSCTRL_PCLKSR_OSC8MRDY (1u << 3)
#define SYSCTRL_PCLKSR_DFLLRDY (1u << 4)
/* PCLKSR lock flags: LCKF bit 6, LCKC bit 7 (bit 8 is DFLLRCS). */
#define SYSCTRL_PCLKSR_DFLLLCKF (1u << 6)
#define SYSCTRL_PCLKSR_DFLLLCKC (1u << 7)

/* INTFLAG bits the BTL clears before the DFLL bring-up: PCLKSR status latches from
 * this domain, so stale POR flags make the first DFLLRDY wait return instantly. */
#define SYSCTRL_INTFLAG_DFLLRDY (1u << 4)
/* The two DFLL lock flags share their positions with PCLKSR (6/7)
 * (docs/hardware_samr21.md §2, vendor halLowLevelInit.c:867-868). */
#define SYSCTRL_INTFLAG_DFLLLCKF (1u << 6)
#define SYSCTRL_INTFLAG_DFLLLCKC (1u << 7)
#define SYSCTRL_INTFLAG_BOD33RDY (1u << 9)
#define SYSCTRL_INTFLAG_BOD33DET (1u << 10)

/* OSC32K: ENABLE 1, EN32K 2, EN1K 3, RUNSTDBY 6, ONDEMAND 7,
 * STARTUP[10:8] (1 = 130 us), CALIB[22:16]. */
#define SYSCTRL_OSC32K_ENABLE (1u << 1)
#define SYSCTRL_OSC32K_EN32K (1u << 2)
#define SYSCTRL_OSC32K_EN1K (1u << 3)
#define SYSCTRL_OSC32K_RUNSTDBY (1u << 6)
#define SYSCTRL_OSC32K_ONDEMAND (1u << 7)
#define SYSCTRL_OSC32K_STARTUP(s) (((s) & 0x7u) << 8)
#define SYSCTRL_OSC32K_CALIB(c) (((c) & 0x7Fu) << 16)

/* OSC8M: ENABLE 1, RUNSTDBY 6, ONDEMAND 7, PRESC[9:8] (the GCLK source
 * is the prescaled output: 8 MHz / 2^PRESC — the reset 3 = 1 MHz).
 * CALIB[25:16]/FRANGE[31:30] are preserved by read-modify-write. */
#define SYSCTRL_OSC8M_ENABLE (1u << 1)
#define SYSCTRL_OSC8M_RUNSTDBY (1u << 6)
#define SYSCTRL_OSC8M_ONDEMAND (1u << 7)
#define SYSCTRL_OSC8M_PRESC(p) (((p) & 0x3u) << 8)

/* DFLLCTRL is a 16-bit register in a 32-bit slot, reset 0x0080 (ONDEMAND).
 * ONDEMAND (bit 7) gates the DFLL register writes: the errata-9905 procedure writes
 * ENABLE first, clearing ONDEMAND, before the remaining writes are accepted. */
#define SYSCTRL_DFLLCTRL_ENABLE (1u << 1)
#define SYSCTRL_DFLLCTRL_MODE (1u << 2)   /* 1 = closed loop */
#define SYSCTRL_DFLLCTRL_STABLE (1u << 3) /* 1 = fix after lock */
#define SYSCTRL_DFLLCTRL_LLAW (1u << 4)   /* lose lock after wake */
#define SYSCTRL_DFLLCTRL_USBCRM (1u << 5) /* USB clock recovery (SOF) */
#define SYSCTRL_DFLLCTRL_RUNSTDBY (1u << 6)
#define SYSCTRL_DFLLCTRL_ONDEMAND (1u << 7)
#define SYSCTRL_DFLLCTRL_CCDIS (1u << 8)
#define SYSCTRL_DFLLCTRL_QLDIS (1u << 9)
#define SYSCTRL_DFLLCTRL_BPLCKC (1u << 10)
#define SYSCTRL_DFLLCTRL_WAITLOCK (1u << 11)

/* DFLLVAL: FINE[9:0] fine value, COARSE[15:10] coarse value; DIFF[31:16]
 * (multiplication ratio difference) is read-only. */
#define SYSCTRL_DFLLVAL_FINE(f) ((f) & 0x03FFu)
#define SYSCTRL_DFLLVAL_COARSE(c) (((c) & 0x3Fu) << 10)

/* DFLLMUL: MUL[15:0], FSTEP[30:16], CSTEP[31:26]. */
#define SYSCTRL_DFLLMUL_MUL(m) ((m) & 0x0000FFFFu)
#define SYSCTRL_DFLLMUL_FSTEP(f) (((f) & 0x3FFu) << 16)
#define SYSCTRL_DFLLMUL_CSTEP(c) (((c) & 0x1Fu) << 26)

/* ---- GCLK (base 0x40000C00) ----
 * Byte-packed layout (NOT the SAMD21 32-bit register map): single
 * indirect registers selected by the ID field of the written value.
 * Vendor CMSIS (verified against the frozen-state readbacks):
 *   CTRL    @0x00 (8-bit)  SWRST bit0
 *   STATUS  @0x01 (8-bit)  SYNCBUSY bit7 — reads 0 while a dead-source
 *                           write is pending; it does NOT flag the
 *                           stuck sync, and the next GCLK access after
 *                           such a write stalls the CPU bus forever
 *   CLKCTRL @0x02 (16-bit) ID[5:0], GEN[11:8], CLKEN bit14, WRTLOCK 15
 *   GENCTRL @0x04 (32-bit) ID[3:0], SRC[12:8], GENEN bit16, IDC 17,
 *                           OOV 18, OE 19, DIVSEL 20, RUNSTDBY 21
 *   GENDIV  @0x08 (32-bit) ID[3:0], DIV[23:8]  */
typedef struct
{
    volatile uint8_t CTRL;     /* +0x00 */
    volatile uint8_t STATUS;   /* +0x01 */
    volatile uint16_t CLKCTRL; /* +0x02 */
    volatile uint32_t GENCTRL; /* +0x04 */
    volatile uint32_t GENDIV;  /* +0x08 */
} Samr21Gclk;

#define GCLK_CTRL_SWRST (1u << 0)
#define GCLK_STATUS_SYNCBUSY (1u << 7)

/* Generator selection + source. Field positions: ID[3:0], SRC[12:8], GENEN bit16.
 * Source numbers: XOSC 0, GCLKIN 1, GCLKGEN1 2, OSCULP32K 3, OSC32K 4, XOSC32K 5,
 * OSC8M 6, DFLL48M 7, FDPLL 8; XOSC is not populated on the CB2. */
#define GCLK_GENCTRL_ID(id) ((id) & 0xFu)
#define GCLK_GENCTRL_SRC_GCLKIN (1u << 8)
#define GCLK_GENCTRL_SRC_GCLKGEN1 (2u << 8)
#define GCLK_GENCTRL_SRC_OSCULP32K (3u << 8)
#define GCLK_GENCTRL_SRC_OSC8M (6u << 8)
#define GCLK_GENCTRL_SRC_DFLL48M (7u << 8)
#define GCLK_GENCTRL_GENEN (1u << 16)
/* Immediate dead cycle (halLowLevelInit.c:799,:828, via
 * docs/hardware_samr21.md §2). */
#define GCLK_GENCTRL_IDC (1u << 17)
#define GCLK_GENCTRL_OE (1u << 19)
#define GCLK_GENCTRL_DIVSEL (1u << 20) /* 1 = /2^(DIV+1) */
#define GCLK_GENCTRL_RUNSTDBY (1u << 21)

#define GCLK_GENDIV_ID(id) ((id) & 0xFu)
#define GCLK_GENDIV_DIV(d) (((d) & 0xFFFFu) << 8)

#define GCLK_CLKCTRL_ID_DFLL48 0x00u
#define GCLK_CLKCTRL_ID_WDT 0x03u
#define GCLK_CLKCTRL_ID_RTC 0x04u
#define GCLK_CLKCTRL_ID_EIC 0x05u
#define GCLK_CLKCTRL_ID_USB 0x06u
#define GCLK_CLKCTRL_ID_TCC01 0x1Au
#define GCLK_CLKCTRL_ID_TC4_TC5 0x1Cu
#define GCLK_CLKCTRL_ID_SERCOMX_SLOW 0x13u
#define GCLK_CLKCTRL_ID_SERCOM2_CORE 0x16u
#define GCLK_CLKCTRL_ID_SERCOM4_CORE 0x18u
#define GCLK_CLKCTRL_ID_SERCOM5_CORE 0x19u /* samr21.txt:5566 */
/* Channel + generator. Field positions: ID[5:0], GEN[11:8], CLKEN bit14. */
#define GCLK_CLKCTRL_GEN(g) (((g) & 0xFu) << 8)
#define GCLK_CLKCTRL_GEN_MASK (0xFu << 8) /* GEN[11:8] is a FIELD, not a bitmask */
#define GCLK_CLKCTRL_ID(id) ((id) & 0x3Fu)
#define GCLK_CLKCTRL_CLKEN (1u << 14)

/* ---- WDT (base 0x40001000) ----
 * Feed: write 0xA5 to CLEAR (offset 0x08). Cannot be disabled once started. */
typedef struct
{
    volatile uint8_t CTRL;     /* +0x00 */
    volatile uint8_t CONFIG;   /* +0x01 */
    volatile uint8_t EWCTRL;   /* +0x02 */
    uint8_t pad0;              /* +0x03 */
    volatile uint8_t INTENCLR; /* +0x04 */
    volatile uint8_t INTENSET; /* +0x05 */
    volatile uint8_t INTFLAG;  /* +0x06 */
    volatile uint8_t STATUS;   /* +0x07 */
    volatile uint8_t CLEAR;    /* +0x08  feed key, write-only */
} Samr21Wdt;

#define WDT_CTRL_ENABLE (1u << 1)
#define WDT_CTRL_WEN (1u << 2)      /* window mode */
#define WDT_CTRL_ALWAYSON (1u << 7) /* set at reset; POR-only clear */
/* PER[4:0]: period = 2^(PER+3) WDT-clock cycles. The BTL starts it with
 * WDT_INTERVAL_16000 = enum value 11 (de_btl/+++wdt.h); on gen4
 * (ULP32K/16 = 2048 Hz) that is 2^14 / 2048 = 8.0 s. CONFIG resets to
 * 0xBB, which already carries PER 11. */
#define WDT_CONFIG_PER(p) ((p) & 0x1Fu)
#define WDT_INTENCLR_EW (1u << 0)
#define WDT_STATUS_SYNCBUSY (1u << 7)
#define WDT_CLEAR 0xA5u

/* ---- PM (base 0x40000400) ----
 * Clock/power gating masks; reset to 0x7F. The update-flag clear re-ungates the
 * NVMCTRL bits anyway: a hang there would leave the flag set and the next boot
 * would erase the app page (eternal update window). */
typedef struct
{
    uint8_t pad0[8];            /* +0x00..0x07 */
    volatile uint8_t CPUSEL;    /* +0x08  CPU divider (0 = /1) */
    volatile uint8_t APBASEL;   /* +0x09  APBA divider */
    volatile uint8_t APBBSEL;   /* +0x0A  APBB divider */
    volatile uint8_t APBCSEL;   /* +0x0B  APBC divider */
    uint8_t pad1[8];            /* +0x0C..0x13 */
    volatile uint32_t AHBMASK;  /* +0x14  AHB clock mask */
    volatile uint32_t APBAMASK; /* +0x18  APB-A clock mask */
    volatile uint32_t APBBMASK; /* +0x1C  APB-B clock mask */
    volatile uint32_t APBCMASK; /* +0x20  APB-C clock mask */
} Samr21Pm;

#define PM_AHBMASK_NVMCTRL (1u << 4)
#define PM_APBBMASK_NVMCTRL (1u << 2)
#define PM_APBAMASK_GCLK (1u << 3) /* APBA mask (reset 0x7F: set) */
/* R21 APBB bit for the USB clock is 5 (not 8 as on the SAMD21);
 * APBBMASK resets to 0x7F, so it is enabled at POR already. */
#define PM_APBBMASK_USB (1u << 5)
/* APBCMASK resets to 0x00010000 (unlike AHBMASK/APBAMASK): the SERCOM4
 * gate is off after a reset and must be set before the SERCOM can run. */
#define PM_APBCMASK_SERCOM4 (1u << 6)
#define PM_APBCMASK_SERCOM5 (1u << 7) /* APBCMASK bit 7 = SERCOM5 (samr21.txt:7295-7297) */
#define PM_APBCMASK_TC4 (1u << 12)
#define PM_APBCMASK_TC5 (1u << 13)
#define PM_APBCMASK_RFCTRL (1u << 21) /* APBCMASK bit 21 = RFCTRL (samr21.txt:7239-7241) */
/* APBAMASK bit 6 gates the EIC's bus clock; the reset value 0x7F has it on. */
#define PM_APBAMASK_EIC (1u << 6)

/* ---- DSU (base 0x41002000) ----
 * Device Service Unit: chip identity and die revision. Read-only here - the
 * ADDR/LENGTH/DATA service registers are shared and a command is discarded until
 * STATUSA.DONE (11.11.1, samr21.txt:2966-2971); CTRL.SWRST is a debugger facility.
 * Base 0x41002000 (samr21.txt:2513), AHB-clocked out of reset (AHBMASK reset 0x7F
 * :6953-6955, bit 3 = DSU :6987, :7009-7011); every PM mask write in this tree is
 * `|=`, so nothing upstream can have stopped that clock.
 * Offsets per Table 11-7 (samr21.txt:3193-3236): CTRL 0x00, STATUSA 0x01,
 * STATUSB 0x02, 0x03 reserved :3199, ADDR 0x04, LENGTH 0x08, DATA 0x0C, DCC0 0x10,
 * DCC1 0x14, DID 0x18 (§11.13.8 :3728-3733), 0x1C..0x00FF Reserved :3229-3236.
 * DID fields (samr21.txt:3770-3800): PROCESSOR[31:28] :3770, FAMILY[27:23] :3774,
 * SERIES[21:16] :3782, DIE[15:12] :3786, REVISION[11:8] :3793-3794 (the die
 * revision), DEVSEL[7:0] :3796; Table 11-8 :3801-3819 gives 0x19 = SAMR21G18A
 * :3809, 0x1C = SAMR21E18A :3815.
 * No DIDCID on this die: Table 11-7 goes from DID straight to Reserved
 * (samr21.txt:3229-3236) and no SAMR21 CMSIS defines it. The CoreSight ROM ID
 * serves instead (§11.10.1 :2920-2956, §11.10.2 :2957-2962, Table 11-7
 * :3249-3332): PID1 0x1FE4 :4109, PID2 0x1FE8 :4165, CID3 0x1FFC :4443, reset
 * values PID1 0xFC :4110, PID2 0x09 :4166, CID3 0xB1 :4444 (the Atmel DSU
 * signature :4152-4156); PID2.REVISION[7:4] :4208 is the DSU peripheral's
 * revision, not the die's. */
typedef struct
{
    volatile uint8_t CTRL;    /* +0x00 ( /W  8) CE MBIST CRC SWRST */
    volatile uint8_t STATUSA; /* +0x01 (R/W  8) PERR FAIL BERR CRSTEXT DONE */
    volatile uint8_t STATUSB; /* +0x02 (R/   8) HPE DCCD1 DCCD0 DBGPRES PROT */
    uint8_t pad0;             /* +0x03 reserved (Table 11-7, samr21.txt:3199) */
    uint32_t pad1[5];         /* +0x04..0x17 ADDR / LENGTH / DATA / DCC0 / DCC1 */
    volatile uint32_t DID;    /* +0x18 (R/  32) Device Identification */
} Samr21Dsu;

/* DID is the last member of a 4-aligned struct, so sizeof == 0x1C is the statement
 * "DID sits at 0x0018": a miscounted pad stops the build. */
#define SAMR21_DSU_BASE 0x41002000u
typedef char dsuDidSitsAtTable11_7Offset0018[(sizeof(Samr21Dsu) == 0x001Cu) ? 1 : -1];

#define DSU_DID_PROCESSOR(d) (((d) >> 28) & 0xFu) /* samr21.txt:3770-3773 */
#define DSU_DID_FAMILY(d) (((d) >> 23) & 0x1Fu)   /* :3774-3776 */
#define DSU_DID_SERIES(d) (((d) >> 16) & 0x3Fu)   /* :3782-3784 */
#define DSU_DID_DIE(d) (((d) >> 12) & 0xFu)       /* :3786-3788 */
#define DSU_DID_REVISION(d) (((d) >> 8) & 0xFu)   /* :3793-3794: THE die revision */
#define DSU_DID_DEVSEL(d) ((d) & 0xFFu)           /* :3796-3800, Table 11-8 */

/* CMSIS's own reset value for this ordering code (samr21g18a.h:532); the guard
 * checks the extraction macros against it, so a wrong shift stops the build. */
#define DSU_DID_G18A_CMSIS 0x10010019u
#define DSU_DEVSEL_SAMR21G18A 0x19u /* Table 11-8, samr21.txt:3809 */
#define DSU_DEVSEL_SAMR21E18A 0x1Cu /* :3815 */
typedef char dsuDidFieldLayoutMatchesCmsisDid
    [(DSU_DID_PROCESSOR(DSU_DID_G18A_CMSIS) == 1u && DSU_DID_FAMILY(DSU_DID_G18A_CMSIS) == 0u &&
      DSU_DID_SERIES(DSU_DID_G18A_CMSIS) == 1u && DSU_DID_DIE(DSU_DID_G18A_CMSIS) == 0u &&
      DSU_DID_REVISION(DSU_DID_G18A_CMSIS) == 0u &&
      DSU_DID_DEVSEL(DSU_DID_G18A_CMSIS) == DSU_DEVSEL_SAMR21G18A)
         ? 1
         : -1];

/* CoreSight ROM identity, kept out of the struct so the 0x00..0x1C window stays
 * compact. PID1 0x1FE4 :4109, PID2 0x1FE8 :4165, CID3 0x1FFC :4443 (samr21.txt
 * Table 11-7 :3249-3332); §11.11.2.3 (:2983-2986): CPU access is unrestricted. */
#define SAMR21_DSU_ROM_U32(off)                                                                    \
    (*(volatile const uint32_t *)((const volatile uint8_t *)SAMR21_DSU_BASE + (off)))
#define DSU_ROM_PID1 SAMR21_DSU_ROM_U32(0x1FE4u)
#define DSU_ROM_PID2 SAMR21_DSU_ROM_U32(0x1FE8u)
#define DSU_ROM_CID3 SAMR21_DSU_ROM_U32(0x1FFCu)
/* PARTNBH 0xC + JEPIDCL 0xF: "indicating that this device implements a DSU module
 * instance", samr21.txt:4152-4156; reset value :4110. */
#define DSU_PID1_ATMEL_DSU 0xFCu
#define DSU_CID3_ATMEL 0xB1u /* Atmel JEP-106, samr21.txt:4444 */
/* PID2.REVISION[7:4] is the DSU peripheral revision, not the die's
 * (samr21.txt:4208). */
#define DSU_PID2_REVISION(p) (((p) >> 4) & 0xFu)

/* ---- NVMCTRL (base 0x41004000) ----
 * 64-byte pages, 4-page (256-byte) rows, erase granularity = row. A command is
 * CMD[6:0] | key[15:8] in CTRLA; the page buffer is loaded by plain writes to the
 * main flash address space; ADDR is the word address (byte address >> 1). */
typedef struct
{
    volatile uint16_t CTRLA;  /* +0x00 */
    uint16_t pad0;            /* +0x02 */
    volatile uint16_t CTRLB;  /* +0x04 */
    uint8_t pad1[14];         /* +0x06..0x13 (INTENCLR @0x0C, INTENSET @0x10; rest
                                  reserved) */
    volatile uint8_t INTFLAG; /* +0x14  bit 0 = READY (8-bit INTENCLR 0x0C /
                                  INTENSET 0x10 / STATUS 0x18 / ADDR 0x1C) */
    uint8_t pad2[7];          /* +0x15..0x1B */
    volatile uint32_t ADDR;   /* +0x1C  word address in bits 0..21; must be 32-bit
                                  or bit 16 truncates */
} Samr21Nvmctrl;

#define NVMCTRL_CTRLA_CMD_ER 0x02u  /* erase row at ADDR */
#define NVMCTRL_CTRLA_CMD_WP 0x04u  /* commit page buffer to page at ADDR */
#define NVMCTRL_CTRLA_CMD_PBC 0x44u /* clear page buffer */
#define NVMCTRL_CTRLA_KEY (0xA5u << 8)
#define NVMCTRL_INTFLAG_READY (1u << 0)

/* CTRLB (16-bit at +0x04). RWS[3:0] is a plain wait-state count on the SAMR21 (0..15,
 * not the SAMD21 encoding); 48 MHz needs 2 (the vendor bootloader runs 2, a cold-boot
 * app has RWS 0). */
#define NVMCTRL_CTRLB_RWS_MASK (0xFu << 1)
#define NVMCTRL_CTRLB_RWS(n) (((n) & 0xFu) << 1)
#define NVMCTRL_CTRLB_MANW (1u << 7)
#define NVMCTRL_CTRLB_SLEEPPRM_WAKEONACCESS 0u /* bits [9:8] */
#define NVMCTRL_CTRLB_SLEEPPRM_WAKEUPINSTANT (1u << 8)
#define NVMCTRL_CTRLB_SLEEPPRM_DISABLED (3u << 8)
#define NVMCTRL_CTRLB_READMODE_NO_MISS_PENALTY 0u /* bits [17:16] */
#define NVMCTRL_CTRLB_READMODE_LOW_POWER (1u << 16)
#define NVMCTRL_CTRLB_READMODE_DETERMINISTIC (2u << 16)

/* ---- PORT (base 0x41004400; groups 0..2 at +0x00/+0x80/+0x100) ---- */
typedef struct
{
    volatile uint32_t DIR;      /* +0x00 */
    volatile uint32_t DIRCLR;   /* +0x04 */
    volatile uint32_t DIRSET;   /* +0x08 */
    volatile uint32_t DIRTGL;   /* +0x0C */
    volatile uint32_t OUT;      /* +0x10 */
    volatile uint32_t OUTCLR;   /* +0x14 */
    volatile uint32_t OUTSET;   /* +0x18 */
    volatile uint32_t OUTTGL;   /* +0x1C */
    volatile uint32_t IN;       /* +0x20 */
    volatile uint32_t CTRL;     /* +0x24 */
    volatile uint32_t WRCONFIG; /* +0x28 */
} Samr21PortGroup;

#define SAMR21_PORT_G(n) ((Samr21PortGroup *)((volatile uint8_t *)0x41004400u + (n) * 128u))

#define PORT_PIN(n) (1u << (n))
#define PORT_WRCONFIG_PINMASK(m) ((m) & 0xFFFFu)
#define PORT_WRCONFIG_PMUXEN (1u << 16)
#define PORT_WRCONFIG_INEN (1u << 17)
#define PORT_WRCONFIG_PMUX(p) (((p) & 0xFu) << 24)
#define PORT_WRCONFIG_WRPINCFG (1u << 30)
#define PORT_WRCONFIG_HWSEL (1u << 31)
/* SAMR21: WRPMUX=0 leaves the PMUXn register untouched; the PMUX value
 * is written only when it is set. */
#define PORT_WRCONFIG_WRPMUX (1u << 28)
/* WRCONFIG bit 18 = Pull Enable. Writing it 0 with WRPINCFG set clears the pin's
 * pull, which is what errata 12368 asks for on PA24/PA25 before their alternative
 * function is enabled. */
#define PORT_WRCONFIG_PULLEN (1u << 18)

/* PC16, function F = GCLK_IO1 (AT86RF233 MCLK, 1 MHz). */
#define PORT_MUX_PC16F_GCLK_IO1 5u
/* SERCOM4 pads (function F = 5 on the SAMR21). */
#define PORT_MUX_PC18F_SERCOM4_PAD3 5u
#define PORT_MUX_PC19F_SERCOM4_PAD0 5u
#define PORT_MUX_PB30F_SERCOM4_PAD2 5u
/* PA08/PA09, function F = RFCTRL FECTRL[0]/FECTRL[1] (Table 5-1, samr21.txt:722-725). */
#define PORT_MUX_PA08F_FECTRL0 5u
#define PORT_MUX_PA09F_FECTRL1 5u
/* PA24/PA25, function G = USB DM/DP. */
#define PORT_MUX_PA24G_USB_DM 6u
#define PORT_MUX_PA25G_USB_DP 6u
/* The same pair, function D = SERCOM5 PAD2/PAD3 (Table 5-1; mux C on these pins is
 * SERCOM3). The RaspBee II's host link takes this personality (docs/hardware_samr21.md §9). */
#define PORT_MUX_PA24D_SERCOM5_PAD2 3u
#define PORT_MUX_PA25D_SERCOM5_PAD3 3u

/* ---- SERCOM4 SPI (base 0x42001800) ----
 * The AT86RF233 link (samr21_rf.c); the SAMR21 SERCOMs sit on APB at 0x42001800. */
typedef struct
{
    volatile uint32_t CTRLA;    /* +0x00 */
    volatile uint32_t CTRLB;    /* +0x04 */
    uint8_t pad0[4];            /* +0x08..0x0B */
    volatile uint8_t BAUD;      /* +0x0C */
    uint8_t pad1[7];            /* +0x0D..0x13 */
    uint8_t pad2[2];            /* +0x14..0x15 (INTENCLR/SET) */
    volatile uint8_t INTENSET;  /* +0x16 */
    uint8_t pad3;               /* +0x17 */
    volatile uint8_t INTFLAG;   /* +0x18  write-1-clear */
    uint8_t pad4;               /* +0x19 */
    volatile uint16_t STATUS;   /* +0x1A */
    volatile uint32_t SYNCBUSY; /* +0x1C */
    uint32_t pad5;              /* +0x20..0x23 (Table 26-4: reserved) */
    volatile uint32_t ADDR;     /* +0x24 */
    volatile uint32_t DATA;     /* +0x28 */
} Samr21SercomSpi;

/* SPI register bits; the SAMR21 CTRLA layout differs from the
 * SAMD21's (ENABLE at bit 1, 3-bit MODE at [4:2], SPI master = 0x3). */
#define SERCOM_SPI_CTRLA_SWRST (1u << 0)
#define SERCOM_SPI_CTRLA_ENABLE (1u << 1)
#define SERCOM_SPI_CTRLA_MODE_SPI_MASTER (3u << 2)
#define SERCOM_SPI_CTRLA_DOPO(p) (((p) & 0x3u) << 16)
#define SERCOM_SPI_CTRLA_DIPO(p) (((p) & 0x3u) << 20)
#define SERCOM_SPI_CTRLB_RXEN (1u << 17)
/* CHSIZE[2:0] = 0 (8-bit) is the reset value of the SPI CTRLB. */
#define SERCOM_SPI_INTFLAG_DRE (1u << 0)
#define SERCOM_SPI_INTFLAG_TXC (1u << 1)
#define SERCOM_SPI_INTFLAG_RXC (1u << 2)
#define SERCOM_SPI_SYNCBUSY_SWRST (1u << 0)
#define SERCOM_SPI_SYNCBUSY_ENABLE (1u << 1)
#define SERCOM_SPI_SYNCBUSY_CTRLB (1u << 2)
#define SERCOM_SPI_BAUD_BAUD(b) ((b) & 0xFFu)

/* ---- SERCOM USART (base 0x42001C00 for SERCOM5, Table 10-1) ----
 * The same register window as the SPI view, with the 16-bit BAUD at 0x0C (24.8.3),
 * STATUS at 0x1A (24.8.9) and DATA at 0x28 (24.8.10). */
typedef struct
{
    volatile uint32_t CTRLA;    /* +0x00 */
    volatile uint32_t CTRLB;    /* +0x04 */
    uint8_t pad0[4];            /* +0x08..0x0B */
    volatile uint16_t BAUD;     /* +0x0C */
    volatile uint8_t RXPL;      /* +0x0E */
    uint8_t pad1[5];            /* +0x0F..0x13 (ICTRRL/ICTRRH) */
    volatile uint8_t INTENCLR;  /* +0x14 */
    uint8_t pad2;               /* +0x15 */
    volatile uint8_t INTENSET;  /* +0x16 */
    uint8_t pad3;               /* +0x17 */
    volatile uint8_t INTFLAG;   /* +0x18  write-1-clear */
    uint8_t pad4;               /* +0x19 */
    volatile uint16_t STATUS;   /* +0x1A  write-1-clear for its error bits */
    volatile uint32_t SYNCBUSY; /* +0x1C */
    uint8_t pad5[8];            /* +0x20..0x27 */
    volatile uint16_t DATA;     /* +0x28 */
    uint8_t pad6[6];            /* +0x2A..0x2F */
    volatile uint8_t DBGCTRL;   /* +0x30 */
} Samr21SercomUart;

/* CTRLA layout on this die (24.8.1), which is not the SAMD-family one: MODE[4:2],
 * SAMPR[15:13], TXPO[17:16], RXPO[21:20], FORM[27:24], CMODE 28, DORD 30. */
#define SERCOM_UART_CTRLA_SWRST (1u << 0)
#define SERCOM_UART_CTRLA_ENABLE (1u << 1)
#define SERCOM_UART_CTRLA_MODE_EXT_CLK (0u << 2)
#define SERCOM_UART_CTRLA_MODE_INT_CLK (1u << 2) /* 24.8.1 MODE = 0x1 */
#define SERCOM_UART_CTRLA_RUNSTDBY (1u << 7)
#define SERCOM_UART_CTRLA_SAMPR_16X (0u << 13)   /* 24.8.1 Table 24-10: 16x, arithmetic */
#define SERCOM_UART_CTRLA_DORD_LSB (1u << 30)    /* 1 = LSB first: the UART convention */
#define SERCOM_UART_CTRLA_TXPO(p) (((p) & 0x3u) << 16) /* Table 24-9: 0x1 = TxD on PAD2 */
#define SERCOM_UART_CTRLA_RXPO(p) (((p) & 0x3u) << 20) /* Table 24-8: the RxD pad number */
#define SERCOM_UART_CTRLA_FORM_USART (0u << 24)       /* Table 24-6: no parity */

/* CTRLB (24.8.2): CHSIZE[2:0] = 0 (8-bit), SBMODE 0 (one stop bit), PMODE 0, ENC 0 are
 * the reset values and are written as such. */
#define SERCOM_UART_CTRLB_TXEN (1u << 16)
#define SERCOM_UART_CTRLB_RXEN (1u << 17)

/* INTENSET/INTENCLR/INTFLAG share the layout (24.8.5-24.8.7). */
#define SERCOM_UART_INT_DRE (1u << 0)
#define SERCOM_UART_INT_TXC (1u << 1)
#define SERCOM_UART_INT_RXC (1u << 2)
#define SERCOM_UART_INT_ERROR (1u << 7)

/* STATUS (24.8.9): each error bit is cleared by writing a one, and reading it before
 * DATA reports the status of the character that read takes out. */
#define SERCOM_UART_STATUS_PERR (1u << 0)
#define SERCOM_UART_STATUS_FERR (1u << 1)
#define SERCOM_UART_STATUS_BUFOVF (1u << 2)

#define SERCOM_UART_SYNCBUSY_SWRST (1u << 0)
#define SERCOM_UART_SYNCBUSY_ENABLE (1u << 1)
#define SERCOM_UART_SYNCBUSY_CTRLB (1u << 2)

/* ---- USB (base 0x41005000) ---- */
typedef struct
{
    volatile uint8_t EPCFG;       /* +0x00 */
    uint8_t pad0[3];              /* +0x01..0x03 */
    volatile uint8_t EPSTATUSCLR; /* +0x04 */
    volatile uint8_t EPSTATUSSET; /* +0x05 */
    volatile uint8_t EPSTATUS;    /* +0x06 */
    volatile uint8_t EPINTFLAG;   /* +0x07 */
    volatile uint8_t EPINTENCLR;  /* +0x08 */
    volatile uint8_t EPINTENSET;  /* +0x09 */
} UsbEndpoint;

/* Register gaps are mandatory (datasheet 29.6): every offset after QOSCTRL depends
 * on them - a pad short by one byte moves CTRLB onward. */
typedef struct
{
    volatile uint8_t CTRLA;      /* +0x00 */
    volatile uint8_t pad1;       /* +0x01 */
    volatile uint8_t SYNCBUSY;   /* +0x02 */
    volatile uint8_t QOSCTRL;    /* +0x03 */
    volatile uint8_t pad2[4];    /* +0x04..0x07 */
    volatile uint8_t CTRLB;      /* +0x08 */
    volatile uint8_t pad3;       /* +0x09 */
    volatile uint8_t DADD;       /* +0x0A */
    volatile uint8_t pad4;       /* +0x0B */
    volatile uint8_t STATUS;     /* +0x0C */
    volatile uint8_t FSMSTATUS;  /* +0x0D */
    volatile uint8_t pad5[2];    /* +0x0E..0x0F */
    volatile uint16_t FNUM;      /* +0x10 */
    volatile uint16_t pad6;      /* +0x12 */
    volatile uint16_t INTENCLR;  /* +0x14 */
    volatile uint16_t pad7;      /* +0x16 */
    volatile uint16_t INTENSET;  /* +0x18 */
    volatile uint16_t pad8;      /* +0x1A */
    volatile uint16_t INTFLAG;   /* +0x1C */
    volatile uint16_t pad9;      /* +0x1E */
    volatile uint16_t EPINTSMRY; /* +0x20 */
    volatile uint16_t pad10;     /* +0x22 */
    volatile uint32_t DESCADD;   /* +0x24 */
    volatile uint16_t PADCAL;    /* +0x28 */
} Samr21Usb;

/* Endpoint n registers at +0x100 + 0x20*n (out of the struct to keep it compact). */
#define SAMR21_USB_EP(n) ((UsbEndpoint *)((volatile uint8_t *)0x41005000u + 0x100u + (n) * 32u))

#define USB_CTRLA_SWRST (1u << 0)
#define USB_CTRLA_ENABLE (1u << 1)
#define USB_CTRLA_RUNSTDBY (1u << 2)
#define USB_CTRLA_MODE_HOST (1u << 7) /* device mode = 0 */

#define USB_CTRLB_DETACH (1u << 0)
#define USB_CTRLB_UPRSM (1u << 1)
#define USB_CTRLB_SPDCONF_FS 0u /* bits [3:2] = 0 */

#define USB_INTFLAG_SUSPEND (1u << 0)
#define USB_INTFLAG_SOF (1u << 2)
#define USB_INTFLAG_EORST (1u << 3)
#define USB_INTFLAG_WAKEUP (1u << 4)
/* INTFLAG/INTENSET/INTENCLR layout, 29.8.4 (samr21.txt:36637, :37513): bit 5
 * EORSM, bit 6 UPRSM. */
#define USB_INTFLAG_EORSM (1u << 5)
#define USB_INTFLAG_UPRSM (1u << 6)

#define USB_DADD_ADDEN (1u << 7)
#define USB_DADD_DADD(a) ((a) & 0x7Fu)

/* SAMR21 layout: BYTE_COUNT[13:0], MULTI_PACKET_SIZE[27:14], SIZE[30:28];
 * SIZE = 8 << SIZE, so 3 is the 64 B full-speed max packet. */
#define USB_PCKSIZE_SIZE(s) (((s) & 0x7u) << 28)
#define USB_PCKSIZE_BYTE_COUNT(b) ((b) & 0x3FFFu)
#define USB_PCKSIZE_MULTI_PACKET_SIZE(m) (((m) & 0x3FFFu) << 14)

#define USB_EPCFG_EPTYPE0(t) ((t) & 0x7u)        /* OUT, 3-bit */
#define USB_EPCFG_EPTYPE1(t) (((t) & 0x7u) << 4) /* IN,  3-bit */

#define USB_EPSTATUS_DTGLOUT (1u << 0)
#define USB_EPSTATUS_DTGLIN (1u << 1)
#define USB_EPSTATUS_STALLRQ0 (1u << 4)
#define USB_EPSTATUS_STALLRQ1 (1u << 5)
#define USB_EPSTATUS_BK0RDY (1u << 6)
#define USB_EPSTATUS_BK1RDY (1u << 7)

#define USB_EPINT_TRCPT0 (1u << 0)
#define USB_EPINT_TRCPT1 (1u << 1)
#define USB_EPINT_TRFAIL0 (1u << 2)
#define USB_EPINT_TRFAIL1 (1u << 3)
#define USB_EPINT_RXSTP (1u << 4)
/* EPINTFLAG / EPINTENSET layout, 29.6.8 (samr21.txt:36691, :38100-38117):
 * STALL0 bit 5, STALL1 bit 6 - set when the hardware answers with a STALL. */
#define USB_EPINT_STALL0 (1u << 5)
#define USB_EPINT_STALL1 (1u << 6)

/* SRAM endpoint descriptor bank entry: 16 bytes, so EP0 IN sits at
 * +0x10 in the bank array. */
typedef struct
{
    volatile uint32_t ADDR;     /* +0x00 */
    volatile uint32_t PCKSIZE;  /* +0x04 */
    volatile uint16_t EXTREG;   /* +0x08 */
    volatile uint8_t STATUS_BK; /* +0x0A */
    uint8_t pad[5];             /* +0x0B..0x0F */
} UsbDescBank;

/* ---- Instance addresses ---- */
#define SAMR21_PM ((Samr21Pm *)0x40000400u)
#define SAMR21_SYSCTRL ((Samr21Sysctrl *)0x40000800u)
#define SAMR21_GCLK ((Samr21Gclk *)0x40000C00u)
#define SAMR21_WDT ((Samr21Wdt *)0x40001000u)
#define SAMR21_DSU ((Samr21Dsu *)SAMR21_DSU_BASE)
#define SAMR21_NVMCTRL ((Samr21Nvmctrl *)0x41004000u)
#define SAMR21_SERCOM4 ((Samr21SercomSpi *)0x42001800u)
#define SAMR21_SERCOM5 ((Samr21SercomUart *)0x42001C00u) /* Table 10-1 */
#define SAMR21_USB ((Samr21Usb *)0x41005000u)

/* ---- Fuses: the word at 0x806024 (NVMCTRL_OTP4 + 4) holds:
 * USB TRANSN [17:13], TRANSP [22:18], TRIM [25:23], DFLL48M coarse
 * cal [31:26]. ---- */
#define FUSES_OTP4_WORD (*(volatile const uint32_t *)0x00806024u)
/* The DFLL48M fine-cal fuse (bits [15:0] of that word) lives in its own
 * word, separate from the coarse/USB word. */
#define FUSES_DFLL_FINE_WORD (*(volatile const uint32_t *)0x00806028u)
#define FUSE_DFLL_FINE(f) ((f) & 0x03FFu)         /* [15:0] */
#define FUSE_DFLL_COARSE(c) (((c) >> 26) & 0x3Fu) /* [31:26] */
/* OSC32K cal (7 bits): the BTL reads it from bits [12:6] of the same
 * OTP4+4 word (FUSES_OSC32K_CAL in the vendor nvmctrl.h). */
#define FUSE_OSC32K_CAL(f) (((f) >> 6) & 0x7Fu)  /* [12:6] */
#define FUSE_USB_TRANSN(f) (((f) >> 13) & 0x1Fu) /* [17:13] */
#define FUSE_USB_TRANSP(f) (((f) >> 18) & 0x1Fu) /* [22:18] */
#define FUSE_USB_TRIM(f) (((f) >> 23) & 0x7u)    /* [25:23] */

/* ---- PADCAL layout (29.8.1.6): TRANSP[4:0], TRANSN[10:6], TRIM[14:12]; bits 5
 * and 11 reserved. ---- */
#define USB_PADCAL_TRANSP(v) ((v) & 0x1Fu)
#define USB_PADCAL_TRANSN(v) (((v) & 0x1Fu) << 6)
#define USB_PADCAL_TRIM(v) (((v) & 0x7u) << 12)

/* QOSCTRL: CQOS bits [1:0], DQOS bits [3:2]; the deCONZ bootloader on
 * this device uses CQOS=2, DQOS=2. */
#define USB_QOSCTRL_CQOS(q) ((q) & 0x3u)
#define USB_QOSCTRL_DQOS(q) (((q) & 0x3u) << 2)

/* Standard FEATURE endpoint halt (wValue for SET/CLEAR_FEATURE). */
#define USB_FEATURE_ENDPOINT_HALT 0x00u

/* ---- System peripherals (Cortex-M) ---- */
typedef struct
{
    volatile uint32_t CTRL;  /* +0x00 */
    volatile uint32_t LOAD;  /* +0x04 */
    volatile uint32_t VAL;   /* +0x08 */
    volatile uint32_t CALIB; /* +0x0C */
} Samr21Systick;
/* TC in 32-bit mode, the one this tree uses: TC4 the master of the pair. */
typedef struct
{
    volatile uint16_t CTRLA;   /* +0x00 */
    volatile uint16_t READREQ; /* +0x02 */
    uint8_t reserved0[11];
    volatile uint8_t STATUS;   /* +0x0F */
    volatile uint32_t COUNT;   /* +0x10 */
} Samr21Tc32;

#define SAMR21_TC4 ((Samr21Tc32 *)0x42003000u)
#define TC_CTRLA_ENABLE (1u << 1)
#define TC_CTRLA_MODE_COUNT32 (2u << 2)
#define TC_READREQ_RREQ (1u << 15)
#define TC_READREQ_RCONT (1u << 14)
#define TC_READREQ_ADDR_COUNT 0x10u
#define TC_STATUS_SYNCBUSY (1u << 7)

#define SAMR21_SYSTICK ((Samr21Systick *)0xE000E010u)
#define SYSTICK_CTRL_ENABLE (1u << 0)
#define SYSTICK_CTRL_TICKINT (1u << 1)
#define SYSTICK_CTRL_CLKSRC (1u << 2) /* processor clock */
/* LOAD/COUNT are 24-bit: one stopwatch period is (LOAD + 1) cycles. The pump reads
 * the period from the register, because Clock_Init re-programs LOAD to the 1 ms
 * cadence half way through bring-up. */
#define SYSTICK_COUNT_MASK 0xFFFFFFu

/* NVIC (Cortex-M0+): ISER at 0xE000E100, ICER at 0xE000E180
 * (0xE000ED00 is the SCB/CPUID base — writes there are ignored). */
#define NVIC_ISER0 (*(volatile uint32_t *)0xE000E100u)
#define NVIC_ICER0 (*(volatile uint32_t *)0xE000E180u)
#define NVIC_ICER1 (*(volatile uint32_t *)0xE000E184u)

#define NVIC_ISPR0 (*(volatile uint32_t *)0xE000E200u)
#define NVIC_ICPR0 (*(volatile uint32_t *)0xE000E280u)
/* IPR0..IPR7 hold four lines each, one byte per line, and only bits 7:6 of a byte are
 * implemented (ARMv6-M: word access only, two priority bits). */
#define NVIC_IPR(n) (*(volatile uint32_t *)(0xE000E400u + 4u * (n)))
#define NVIC_SET_PRIO(line, prio)                                                                  \
    (NVIC_IPR((line) >> 2) = (NVIC_IPR((line) >> 2) & ~(0xFFuL << (8u * ((line) & 0x3u)))) |       \
                             ((uint32_t)((prio) & 0x3u) << (8u * ((line) & 0x3u) + 6u)))
/* SHPR3 bits 31:30 are SysTick's priority. */
#define SCB_SHPR3 (*(volatile uint32_t *)0xE000ED20u)
#define SCB_SHPR3_SYSTICK_MASK (0xFFuL << 24)
#define SCB_SHPR3_SYSTICK(prio) ((uint32_t)((prio) & 0x3u) << 30)

/* ---- IRQ numbers (SAMR21) ---- */
#define IRQN_EIC 4
#define IRQN_USB 7
#define IRQN_SERCOM5 14 /* Table 10-1: SERCOM5 IRQ line */

/* ---- EIC (base 0x40001800, samr21 datasheet Table 19-3) ----
 * Four byte registers, then one 32-bit slot each for EVCTRL, INTENCLR, INTENSET, INTFLAG,
 * WAKEUP and CONFIG0..1. CONFIGn holds eight lines at four bits each: SENSE[2:0] and FILTEN in
 * bit 3. The AT86RF233's IRQ is PB00 = EXTINT0 (function A). */
typedef struct
{
    volatile uint8_t CTRL;       /* +0x00 ENABLE bit1, SWRST bit0 */
    volatile uint8_t STATUS;     /* +0x01 SYNCBUSY bit7 */
    volatile uint8_t NMICTRL;    /* +0x02 */
    volatile uint8_t NMIFLAG;    /* +0x03 */
    volatile uint32_t EVCTRL;    /* +0x04 */
    volatile uint32_t INTENCLR;  /* +0x08 */
    volatile uint32_t INTENSET;  /* +0x0C */
    volatile uint32_t INTFLAG;   /* +0x10 write 1 to clear */
    volatile uint32_t WAKEUP;    /* +0x14 */
    volatile uint32_t CONFIG[2]; /* +0x18, +0x1C */
} Samr21Eic;

#define SAMR21_EIC ((Samr21Eic *)0x40001800u)
#define EIC_CTRL_SWRST (1u << 0)
#define EIC_CTRL_ENABLE (1u << 1)
#define EIC_STATUS_SYNCBUSY (1u << 7)
#define EIC_SENSE_NONE 0u
#define EIC_SENSE_RISE 1u
#define EIC_SENSE_HIGH 4u
#define EIC_CONFIG_SENSE(line, v) (((uint32_t)(v) & 0x7u) << (4u * ((line) & 0x7u)))
#define EIC_CONFIG_LINE_MASK(line) (0xFuL << (4u * ((line) & 0x7u)))
#define EIC_LINE_RF 0u

/* PORT function A on PB00 is EIC EXTINT0 (samr21 datasheet 7.1 multiplexing table). */
#define PORT_MUX_PB00A_EIC_EXTINT0 0u

/* ---- Flash map (see atsamr21.ld) ---- */
#define FLASH_APP_VECTORS 0x00005000u
#define FLASH_APP_CODE 0x00009200u
#define FLASH_NVRAM_START 0x0003EF00u /* image must end before this */
#define RAM_START 0x20000000u
#define RAM_SIZE 0x8000u

/* ---- RFCTRL (base 0x42005400): the transceiver's DIG1..4 routed to FECTRL[0..5] ----
 * FECTRL holds one 2-bit source per output: 0 DIG1, 1 DIG2, 2 DIG3, 3 DIG4 (samr21.txt
 * Table 33-1). */
#define SAMR21_RFCTRL_FECTRL ((volatile uint16_t *)0x42005400u)
#define RFCTRL_FECTRL_F0_DIG4 (3u << 0)
#define RFCTRL_FECTRL_F1_DIG3 (2u << 2)

#endif
