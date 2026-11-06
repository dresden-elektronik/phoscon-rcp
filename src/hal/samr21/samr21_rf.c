/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* AT86RF233 wake + CLKM rate (SAMR21, CB2).
 *
 * CLKM is the DFLL48M reference: PC16/GCLKIN, divided by 250 in generator 1. The
 * vendor's released app puts CLKM at 8 MHz (CLK_CTRL_8MHZ = 4) paired with GENDIV
 * 250 / MUL 1500 = 48 MHz (docs/hardware_samr21.md §2); this matches
 * that pair. The wake is required because the RF is VBUS-powered and survives an
 * MCU /RST: it is either in SLEEP (SLP_TR floats High, CLKM off after 35 CLKM
 * cycles - at86rf233 7.1.2.2, at86rf233.txt:4168-4173) or awake with a shadowed
 * CLKM rate (9.6.4 note 1: CLKM_CTRL keeps the pad rate but reads back the reset
 * value 1), so the rate must be written, not trusted.
 *
 * Rf_WakeAndForceClkm() proves the pad has edges: PART_NUM 0x1C answers 0x0B,
 * TRX_STATUS holds 0x08 over four consecutive reads, TRX_CTRL_0 reads back the
 * 0x04 written (docs/hardware_samr21.md §6). Only then may Clock_Init point
 * generator 1 at the CLKM pad.
 *
 * Datasheet refs: at86rf233.txt 6.3/Table 6-2, 6.5 (PART_NUM :2780, Table 6-5
 * :2844-2854), 9.6.4 (Tables 9-18/9-19, note 1 :13881-13895), 7.1.2 (Table 7-3
 * :5328-5412), 12.4 (t10, t11, tXTAL, fasync <= 7.5 MHz); samr21.txt 35.5.1.1
 * (:44582-44585, Table 35-4 :44597-44601), Table 38-19 (:49993-50010). */
#include <stdint.h>

#include "hal.h"
#include "hal_diag.h"
#include "dbg.h"
#include "samr21_regs.h"
#include "samr21_rf.h"

/* AT86RF233 register addresses (at86rf233.txt: TRX_STATUS 0x01 :5253, TRX_STATE
 * 0x02 :5429, TRX_CTRL_0 0x03 :13899). PART_NUM is 0x1C, not 0x00: "Register 0x1C
 * (PART_NUM)" :2780 with Table 6-5 :2844-2854, and samr21.txt:44582-44585, Table
 * 35-4 :44597-44601. No register is documented at 0x00 (6.3.1, :2078-2081), so
 * 0x00 is not an identity probe. */
#define RF_REG_TRX_STATUS 0x01u
#define RF_REG_TRX_STATE 0x02u
#define RF_REG_TRX_CTRL_0 0x03u
#define RF_REG_PART_NUM 0x1Cu
/* IRQ_STATUS: at86rf233.txt:3267-3270, bit map :3284-3320 (3 TRX_END, 6 TRX_UR). */
#define RF_REG_IRQ_STATUS 0x0Fu
#define RF_REG_TRX_CTRL_1 0x04u
#define RF_REG_PHY_RSSI 0x06u
#define RF_REG_PHY_ED_LEVEL 0x07u
#define RF_REG_PHY_CC_CCA 0x08u
#define RF_REG_PHY_TX_PWR 0x05u
#define RF_REG_IRQ_MASK 0x0Eu
#define RF_REG_TRX_CTRL_2 0x0Cu
#define RF_REG_SHORT_ADDR_0 0x20u
#define RF_REG_PAN_ID_0 0x22u
#define RF_REG_IEEE_ADDR_0 0x24u
#define RF_REG_XAH_CTRL_0 0x2Cu

#define RF_TRX_CMD_OFF 0x08u    /* TRX_STATE: clock state */
#define RF_TRX_STATUS_OFF 0x08u /* TRX_STATUS[4:0] */
#define RF_TRX_STATUS_MASK 0x1Fu
#define RF_PART_NUM_AT86RF233 0x0Bu /* Table 6-5 / Table 35-4 */
#define RF_TRX_STATUS_TRANS 0x1Fu   /* state transition in progress, Table 7-3 */

/* SPI command byte (datasheet Table 6-2 / 6.3): bit 7 = register access,
 * bit 6 = write, bits [5:0] = 6-bit register address. */
#define RF_SPI_CMD_READ(addr) (0x80u | (addr))
#define RF_SPI_CMD_WRITE(addr) (0xC0u | (addr))

/* Frame-buffer access has its own command bytes, auto-incrementing from the frame
 * buffer's start (Table 6-2, at86rf233.txt:1984-2065;
 * the OpenThread SAMR21 port's src/HAL/include/samr21At86rf233.h). */
#define RF_SPI_CMD_FRAMEBUFFER_READ 0x20u
#define RF_SPI_CMD_FRAMEBUFFER_WRITE 0x60u
#define RF_SPI_CMD_SRAM_READ 0x00u
#define RF_SPI_CMD_SRAM_WRITE 0x40u

/* The AES engine's SRAM addresses and AES_CTRL values (at86rf233 datasheet 11.1). The first data octet
 * of a read arrives two octets, 4 us, after /SEL, which is part of the block's 24 us. */
#define RF_SRAM_AES_CTRL 0x83u
#define RF_SRAM_AES_STATE 0x84u
#define RF_AES_MODE_ECB 0x00u
#define RF_AES_MODE_KEY 0x10u
#define RF_AES_REQUEST 0x80u
#define RF_AES_WAIT_CYCLES (21u * 48u)

/* TRX_CTRL_0 = CLKM_CTRL[2:0] 4 (8 MHz, Table 9-19 :13985-14000) + CLKM_SHA_SEL 0
 * (bit 3, rate change immediate, Table 9-18) + PMU_EN/TOM_EN 0. Bit map: 7 TOM_EN,
 * 6 reserved, 5 PMU_EN, 4 PMU_IF_INVERSE, 3 CLKM_SHA_SEL, 2:0 CLKM_CTRL
 * (at86rf233.txt:13915-13938). Vendor CLK_CTRL_8MHZ = 4, at86rf233.h:203. */
#define RF_TRX_CTRL_0_8MHZ 0x04u

/* SCLK = gen0 / (2 * (BAUD + 1)) = 8 MHz / 4 = 2 MHz, inside the 7.5 MHz async
 * limit. */
#define RF_SPI_BAUD 1u

/* SERCOM4 as SPI master, RX enabled, CHSIZE 8-bit (reset value). Shared by
 * rfSercomInit() and Rf_SetSpiBaud(): BAUD is Enable-Protected
 * (samr21.txt:22999-23002), so a rate change re-writes this whole word. */
#define RF_SERCOM_SPI_CTRLA                                                                        \
    (SERCOM_SPI_CTRLA_ENABLE | SERCOM_SPI_CTRLA_MODE_SPI_MASTER | SERCOM_SPI_CTRLA_DOPO(1) |       \
     SERCOM_SPI_CTRLA_DIPO(0))

/* Set once the pins and SERCOM4 are up: a GCLK re-route in the wrong state stalls
 * the core (errata 10416). */
static int rfSercomReady;

/* One SPI frame is 4 us at 2 MHz; the bound only guards a stuck SERCOM. */
#define RF_SPI_FRAME_LIMIT 0x400u

/* TRX_STATUS poll budget (~10 us/transaction => < 1 s). */
#define RF_STATUS_WAIT_LIMIT 0x8000u

/* Consecutive identical reads required before a state counts as held. One match
 * proves nothing: MISO (PC19) is a floating input unless /SEL is low (6.2 note 1,
 * at86rf233.txt:1960-1963), so a dead line can supply one byte. Four identical
 * reads across three addresses with three different values cannot. */
#define RF_PROOF_STATE_IDENTICAL 4u

/* Write-then-read-back attempts; each accepted read-back follows its own write. */
#define RF_PROOF_SETTING_ATTEMPTS 2u

/* SERCOM4/GCLK sync-wait budget: ~80 ms at 8 MHz, far above the us-scale sync
 * delays and far below the ~8 s watchdog. */
#define RF_SYNC_WAIT_LIMIT 0x10000u

/* Busy-wait granularity on the 8 MHz fallback tree: one step is ~4 CPU cycles, so
 * the factor keeps the delay >= the nominal ms. */
#define RF_DELAY_UNITS_PER_MS 2500u

static void rfDelayMs(uint32_t ms)
{
    uint32_t n = ms * RF_DELAY_UNITS_PER_MS;

    while (n > 0)
    {
        /* Feed the ALWAYSON WDT across the ~5 ms wait. */
        if ((n & 0x1FFu) == 0)
        {
            Clock_BringupPump();
            Wdt_Feed();
        }

        --n;
    }
}

/* RF pins. SLP_TR must go Low before the reset pulse: after an MCU /RST it floats
 * High and the RF sleeps. PINMASK is relative to the HWSEL half-word, so pins
 * 16-31 shift right by 16. */
static void rfPinInit(void)
{
    Samr21PortGroup *g0 = SAMR21_PORT_G(0);
    Samr21PortGroup *g1 = SAMR21_PORT_G(1);
    Samr21PortGroup *g2 = SAMR21_PORT_G(2);

    /* SLP_TR (PA20) out Low = wake / keep awake. */
    g0->DIRSET = PORT_PIN(20);
    g0->WRCONFIG =
        PORT_WRCONFIG_HWSEL | PORT_WRCONFIG_WRPINCFG | PORT_WRCONFIG_PINMASK(PORT_PIN(20) >> 16);
    g0->OUTCLR = PORT_PIN(20);

    /* /RST (PB15) out, de-asserted (rfReset pulses it Low); lower half-word. */
    g1->DIRSET = PORT_PIN(15);
    g1->WRCONFIG = PORT_WRCONFIG_WRPINCFG | PORT_WRCONFIG_PINMASK(PORT_PIN(15));
    g1->OUTSET = PORT_PIN(15);

    /* /SEL (PB31) GPIO out, High = deselected; not a SERCOM pin. */
    g1->DIRSET = PORT_PIN(31);
    g1->WRCONFIG =
        PORT_WRCONFIG_HWSEL | PORT_WRCONFIG_WRPINCFG | PORT_WRCONFIG_PINMASK(PORT_PIN(31) >> 16);
    g1->OUTSET = PORT_PIN(31);

    /* MOSI (PB30) = SERCOM4 PAD2, SCLK (PC18) = SERCOM4 PAD3,
   * MISO (PC19) = SERCOM4 PAD0 (input). WRPMUX is required for the
   * PMUX value to be written at all (SAMR21 PORT WRCONFIG). */
    g1->DIRSET = PORT_PIN(30);
    g1->WRCONFIG = PORT_WRCONFIG_HWSEL | PORT_WRCONFIG_WRPINCFG | PORT_WRCONFIG_WRPMUX |
                   PORT_WRCONFIG_PMUX(PORT_MUX_PB30F_SERCOM4_PAD2) | PORT_WRCONFIG_PMUXEN |
                   PORT_WRCONFIG_PINMASK(PORT_PIN(30) >> 16);

    g2->DIRSET = PORT_PIN(18);
    g2->WRCONFIG = PORT_WRCONFIG_HWSEL | PORT_WRCONFIG_WRPINCFG | PORT_WRCONFIG_WRPMUX |
                   PORT_WRCONFIG_PMUX(PORT_MUX_PC18F_SERCOM4_PAD3) | PORT_WRCONFIG_PMUXEN |
                   PORT_WRCONFIG_PINMASK(PORT_PIN(18) >> 16);

    g2->DIRCLR = PORT_PIN(19);
    g2->WRCONFIG = PORT_WRCONFIG_HWSEL | PORT_WRCONFIG_WRPINCFG | PORT_WRCONFIG_WRPMUX |
                   PORT_WRCONFIG_PMUX(PORT_MUX_PC19F_SERCOM4_PAD0) | PORT_WRCONFIG_INEN |
                   PORT_WRCONFIG_PMUXEN | PORT_WRCONFIG_PINMASK(PORT_PIN(19) >> 16);
}

/* SERCOM4 as SPI master: APBC gate plus the core clock channel at gen0 = OSC8M -
 * the 12 MHz gen4 the RF SPI normally uses needs the DFLL this module enables.
 * 8-bit MSB-first, MOSI/SCLK on PAD2/PAD3 (DOPO=1), MISO on PAD0 (DIPO=0).
 *
 * Every wait is bounded and WDT-fed: a SYNCBUSY that never clears means the clock
 * routing has no edges. A stuck GCLK sync is the errata-10416 case - reading STATUS
 * is allowed, writing would stall the bus (samr21.txt:4782-4783) - so a failed
 * rfGclkIdle aborts instead of issuing a GCLK write that could freeze the core. */
static int rfGclkIdle(uint16_t rsn)
{
    uint32_t n;

    for (n = 0; n < RF_SYNC_WAIT_LIMIT; ++n)
    {
        if ((SAMR21_GCLK->STATUS & GCLK_STATUS_SYNCBUSY) == 0u)
            return 1;

        Clock_BringupPump();
        Wdt_FeedPoll(n);
    }

    DBG(DBG_COMP_RF, rsn);
    return 0;
}

static int rfSercomSyncClear(uint32_t mask, uint16_t rsn)
{
    uint32_t n;

    for (n = 0; n < RF_SYNC_WAIT_LIMIT; ++n)
    {
        if ((SAMR21_SERCOM4->SYNCBUSY & mask) == 0u)
            return 1;

        Clock_BringupPump();
        Wdt_FeedPoll(n);
    }

    DBG(DBG_COMP_RF, rsn);
    return 0;
}

/* SWRST takes two bits: SYNCBUSY and the self-clearing CTRLA reset itself. */
static int rfSercomSwrstClear(void)
{
    uint32_t n;

    for (n = 0; n < RF_SYNC_WAIT_LIMIT; ++n)
    {
        if ((SAMR21_SERCOM4->CTRLA & SERCOM_SPI_CTRLA_SWRST) == 0u &&
            (SAMR21_SERCOM4->SYNCBUSY & SERCOM_SPI_SYNCBUSY_SWRST) == 0u)
            return 1;

        Clock_BringupPump();
        Wdt_FeedPoll(n);
    }

    DBG(DBG_COMP_RF, DBG_RF_RSN_SWRST);
    return 0;
}

static void rfSercomInit(void)
{
    SAMR21_PM->APBCMASK |= PM_APBCMASK_SERCOM4;

    if (!rfGclkIdle(DBG_RF_RSN_GCLK_SYNC))
        return;

    SAMR21_GCLK->CLKCTRL =
        GCLK_CLKCTRL_ID(GCLK_CLKCTRL_ID_SERCOM4_CORE) | GCLK_CLKCTRL_GEN(0) | GCLK_CLKCTRL_CLKEN;

    SAMR21_SERCOM4->CTRLA = SERCOM_SPI_CTRLA_SWRST;
    (void)rfSercomSwrstClear();

    /* Receiver on (full-duplex reads); CHSIZE 8-bit is the reset value. */
    SAMR21_SERCOM4->CTRLB = SERCOM_SPI_CTRLB_RXEN;
    (void)rfSercomSyncClear(SERCOM_SPI_SYNCBUSY_CTRLB, DBG_RF_RSN_CTRLB);

    SAMR21_SERCOM4->BAUD = RF_SPI_BAUD;

    SAMR21_SERCOM4->CTRLA = RF_SERCOM_SPI_CTRLA;

    if (rfSercomSyncClear(SERCOM_SPI_SYNCBUSY_ENABLE, DBG_RF_RSN_ENABLE))
        rfSercomReady = 1;
}

/* One full-duplex byte: wait for the TX buffer to drain, then shift tx out on MOSI
 * and capture MISO on the same 8 SCLK edges. */
static int rfSpiTransceiveByte(uint8_t tx, uint8_t *rx)
{
    uint32_t n = RF_SPI_FRAME_LIMIT;

    while (!(SAMR21_SERCOM4->INTFLAG & SERCOM_SPI_INTFLAG_DRE))
    {
        if (--n == 0)
            return 0;
    }

    SAMR21_SERCOM4->DATA = tx;

    n = RF_SPI_FRAME_LIMIT;

    while (!(SAMR21_SERCOM4->INTFLAG & SERCOM_SPI_INTFLAG_RXC))
    {
        if (--n == 0)
            return 0;
    }

    *rx = (uint8_t)SAMR21_SERCOM4->DATA;
    return 1;
}

/* A run of n received octets, clocked with zeros. The transmitter is single-buffered and the
 * receiver two-level (samr21 datasheet 25.6.1), so one octet waits in DATA while the previous
 * one shifts and SCLK never pauses between octets; at most two are ever unread. */
static int rfSpiBurstRead(uint8_t *dst, uint16_t n)
{
    uint16_t sent = 0;
    uint16_t got = 0;
    uint32_t guard = (uint32_t)RF_SPI_FRAME_LIMIT * (uint32_t)(n + 1u);

    while (got < n)
    {
        uint8_t flags = SAMR21_SERCOM4->INTFLAG;

        if (sent < n && (uint16_t)(sent - got) < 2u && (flags & SERCOM_SPI_INTFLAG_DRE))
        {
            SAMR21_SERCOM4->DATA = 0x00u;
            sent++;
        }

        if (flags & SERCOM_SPI_INTFLAG_RXC)
            dst[got++] = (uint8_t)SAMR21_SERCOM4->DATA;

        if (--guard == 0u)
            return 0;
    }

    return 1;
}

/* The burst read's other direction: SCLK never pauses, and what comes back is dropped. */
static int rfSpiBurstWrite(const uint8_t *src, uint16_t n)
{
    uint16_t sent = 0;
    uint16_t got = 0;
    uint32_t guard = (uint32_t)RF_SPI_FRAME_LIMIT * (uint32_t)(n + 1u);

    while (got < n)
    {
        uint8_t flags = SAMR21_SERCOM4->INTFLAG;

        if (sent < n && (uint16_t)(sent - got) < 2u && (flags & SERCOM_SPI_INTFLAG_DRE))
            SAMR21_SERCOM4->DATA = src[sent++];

        if (flags & SERCOM_SPI_INTFLAG_RXC)
        {
            (void)SAMR21_SERCOM4->DATA;
            got++;
        }

        if (--guard == 0u)
            return 0;
    }

    return 1;
}

/* One 16-bit transaction under /SEL: command byte then data byte. MISO byte 0
 * carries the TRX_STATUS echo (datasheet 6.4) and is dropped. */
static int rfSpiRegWrite(uint8_t addr, uint8_t data)
{
    Samr21PortGroup *g1 = SAMR21_PORT_G(1);
    uint8_t status;

    g1->OUTCLR = PORT_PIN(31);

    if (!rfSpiTransceiveByte(RF_SPI_CMD_WRITE(addr), &status) ||
        !rfSpiTransceiveByte(data, &status))
    {
        g1->OUTSET = PORT_PIN(31);
        return 0;
    }

    g1->OUTSET = PORT_PIN(31);
    return 1;
}

static int rfSpiRegRead(uint8_t addr, uint8_t *value)
{
    Samr21PortGroup *g1 = SAMR21_PORT_G(1);
    uint8_t status;

    g1->OUTCLR = PORT_PIN(31);

    if (!rfSpiTransceiveByte(RF_SPI_CMD_READ(addr), &status) || !rfSpiTransceiveByte(0x00u, value))
    {
        g1->OUTSET = PORT_PIN(31);
        return 0;
    }

    g1->OUTSET = PORT_PIN(31);
    return 1;
}

/* /RST pulse (t10 >= 625 ns, 1 ms here) puts the RF in TRX_OFF from any state; the
 * xtal settles within tXTAL <= 1 ms before the first SPI access. */
static void rfReset(void)
{
    Samr21PortGroup *g1 = SAMR21_PORT_G(1);

    g1->OUTCLR = PORT_PIN(15);
    rfDelayMs(1);
    g1->OUTSET = PORT_PIN(15);
    rfDelayMs(4);
}

/* ---- The three wake proofs (docs/hardware_samr21.md §6) ----
 * Proofs read bytes the transceiver put on the wire, not our own SERCOM flags; a
 * failure names itself (DBG_RF_RSN_PROOF_*). */

/* Proof 1 - identity: PART_NUM (0x1C) answers 0x0B (at86rf233.txt:2780, Table 6-5
 * :2844-2854; samr21.txt:44582-44585, Table 35-4 :44597-44601). A write that never
 * answers is only a statement about SERCOM4. */
static int rfProofIdentity(void)
{
    uint8_t value;
    uint32_t n = RF_STATUS_WAIT_LIMIT;

    while (n > 0)
    {
        --n;
        Clock_BringupPump();
        Wdt_FeedPoll(n);

        if (rfSpiRegRead(RF_REG_PART_NUM, &value) && value == RF_PART_NUM_AT86RF233)
            return 1;
    }

    return 0;
}

/* Proof 2 - TRX_OFF held, not glimpsed: any non-TRX_OFF read (including 0x1F
 * STATE_TRANSITION_IN_PROGRESS, Table 7-3 :5394-5396, note 3 :5411-5412) restarts
 * the run, so the budget bounds a dead radio instead of hiding it. */
static int rfProofTrxOffHeld(void)
{
    uint8_t status;
    uint32_t same = 0;
    uint32_t n = RF_STATUS_WAIT_LIMIT;

    while (n > 0)
    {
        --n;
        Clock_BringupPump();
        Wdt_FeedPoll(n);

        if (rfSpiRegRead(RF_REG_TRX_STATUS, &status) &&
            (status & RF_TRX_STATUS_MASK) == RF_TRX_STATUS_OFF)
        {
            ++same;

            if (same >= RF_PROOF_STATE_IDENTICAL)
                return 1;
        }
        else
            same = 0;
    }

    return 0;
}

/* Proof 3 - the CLKM setting landed: write TRX_CTRL_0 then require the value back.
 * 9.6.4 note 1 (at86rf233.txt:13881-13895): after a reset CLKM_CTRL is shadowed -
 * the pad keeps its rate while reads answer the reset value 1 - so the datasheet's
 * own instruction is to write it again. A read-back of 1 means the write never
 * arrived; 0x04 means 8 MHz with SHA_SEL 0 (Table 9-19 :13985-14000), applied
 * immediately (:13866-13873). Each read-back follows its own write. */
static int rfProofClkmSetting(void)
{
    uint8_t value;
    uint32_t attempt;

    for (attempt = 0; attempt < RF_PROOF_SETTING_ATTEMPTS; ++attempt)
    {
        Clock_BringupPump();
        Wdt_FeedPoll(attempt);

        if (!rfSpiRegWrite(RF_REG_TRX_CTRL_0, RF_TRX_CTRL_0_8MHZ))
            continue;

        if (rfSpiRegRead(RF_REG_TRX_CTRL_0, &value) && value == RF_TRX_CTRL_0_8MHZ)
            return 1;
    }

    return 0;
}

/* 1 only when every proof passed (generator 1 is pointed at a pad with edges); 0
 * means the pad is not proven, so Clock_Init must not point a generator at it. */
int Rf_WakeAndForceClkm(void)
{
    rfPinInit();
    rfSercomInit();
    /* SLP_TR (PA20) stays Low from rfPinInit, so a rising edge never stops CLKM
   * (7.1.2.2, at86rf233.txt:4166-4190); /RST keeps its t10/tXTAL timing. */
    rfReset();

    if (!rfProofIdentity())
    {
        DBG(DBG_COMP_RF, DBG_RF_RSN_PROOF_NO_IDENTITY);
        return 0;
    }

    /* /RST alone leaves a P_ON RF in P_ON (Table 7-3 :5334); settle TRX_OFF before
   * the clock change. */
    if (!rfSpiRegWrite(RF_REG_TRX_STATE, RF_TRX_CMD_OFF) || !rfProofTrxOffHeld())
    {
        DBG(DBG_COMP_RF, DBG_RF_RSN_PROOF_NO_STATE);
        return 0;
    }

    /* CLKM to the vendor's 8 MHz, written AND verified: CLKM_CTRL is shadowed across
   * the radio's own reset (9.6.4 note 1), so a factory deCONZ board may already
   * carry 8 MHz while every read-back says 1 MHz. */
    if (!rfProofClkmSetting())
    {
        DBG(DBG_COMP_RF, DBG_RF_RSN_PROOF_NO_SETTING);
        return 0;
    }

    return 1;
}

/* ==================================================================== */
/* AT86RF233 access layer (samr21_rf.h)                                 */
/* ==================================================================== */

/* /SEL: PORT group 1 pin 31; low for the whole of a multi-byte access. */
static void rfSelLow(void)
{
    SAMR21_PORT_G(1)->OUTCLR = PORT_PIN(31);
}

static void rfSelHigh(void)
{
    SAMR21_PORT_G(1)->OUTSET = PORT_PIN(31);
}

int Rf_HwReady(void)
{
    return rfSercomReady;
}

/* Deliberately NOT a transceiver reset: a reset drops CLKM, the DFLL48M reference
 * the CPU runs on by the time the radio data path runs, so the radio is only ever
 * re-programmed here. The clock bring-up already pulsed /RST and proved identity. */
int Rf_HwInit(void)
{
    if (!rfSercomReady)
    {
        rfPinInit();
        rfSercomInit();
        rfReset();
    }

    return rfSercomReady;
}

int Rf_RegRead(uint8_t addr, uint8_t *value)
{
    int ok;

    Rf_Lock();
    ok = rfSpiRegRead(addr, value);
    Rf_Unlock();
    return ok;
}

int Rf_RegWrite(uint8_t addr, uint8_t value)
{
    int ok;

    Rf_Lock();
    ok = rfSpiRegWrite(addr, value);
    Rf_Unlock();
    return ok;
}

int Rf_TrxCmd(uint8_t cmd)
{
    return Rf_RegWrite(RF_REG_TRX_STATE, cmd);
}

int Rf_TrxStatus(uint8_t *status)
{
    return Rf_RegRead(RF_REG_TRX_STATUS, status);
}

static int rfWaitTrxState2(uint8_t wantA, uint8_t wantB)
{
    uint32_t n = RF_STATUS_WAIT_LIMIT;
    uint8_t st;

    while (n > 0u)
    {
        --n;
        Clock_BringupPump();
        Wdt_FeedPoll(n);

        if (!Rf_TrxStatus(&st))
            continue;

        if ((st & RF_TRX_STATUS_MASK) == wantA || (st & RF_TRX_STATUS_MASK) == wantB)
            return 1;
    }

    return 0;
}

int Rf_WaitTrxState(uint8_t want)
{
    /* 0xFF is unreachable through the status mask: the single-state case. */
    return rfWaitTrxState2(want, 0xFFu);
}

int Rf_WaitTrxStateAny(uint8_t wantA, uint8_t wantB)
{
    return rfWaitTrxState2(wantA, wantB);
}

/* A read clears the register and drops the pin (at86rf233 datasheet 6.6); the register is
 * read-only, so nothing is written back. Once the radio vector is armed, the handler is the
 * only reader and the thread takes its bits from the handler's mirror. */
int Rf_IrqStatus(uint8_t *status)
{
    return Rf_RegRead(RF_REG_IRQ_STATUS, status);
}

/* One frame in ONE /SEL-low access: the frame-buffer address counter cannot be
 * restarted mid-frame, so PHR is read inside the same access. Layout and length:
 * [PHY_STATUS, PHR, PSDU..., LQI, ED, RX_STATUS] = 5 + frame_length bytes
 * (at86rf233.txt:2343-2348). */
int Rf_FrameBufferRead(Rf_RxFrame *out)
{
    uint8_t phr;
    int ok = 1;

    if (out == 0)
        return 0;

    Rf_Lock();
    rfSelLow();

    if (!rfSpiTransceiveByte(RF_SPI_CMD_FRAMEBUFFER_READ, &out->phyStatus) ||
        !rfSpiTransceiveByte(0x00u, &phr))
        ok = 0;
    else if (phr > RADIO_PSDU_MAX)
    {
        /* A PHR beyond the frame buffer's 128-byte capacity is corruption, not a
     * frame (at86rf233.txt:2179-2182). */
        ok = 0;
    }
    else
    {
        uint8_t tail[3];

        out->len = phr;
        ok = rfSpiBurstRead(out->psdu, phr) && rfSpiBurstRead(tail, 3u);
        out->lqi = tail[0];
        out->ed = tail[1];
        out->rxStatus = tail[2];
    }

    rfSelHigh();
    Rf_Unlock();
    return ok;
}

int Rf_SramWrite(uint8_t addr, const uint8_t *src, uint8_t n)
{
    uint8_t head[2];
    int ok;

    head[0] = RF_SPI_CMD_SRAM_WRITE;
    head[1] = addr;
    Rf_Lock();
    rfSelLow();
    ok = rfSpiBurstWrite(head, 2u) && rfSpiBurstWrite(src, n);
    rfSelHigh();
    Rf_Unlock();
    return ok;
}

int Rf_AesSetKey(const uint8_t key[16])
{
    uint8_t mode = RF_AES_MODE_KEY;
    uint8_t head[2];
    int ok;

    head[0] = RF_SPI_CMD_SRAM_WRITE;
    head[1] = RF_SRAM_AES_CTRL;
    Rf_Lock();
    rfSelLow();
    ok = rfSpiBurstWrite(head, 2u) && rfSpiBurstWrite(&mode, 1u) && rfSpiBurstWrite(key, 16u);
    rfSelHigh();
    Rf_Unlock();
    return ok;
}

/* AES_CTRL, the block, and AES_CTRL_MIRROR's request in one access, which is what starts the engine; a
 * second access reads the result once the block is through. Nothing is polled: the engine takes a fixed
 * at most 24 us (at86rf233 datasheet 11.1), and reading its status costs as long as waiting. */
int Rf_AesEncrypt(const uint8_t in[16], uint8_t out[16])
{
    uint8_t mode = RF_AES_MODE_ECB;
    uint8_t go = RF_AES_REQUEST;
    uint8_t head[2];
    uint32_t stamp;
    int ok;

    head[0] = RF_SPI_CMD_SRAM_WRITE;
    head[1] = RF_SRAM_AES_CTRL;
    Rf_Lock();
    rfSelLow();
    ok = rfSpiBurstWrite(head, 2u) && rfSpiBurstWrite(&mode, 1u) && rfSpiBurstWrite(in, 16u) &&
         rfSpiBurstWrite(&go, 1u);
    rfSelHigh();
    stamp = Tick_CycleStamp();

    while (Tick_CyclesSince(stamp) < RF_AES_WAIT_CYCLES)
    {
    }

    head[0] = RF_SPI_CMD_SRAM_READ;
    head[1] = RF_SRAM_AES_STATE;
    rfSelLow();
    ok = ok && rfSpiBurstWrite(head, 2u) && rfSpiBurstRead(out, 16u);
    rfSelHigh();
    Rf_Unlock();
    return ok;
}

/* The caller supplies a PSDU *with* its FCS, which is what the host's STREAM_RAW length
 * counts (docs/protocol.md §7.1) and what every upstream host sends: OpenThread's SAMR21
 * driver writes PHR = mLength with the FCS inside it, and zigbee-on-host encodes the CRC into
 * the frame it hands over (zigbee-on-host's src/zigbee/mac.ts, encodeMACFrame).
 * PHR is that length, and TX_AUTO_CRC_ON replaces the last two octets of the buffer with the
 * checksum it computes over the first N-2 (at86rf233.txt:10984-10989). The frames this
 * firmware builds itself, the Ack, are written by Rf_FrameBufferWriteRaw with a PHR that
 * counts their FCS, so they are unaffected. */
int Rf_FrameBufferWrite(const uint8_t *psdu, uint8_t len)
{
    uint16_t i;
    uint8_t phr;
    uint8_t in;
    int ok;

    if (psdu == 0 || len == 0u)
        return 0;

    /* PHR is seven bits: the MPDU, its FCS included, has to fit. */
    if (len > RADIO_PSDU_MAX)
        return 0;

    phr = (uint8_t)len;

    Rf_Lock();
    rfSelLow();
    /* [command, PHR, PSDU...] - no address byte in this access mode (at86rf233.txt:2345-2347). */
    ok = rfSpiTransceiveByte(RF_SPI_CMD_FRAMEBUFFER_WRITE, &in) && rfSpiTransceiveByte(phr, &in);

    for (i = 0; ok && i < len; ++i)
        ok = rfSpiTransceiveByte(psdu[i], &in);

    rfSelHigh();
    Rf_Unlock();
    return ok;
}

/* ---- the transceiver's IRQ line and SPI ownership ------------------------------------- */

/* Once the radio vector is armed, the SPI belongs to whichever context holds it: the handler
 * runs at the highest priority and cannot be interrupted by thread code, so the thread side
 * masks the vector for the length of each of its transactions. Masking the one NVIC line
 * leaves USB and SysTick running. The level-sensitive line keeps the request pending while it
 * is masked, so nothing the transceiver raises in that window is lost. */
static volatile uint8_t rfIrqArmed;
static volatile uint8_t rfInHandler;
static uint8_t rfLockDepth;

void Rf_Lock(void)
{
    if (rfInHandler || !rfIrqArmed)
        return;

    if (rfLockDepth++ == 0u)
    {
        NVIC_ICER0 = 1uL << IRQN_EIC;
        HAL_Dsb();
    }
}

void Rf_Unlock(void)
{
    if (rfInHandler || !rfIrqArmed || rfLockDepth == 0u)
        return;

    if (--rfLockDepth == 0u)
        NVIC_ISER0 = 1uL << IRQN_EIC;
}

/* PB00 as EIC EXTINT0, level-high. Level detection is asynchronous and needs no GCLK_EIC
 * (samr21 datasheet 19.6.3), which matters on this board: a GCLK_EIC route reads back
 * uncommitted (docs/hardware_samr21.md §6). The AT86RF233 holds IRQ high until IRQ_STATUS is read
 * (at86rf233 datasheet 6.6), so the level is exactly "a cause is pending". */
void Rf_FrontEndInit(uint32_t enablePins)
{
    Samr21PortGroup *g0 = SAMR21_PORT_G(0);
    uint32_t hi = enablePins >> 16;
    uint32_t lo = enablePins & 0xFFFFu;

    if (enablePins == 0u)
        return;

    SAMR21_PM->APBCMASK |= PM_APBCMASK_RFCTRL;
    *SAMR21_RFCTRL_FECTRL = (uint16_t)(RFCTRL_FECTRL_F0_DIG4 | RFCTRL_FECTRL_F1_DIG3);

    /* PA08 = FECTRL[0], PA09 = FECTRL[1]: outputs of function F, lower half-word. PMUX is
     * shared by the pair, so one write sets both. */
    g0->DIRSET = PORT_PIN(8) | PORT_PIN(9);
    g0->WRCONFIG = PORT_WRCONFIG_WRPINCFG | PORT_WRCONFIG_WRPMUX |
                   PORT_WRCONFIG_PMUX(PORT_MUX_PA08F_FECTRL0) | PORT_WRCONFIG_PMUXEN |
                   PORT_WRCONFIG_PINMASK(PORT_PIN(8) | PORT_PIN(9));

    /* The enable pins: plain GPIO outputs, high. */
    g0->OUTSET = enablePins;
    g0->DIRSET = enablePins;
    if (lo != 0u)
        g0->WRCONFIG = PORT_WRCONFIG_WRPINCFG | PORT_WRCONFIG_PINMASK(lo);
    if (hi != 0u)
        g0->WRCONFIG = PORT_WRCONFIG_HWSEL | PORT_WRCONFIG_WRPINCFG | PORT_WRCONFIG_PINMASK(hi);
}

void Rf_IrqLineInit(void)
{
    Samr21PortGroup *g1 = SAMR21_PORT_G(1);
    uint32_t n;

    g1->DIRCLR = PORT_PIN(0);
    g1->WRCONFIG = PORT_WRCONFIG_WRPINCFG | PORT_WRCONFIG_WRPMUX |
                   PORT_WRCONFIG_PMUX(PORT_MUX_PB00A_EIC_EXTINT0) | PORT_WRCONFIG_INEN |
                   PORT_WRCONFIG_PMUXEN | PORT_WRCONFIG_PINMASK(PORT_PIN(0));

    SAMR21_PM->APBAMASK |= PM_APBAMASK_EIC;

    SAMR21_EIC->CTRL = 0u;

    for (n = 0; n < RF_SYNC_WAIT_LIMIT && (SAMR21_EIC->STATUS & EIC_STATUS_SYNCBUSY); ++n)
        ;

    SAMR21_EIC->INTENCLR = 1uL << EIC_LINE_RF;
    SAMR21_EIC->CONFIG[0] = (SAMR21_EIC->CONFIG[0] & ~EIC_CONFIG_LINE_MASK(EIC_LINE_RF)) |
                            EIC_CONFIG_SENSE(EIC_LINE_RF, EIC_SENSE_HIGH);
    SAMR21_EIC->INTFLAG = 1uL << EIC_LINE_RF;
    SAMR21_EIC->INTENSET = 1uL << EIC_LINE_RF;
    /* WAKEUPEN makes the line an asynchronous interrupt (samr21 datasheet 19.6.6). Without it
     * the request is not taken while the core sleeps in the loop's WFI, so the vector waits for
     * the next SysTick - up to a millisecond, longer than the Ack's whole turnaround. */
    SAMR21_EIC->WAKEUP |= 1uL << EIC_LINE_RF;
    SAMR21_EIC->CTRL = EIC_CTRL_ENABLE;

    for (n = 0; n < RF_SYNC_WAIT_LIMIT && (SAMR21_EIC->STATUS & EIC_STATUS_SYNCBUSY); ++n)
        ;
}

/* Priorities: the radio vector 0, USB and SysTick 2. They keep their relative order (equal, so
 * neither preempts the other, as before), and the radio preempts both, because the Ack's
 * turnaround is a deadline and theirs are not (docs/architecture.md §7). */
void Rf_IrqArm(void)
{
    NVIC_SET_PRIO(IRQN_EIC, 1u);
    NVIC_SET_PRIO(IRQN_USB, 2u);
    SCB_SHPR3 = (SCB_SHPR3 & ~SCB_SHPR3_SYSTICK_MASK) | SCB_SHPR3_SYSTICK(2u);

    SAMR21_EIC->INTFLAG = 1uL << EIC_LINE_RF;
    NVIC_ICPR0 = 1uL << IRQN_EIC;
    rfLockDepth = 0u;
    rfIrqArmed = 1u;
    NVIC_ISER0 = 1uL << IRQN_EIC;
}

int Rf_IrqArmed(void)
{
    return rfIrqArmed;
}

int Rf_IrqPinHigh(void)
{
    return (SAMR21_PORT_G(1)->IN & PORT_PIN(0)) ? 1 : 0;
}

void Rf_HandlerEnter(void)
{
    rfInHandler = 1u;
}

/* The flag is cleared after IRQ_STATUS was read, and the line is level-sensitive: a cause that
 * arrived since sets it again at once, so nothing is lost by clearing late. */
void Rf_HandlerExit(void)
{
    SAMR21_EIC->INTFLAG = 1uL << EIC_LINE_RF;
    rfInHandler = 0u;
}

/* Reading IRQ_STATUS clears it and drops the IRQ pin (at86rf233 datasheet 6.6). */
int Rf_HandlerIrqStatus(uint8_t *status)
{
    return rfSpiRegRead(RF_REG_IRQ_STATUS, status);
}

/* [command, PHR, MPDU octets...]: an Ack is four octets with PHR = 5, and the radio fills the
 * two FCS positions itself, so nothing else is written. */
int Rf_FrameBufferWriteRaw(const uint8_t *buf, uint8_t n)
{
    uint8_t in;
    uint8_t i;
    int ok;

    Rf_Lock();
    rfSelLow();
    ok = rfSpiTransceiveByte(RF_SPI_CMD_FRAMEBUFFER_WRITE, &in);

    for (i = 0; ok && i < n; ++i)
        ok = rfSpiTransceiveByte(buf[i], &in);

    rfSelHigh();
    Rf_Unlock();
    return ok;
}

int Rf_SetSpiBaud(uint8_t baud)
{
    if (!rfSercomReady)
        return 0;
    /* BAUD is Enable-Protected (samr21.txt:22999-23002): the same down/up sequence
   * rfSercomInit used. */
    SAMR21_SERCOM4->CTRLA = 0;

    if (!rfSercomSyncClear(SERCOM_SPI_SYNCBUSY_ENABLE, DBG_RF_RSN_ENABLE))
        return 0;

    SAMR21_SERCOM4->BAUD = baud;
    SAMR21_SERCOM4->CTRLA = RF_SERCOM_SPI_CTRLA;
    return rfSercomSyncClear(SERCOM_SPI_SYNCBUSY_ENABLE, DBG_RF_RSN_ENABLE);
}

int Rf_PartNum(uint8_t *value)
{
    return rfSpiRegRead(RF_REG_PART_NUM, value);
}
