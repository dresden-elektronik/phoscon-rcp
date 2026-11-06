/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* Clock bring-up for the SAMR21 (CB2): the vendor clock regime.
 *
 * Closed loop from the RF233 CLKM pad: CLKM 8 MHz -> gen1 <- GCLKIN /250 =
 * 32 000 Hz, DFLLMUL.MUL 1500 -> 48 MHz (Table 42-40 bounds the loop at fREF
 * 0.732-33 kHz, fOUT 47-49 MHz, samr21.txt:54915-54941). USB is fed from a separate
 * gen5 <- DFLL48M /1. USBCRM is the fallback only: it has no reference until the
 * host sends a SOF (15.6.7.1, samr21.txt:7992-7993).
 *
 * GCLK access is byte-packed indirect, selected by the ID field of the written
 * value; the readback echoes the selected ID, so only SYNCBUSY/CLKEN/GENEN are
 * judged. A write aimed at a generator with no edges never completes and the next
 * core-register access to the block stalls the CPU (12.3.1.2, errata 10416) -> probe
 * before configuring, no GCLK write after a failed probe. Every wait is bounded and
 * WDT-fed; a frozen block ends in HAL_SystemReset().
 */
#include "samr21_regs.h"

#include "dbg.h"
#include "hal.h"
#include "hal_diag.h"
#include "usbc.h"

#define CPU_FREQ_HZ 48000000u
#define TICK_HZ 1000u

/* Bounded-wait budget: a live source settles in microseconds, the budget only
 * catches a dead one; the per-iteration WDT feed bounds the real time. 4096 polls
 * is ~5 ms at 8 MHz, ~0.7 ms at 48 MHz - above every datasheet synchronisation
 * window this tree waits on (13.6.3.1/13.6.3.2, samr21.txt:5196-5203) and far
 * below the ~8 s watchdog period. A window not closed by then is dead, not slow. */
#define CLK_WAIT_LIMIT 0x1000u

/* DFLL lock budget: tLOCK 100 us typ / 500 us max for the coarse+fine pair
 * (Table 42-40, samr21.txt:54930-54936), up to ~2x with the chill cycles left
 * enabled (samr21.txt:7974-7985). 4096 polls of a ~10-25 cycle loop is ~5-13 ms at
 * 8 MHz: a DFLL not locked by then is not locking. */
#define CLK_LOCK_LIMIT 0x1000u

/* ---- Bring-up stopwatch (component DBG_COMP_TIME, dbg.h) ----------------
 * SysTick is the only counter that exists before this function's own work: a core
 * peripheral clocked by the processor clock, needing no GCLK channel, no PM gate and no
 * interrupt (VAL counts with PRIMASK set). LOAD at the 24-bit maximum covers 16 777 216
 * cycles (2.10 s at 8 MHz). Step 11 reprograms LOAD to 1 ms, so a gap longer than one
 * period is credited one period short - read the total as a lower bound. */
#define BRINGUP_LOAD 0xFFFFFFu /* 24-bit maximum */
#define BRINGUP_UNIT 12u       /* stamp unit: 4096 cycles */

static int bringupOn;
static uint32_t bringupVal;    /* VAL as seen at the last pump */
static uint32_t bringupPeriod; /* LOAD + 1 as seen at the last pump */
static uint32_t bringupCycles; /* cycles since Clock_BringupClockStart() */

void Clock_BringupClockStart(void)
{
    bringupOn = 0;
    SAMR21_SYSTICK->CTRL = 0; /* stop: LOAD/VAL commit cleanly */
    SAMR21_SYSTICK->LOAD = BRINGUP_LOAD;
    SAMR21_SYSTICK->VAL = 0;
    /* ENABLE without TICKINT: VAL must move, and PRIMASK is still set here. */
    SAMR21_SYSTICK->CTRL = SYSTICK_CTRL_CLKSRC | SYSTICK_CTRL_ENABLE;
    bringupCycles = 0;
    bringupVal = BRINGUP_LOAD + 1u; /* the counter reloads to LOAD */
    bringupPeriod = BRINGUP_LOAD + 1u;
    bringupOn = 1;
}

void Clock_BringupTick(void)
{
    uint32_t period;

    if (!bringupOn)
        return;

    period = (SAMR21_SYSTICK->LOAD & SYSTICK_COUNT_MASK) + 1u;
    bringupPeriod = period;
    bringupCycles += period;
    bringupVal = SAMR21_SYSTICK->VAL & SYSTICK_COUNT_MASK;
}

void Clock_BringupPump(void)
{
    uint32_t period;
    uint32_t val;

    if (!bringupOn)
        return;

    period = (SAMR21_SYSTICK->LOAD & SYSTICK_COUNT_MASK) + 1u;
    val = SAMR21_SYSTICK->VAL & SYSTICK_COUNT_MASK;

    if (period != bringupPeriod)
    {
        /* Step 11 reprogrammed LOAD mid-bring-up: the previous VAL belonged to the old
     * period, so credit the transition nothing and restart from here. */
        bringupPeriod = period;
        bringupVal = val;
        return;
    }
    /* COUNT runs downwards and reloads: a val above the previous one wrapped. */
    bringupCycles += (val <= bringupVal) ? (bringupVal - val) : (bringupVal + period - val);
    bringupVal = val;
}

/* The clock tree read back to the host (dbg.h DBG_COMP_WITNESS): every stage record
 * says a write settled, none says what the tree ended up being. OSC8M.PRESC is
 * witnessed because a frame proof measured ~1.1 MHz (OSC8M at its reset PRESC /8)
 * while DBG_CLOCK_RSN_CPU_48MHZ reported success. The generator registers are read
 * with the datasheet's select-then-read procedure (13.8.4/13.8.5,
 * samr21.txt:5649-5651): the 8-bit write to the ID field touches bits 7:0 alone, so
 * it cannot clear GENEN (bit 16). */
static void witnessValue(uint16_t rsn, uint32_t value)
{
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)(value & 0xFFFFu), rsn);
}

/* GCLK generator roles: gen0 the CPU (GCLK_MAIN for the Power Manager, 13.6.2.3,
 * samr21.txt:5094-5100 - no peripheral is routed onto it here), gen1 the DFLL
 * reference divider (GCLKIN /250), gen4 the watchdog's inherited clock, gen5 the USB
 * generator (DFLL48M /1). gen2 is the watchdog's RESET generator (Table 13-5) and
 * gen3-style 8 MHz peripherals are not needed on the enumeration path; both stay
 * unwritten. */
#define CLK_GEN_CPU 0 /* the CPU; no peripheral is routed onto it here */
#define CLK_GEN_REF 1 /* GCLKIN (the AT86RF233 CLKM), divided: CLK_GEN_REF_DIV */
#define CLK_GEN_2 2   /* the watchdog's RESET generator (Table 13-5): untouched */
#define CLK_GEN_3 3   /* the DFLL's reference in an image with timed transmit; otherwise unused */
#define CLK_GEN_WDT 4 /* ULP32K/16: the WDT clock (probed, then left alone) */
#define CLK_GEN_USB 5 /* DFLL48M /1 -> GCLK_USB */

/* One generator's state, as two records, read after the window closed. */
static void witnessGenerator(uint16_t rsnCtrl, uint16_t rsnDiv, uint8_t gen)
{
    uint32_t ctrl;
    uint32_t div;

    *((volatile uint8_t *)&SAMR21_GCLK->GENCTRL) = gen;
    ctrl = SAMR21_GCLK->GENCTRL;
    *((volatile uint8_t *)&SAMR21_GCLK->GENDIV) = gen;
    div = SAMR21_GCLK->GENDIV;

    witnessValue(rsnCtrl,
                 (uint16_t)(((ctrl >> 8) & 0x1Fu)             /* SRC */
                            | (((ctrl >> 16) & 0x1u) << 4)    /* GENEN */
                            | (((ctrl >> 17) & 0x1u) << 5)    /* IDC */
                            | (((ctrl >> 20) & 0x1u) << 6)    /* DIVSEL */
                            | (((ctrl >> 21) & 0x1u) << 7)    /* RUNSTDBY */
                            | (((ctrl >> 19) & 0x1u) << 8))); /* OE */
    witnessValue(rsnDiv, (uint16_t)((div >> 8) & 0xFFFFu));
}

void Clock_Witnesses(void)
{
    witnessValue(DBG_WITNESS_RSN_OSC8M, SAMR21_SYSCTRL->OSC8M);
    witnessValue(DBG_WITNESS_RSN_PCLKSR, SAMR21_SYSCTRL->PCLKSR);
    witnessValue(DBG_WITNESS_RSN_DFLLCTRL, SAMR21_SYSCTRL->DFLLCTRL);
    witnessValue(DBG_WITNESS_RSN_DFLLVAL, SAMR21_SYSCTRL->DFLLVAL);
    witnessValue(DBG_WITNESS_RSN_DFLLMUL, SAMR21_SYSCTRL->DFLLMUL);
    witnessValue(DBG_WITNESS_RSN_DFLLMUL_HI,
                 ((SAMR21_SYSCTRL->DFLLMUL >> 16) & 0x03FFu) |
                     (((SAMR21_SYSCTRL->DFLLMUL >> 26) & 0x3Fu) << 8));
    witnessValue(DBG_WITNESS_RSN_GCLK_STATUS, SAMR21_GCLK->STATUS);
    witnessValue(DBG_WITNESS_RSN_GCLK_CTRL, SAMR21_GCLK->CTRL);
    witnessGenerator(DBG_WITNESS_RSN_GEN0_CTRL, DBG_WITNESS_RSN_GEN0_DIV, CLK_GEN_CPU);
    witnessGenerator(DBG_WITNESS_RSN_GEN1_CTRL, DBG_WITNESS_RSN_GEN1_DIV, CLK_GEN_REF);
    /* Which clock each channel is on, read through CLKCTRL's own window (13.8.3,
   * samr21.txt:5420-5421): GEN[11:8] and CLKEN say which generator feeds the channel
   * and whether it is on. Channel 0 is the DFLL's own reference. */
    {
        uint16_t ctrl;

        *((volatile uint8_t *)&SAMR21_GCLK->CLKCTRL) = GCLK_CLKCTRL_ID_DFLL48;
        ctrl = SAMR21_GCLK->CLKCTRL;
        witnessValue(DBG_WITNESS_RSN_CLKCTRL_REF, ctrl);
        *((volatile uint8_t *)&SAMR21_GCLK->CLKCTRL) = GCLK_CLKCTRL_ID_USB;
        ctrl = SAMR21_GCLK->CLKCTRL;
        witnessValue(DBG_WITNESS_RSN_CLKCTRL_USB, ctrl);
    }
    witnessValue(DBG_WITNESS_RSN_PM_CPUSEL, (uint32_t)(SAMR21_PM->CPUSEL & 0x07u));
    witnessGenerator(DBG_WITNESS_RSN_GEN5_CTRL, DBG_WITNESS_RSN_GEN5_DIV, CLK_GEN_USB);
    {
        uint32_t ms;

        Tick_GetMs(&ms);
        witnessValue(DBG_WITNESS_RSN_TICK_MS, (uint16_t)ms);
    }
    witnessValue(DBG_WITNESS_RSN_SYSTICK_LOAD, (uint16_t)(SAMR21_SYSTICK->LOAD & 0xFFFFu));
    witnessValue(DBG_WITNESS_RSN_SYSTICK_CTRL, (uint16_t)(SAMR21_SYSTICK->CTRL & 0xFFFFu));
}

uint16_t Clock_StampUnits(void)
{
    uint32_t units = bringupCycles >> BRINGUP_UNIT;

    /* Saturating, not wrapping: a wrap would look like an early stage. */
    return (units > 0xFFFFu) ? 0xFFFFu : (uint16_t)units;
}

void Clock_Stamp(uint16_t milestone)
{
    Clock_BringupPump();
    Debug_Emit(DBG_COMP_TIME, Clock_StampUnits(), milestone);
}

/* The pump's own floor, measured: 4096 iterations of a core-register read and a RAM
 * add. Clock_BringupPump reads a volatile MMIO register, so -Os cannot delete the
 * loop. */
static volatile uint32_t calibAcc;

void Clock_CalibStamp(void)
{
    uint32_t i;

    for (i = 0; i < 4096u; ++i)
    {
        calibAcc += i;
        Clock_BringupPump();
    }

    Clock_Stamp(DBG_TIME_RSN_CALIB);
}

#define DBG_CLOCK_RSN_DFLL_USBCRM 15u /* DFLL is in USBCRM (fallback only) */
#define DBG_CLOCK_RSN_WDT_CH 16u
#define DBG_CLOCK_RSN_RECOVERY_RESET 18u /* GCLK block frozen: reset the core */
#define DBG_CLOCK_RSN_GEN0 19u
#define DBG_CLOCK_RSN_WDT_INHERIT 20u /* probe: ch3 already GEN 4 + CLKEN */
#define DBG_CLOCK_RSN_WDT_RESTORE 21u
#define DBG_CLOCK_RSN_WDT_STUCK 22u /* probe: the CLKCTRL window did not answer */
#define DBG_CLOCK_RSN_BREADCRUMB 23u
#define DBG_CLOCK_RSN_CH_USB_NOT_ROUTED 24u     /* GCLK_USB did not commit: no function clock */
#define DBG_CLOCK_RSN_GEN_DIV_NOT_COMMITTED 27u /* GENEN+SRC commit, DIV may not */
#define DBG_CLOCK_RSN_GEN_NOT_COMMITTED 25u     /* GENEN/SRC did not read back */
#define DBG_CLOCK_RSN_PC16_MUX 13u              /* PC16 -> PORT function 5 = GCLK/CLKM */
#define DBG_CLOCK_RSN_GEN5 26u
#define DBG_CLOCK_RSN_DFLL_REF_OUT 27u    /* reference not knowable: USBCRM fallback */
#define DBG_CLOCK_RSN_GCLKIN_UNPROVEN 28u /* step 8 skipped: CLKM pad not proven */
#define DBG_CLOCK_RSN_DSU_REV_A 29u       /* die revision A: CLKM reaches GCLK_IO5 */
#define DBG_CLOCK_RSN_DSU_REV_NEWER 30u   /* revision B or later: gen1's pad */
#define DBG_CLOCK_RSN_DSU_READ_BAD 31u    /* DID 0/0xFFFFFFFF or reads disagreed */
/* In THIS build these three are recorded but never leave the chip: no CDC pipe is
 * reachable (Usbc_Init() is downstream) and the post-mortem sink needs
 * Nvram_StatusFlush(), which this path does not call. The hold band is the record. */

/*
 * The closed-loop chain as numbers: CLKM 8 MHz -> gen1 <- GCLKIN /250 -> 32 000 Hz,
 * DFLLMUL.MUL 1500 -> 48 MHz. Table 42-40 bounds the loop at fREF 0.732 / 32.768 /
 * 33 kHz and fOUT 47 / 48 / 49 MHz. The 1 MHz/32 x 1536 and 8 MHz/250 x 1500 trees
 * land on the same 48 MHz, but a divider or multiplier changed on its own is
 * illegal, so both halves of the guard make that a build failure, never a lock
 * timeout.
 */
#define CLKM_VENDOR_HZ 8000000u /* TRX_CTRL_0 = CLKM_CTRL 4 */
#ifdef RCP_TX_TIMING
/* A host schedules against the radio clock, and the DFLL only comes near 48 MHz: it settles some hundreds
 * of ppm off, differently on each boot, at any reference (docs/hardware_samr21.md §2). So this image takes
 * the other tree. CLKM reaches generator 1 alone, which divides it to 1 MHz for the radio clock's counter
 * (Tick_ClockUs) and feeds generator 3, whose 31 250 Hz is the DFLL's reference. */
#define CLK_GEN_REF_DIV 8u
#define CLK_GEN_DFLL_REF CLK_GEN_3
#define CLK_DFLL_REF_DIV 32u
#define DFLL_MUL_CLOSED 1536u
#else
#define CLK_GEN_REF_DIV 250u    /* halLowLevelInit.c:915 (0x0000FA01) */
#define CLK_GEN_DFLL_REF CLK_GEN_REF
#define CLK_DFLL_REF_DIV 1u
#define DFLL_MUL_CLOSED 1500u   /* halLowLevelInit.c:887 (MUL field) */
#endif
#define CLK_DFLL_REF_HZ (CLKM_VENDOR_HZ / CLK_GEN_REF_DIV / CLK_DFLL_REF_DIV)
#ifdef RCP_TX_TIMING
static int clockCrystalUp; /* generator 1 runs from CLKM, and generator 3 from it */
#endif
#define DFLL_REF_MIN_HZ 732u    /* 0.732 kHz, in Hz */
#define DFLL_REF_MAX_HZ 33000u
#define DFLL_OUT_MIN_HZ 47000000u
#define DFLL_OUT_MAX_HZ 49000000u

/* Build-time half of the guard: these constants must land inside Table 42-40. */
typedef char dfllClosedLoopInsideTable42_40
    [(CLK_DFLL_REF_HZ >= DFLL_REF_MIN_HZ &&
      CLK_DFLL_REF_HZ <= DFLL_REF_MAX_HZ &&
      CLK_DFLL_REF_HZ * DFLL_MUL_CLOSED >= DFLL_OUT_MIN_HZ &&
      CLK_DFLL_REF_HZ * DFLL_MUL_CLOSED <= DFLL_OUT_MAX_HZ)
         ? 1
         : -1];

/* DFLL48M USBCRM multiplier (fallback only): 0xBB80 = 48000 against the 1 kHz USB
 * SOF reference = 48.000 MHz (15.6.7.1, samr21.txt:7989-7990; halLowLevelInit.c:596,
 * clock.c:916). */
#define DFLL_MUL_USBCRM 48000u

/* DFLL48M FSTEP/CSTEP: the vendor's closed-loop pair, 0xff/4 = 63 and 0x1f/4 = 7
 * (halLowLevelInit.c:31-32,:887-888). The USBCRM fallback keeps CSTEP 7 with the
 * BTL's FSTEP 10. */
#define DFLL_FSTEP_VENDOR 63u
#define DFLL_CSTEP_VENDOR 7u

/* Bounded, WDT-fed wait for the GCLK write-sync to clear. STATUS is an APB register:
 * readable even while a sync is stuck (the stall hits the next core-register access,
 * not this read). 1 on settle. */
static int clockWaitSync(void)
{
    uint32_t n = 0;

    while (SAMR21_GCLK->STATUS & GCLK_STATUS_SYNCBUSY)
    {
        Clock_BringupPump();
        Wdt_FeedPoll(n);

        if (++n >= CLK_WAIT_LIMIT)
            return 0;
    }

    return 1;
}

/* Bounded, WDT-fed wait for PCLKSR.DFLLRDY, the DFLL register-write sync - not a lock
 * or running indicator (in USBCRM the PCLKSR DFLL bits are unreliable, errata 11938),
 * and mandatory: "any write to any of the DFLL48M control registers while DFLLRDY is
 * zero will be ignored" (samr21.txt:8408-8414). */
static int clockWaitDfllRdy(void)
{
    uint32_t n = 0;

    while (!(SAMR21_SYSCTRL->PCLKSR & SYSCTRL_PCLKSR_DFLLRDY))
    {
        Clock_BringupPump();
        Wdt_FeedPoll(n);

        if (++n >= CLK_WAIT_LIMIT)
            return 0;
    }

    return 1;
}

/* Bounded, WDT-fed wait for the OSC8M ready bit (halLowLevelInit.c:340): a generator
 * just pointed at OSC8M must be proven to have edges. OSC8MRDY is an oscillator-ready
 * status, not a write sync - the two are waited on separately. */
static int clockWaitOsc8mRdy(void)
{
    uint32_t n = 0;

    while (!(SAMR21_SYSCTRL->PCLKSR & SYSCTRL_PCLKSR_OSC8MRDY))
    {
        Clock_BringupPump();
        Wdt_FeedPoll(n);

        if (++n >= CLK_WAIT_LIMIT)
            return 0;
    }

    return 1;
}

/* Bounded, WDT-fed wait for the WDT write-sync (the hwWdtInit
 * ALWAYON reconfiguration). */
static int clockWaitWdtSync(void)
{
    uint32_t n = 0;

    while (SAMR21_WDT->STATUS & WDT_STATUS_SYNCBUSY)
    {
        Clock_BringupPump();
        Wdt_FeedPoll(n);

        if (++n >= CLK_WAIT_LIMIT)
            return 0;
    }

    return 1;
}

static int clockChannelRoute(uint8_t id, uint8_t gen);

/* Route a channel with ONE CLKCTRL store (deCONZ writes GCLK_USB once, immediately
 * before the USBC SWRST, halUsb.c:938). A disable-first pair is a hazard for the USB
 * channel: it opens a window with no function clock on a module about to
 * software-reset, and 12.3.2.2 discards a CLKCTRL write issued while SYNCBUSY is set.
 * Only GEN and CLKEN are judged, never the ID field (it echoes the selection). */
static int clockChannelRoute(uint8_t id, uint8_t gen)
{
    if (!clockWaitSync())
        return 0;

    SAMR21_GCLK->CLKCTRL = GCLK_CLKCTRL_ID(id) | GCLK_CLKCTRL_GEN(gen) | GCLK_CLKCTRL_CLKEN;

    {
        uint32_t n = 0;

        while (SAMR21_GCLK->STATUS & GCLK_STATUS_SYNCBUSY ||
               (SAMR21_GCLK->CLKCTRL & (GCLK_CLKCTRL_CLKEN | GCLK_CLKCTRL_GEN(gen))) !=
                   (GCLK_CLKCTRL_CLKEN | GCLK_CLKCTRL_GEN(gen)))
        {
            Clock_BringupPump();
            Wdt_FeedPoll(n);

            if (++n >= CLK_WAIT_LIMIT)
                return 0;
        }
    }
    return 1;
}

/* One GCLK generator, in the vendor image's shape: GENDIV ID|DIV and GENCTRL
 * ID|SRC|GENEN, each a single store followed by a SYNCBUSY wait, and no byte-select
 * or read-modify-write on GENCTRL (deCONZ.elf @ 0xf6c8). GENCTRL is the only GCLK
 * register with the Write-Synchronized property (samr21.txt:5647-5655), and GCLK is in
 * the common-synchronizer family (12.3, samr21.txt:4585-4591) that errata 10416
 * freezes; 12.3.2.2 discards a write issued while SYNCBUSY is set, and a window
 * register write re-specifies the WHOLE entry, so an extra store is a second
 * transaction of inherited state, not an additive one. `divsel` picks the hardware
 * division (1 = BTL binary /2^(DIV+1), 0 = linear DIV). 1 on commit; 0 on a timeout,
 * after which the caller must stop all GCLK writes. */
static int
clockGenStart(uint8_t gen, uint32_t src, uint32_t div, uint8_t divsel, uint32_t flags, uint16_t rsn)
{
    uint32_t gendiv = GCLK_GENDIV_ID(gen);
    uint32_t mask;

    if (div > 1u)
    {
        if (divsel)
        {
            /* BTL binary division: /2^(DIV+1) with DIVSEL=1. */
            uint32_t c = 0;

            for (mask = 2u; mask < div; mask <<= 1)
                c++;

            gendiv |= GCLK_GENDIV_DIV(c);
            flags |= GCLK_GENCTRL_DIVSEL;
        }
        else
        {
            /* Linear division: DIV = the exact factor, DIVSEL=0. */
            gendiv |= GCLK_GENDIV_DIV(div);
        }
    }

    if (!clockWaitSync())
    {
        DBG(DBG_COMP_CLOCK, rsn);
        return 0;
    }

    /* Select the generator on the divider register. */
    *((volatile uint8_t *)&SAMR21_GCLK->GENDIV) = gen;

    if (!clockWaitSync())
    {
        DBG(DBG_COMP_CLOCK, rsn);
        return 0;
    }

    SAMR21_GCLK->GENDIV = gendiv;

    if (!clockWaitSync())
    {
        DBG(DBG_COMP_CLOCK, rsn);
        return 0;
    }

    /* THE one configuration store: ID | SRC | extras | GENEN in a single word, nothing
   * read out of GENCTRL first, so no bit is inherited from the entry the last store
   * selected. A dead source can still leave SYNCBUSY stuck (13.6.2), so the wait below
   * stays bounded and feeding. */
    SAMR21_GCLK->GENCTRL = GCLK_GENCTRL_ID(gen) | src | flags | GCLK_GENCTRL_GENEN;

    if (!clockWaitSync())
    {
        DBG(DBG_COMP_CLOCK, rsn);
        return 0;
    }

    /* Prove the commit after the window closed: select the generator with an 8-bit write
   * to GENCTRL.ID, then read it back (13.6, samr21.txt:5649-5651). The ID field is not
   * evidence (it echoes the selection), so only GENEN and SRC are judged. */
    *((volatile uint8_t *)&SAMR21_GCLK->GENCTRL) = gen;

    if (!clockWaitSync())
    {
        DBG(DBG_COMP_CLOCK, rsn);
        return 0;
    }

    {
        uint32_t rb = SAMR21_GCLK->GENCTRL;
        uint32_t wantDiv = (gendiv >> 8) & 0xFFFFu;
        uint32_t wantSel = (flags & GCLK_GENCTRL_DIVSEL) ? 1u : 0u;
        uint32_t gotDiv;

        if (!(rb & GCLK_GENCTRL_GENEN) || (rb & 0xF00u) != (src & 0xF00u))
        {
            DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_GEN_NOT_COMMITTED);
            return 0;
        }

        /* The divider is part of the claim, so part of the proof: GENEN and SRC committing
   * says nothing about DIV, and a generator whose divider silently stayed put would
   * report every stage done while dividing by the wrong number. GENDIV is read through
   * its own window. DIVSEL is judged from the GENCTRL readback, not GENDIV: it is
   * GENCTRL bit 20, while the datasheet's GENDIV gives it only ID[3:0] and DIV[23:8]
   * (7:4 reserved, reading zero). */
        *((volatile uint8_t *)&SAMR21_GCLK->GENDIV) = gen;
        gotDiv = SAMR21_GCLK->GENDIV;

        if ((((gotDiv >> 8) & 0xFFFFu) != wantDiv) || (((rb >> 20) & 0x1u) != wantSel))
        {
            DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_GEN_DIV_NOT_COMMITTED);
            return 0;
        }
    }
    DBG(DBG_COMP_CLOCK, rsn);
    return 1;
}


/* DFLL48M COARSE CAL. Table 8-4 "NVM Software Calibration Area Mapping"
 * (samr21.txt:1587): the calibration area at 0x806020, NOT the NVM user row at
 * 0x804000 (Table 8-3 has no DFLL field). FUSE_DFLL_COARSE = OTP4[31:26]
 * (halLowLevelInit.c:573-576); guard 0x3f -> 0x1f (0x3F is erased NVM). */
static uint32_t clockDfllCoarse(void)
{
    uint32_t coarse = FUSE_DFLL_COARSE(FUSES_OTP4_WORD);

    if (coarse == 0x3Fu)
        coarse = 0x1Fu;

    return coarse;
}

/* DFLL48M FINE CAL: Table 8-4 bits 73:64 = word 0x00806028 [9:0] (samr21.txt:1588),
 * vendor guard 0x3ff -> 0x1ff (halLowLevelInit.c:581-585). */
static uint32_t clockDfllFine(void)
{
    uint32_t fine = FUSE_DFLL_FINE(FUSES_DFLL_FINE_WORD);

    if (fine == 0x3FFu)
        fine = 0x1FFu;

    return fine;
}

/*
 * PC16 <- PORT function 5, the SAM side of the vendor's CLKM reference pad
 * (halRfCtrl.c:194-202). The pin table says "GCLK PC16(F) CLKM" (samr21.txt:44113)
 * and errata 13152 states "CLKM is internally connected to GCLK/IO[5] on PC16"
 * (:57140-57144) - the pad cannot clock the CPU directly, which is why the reference
 * goes through a generator divider into the DFLL. Must precede
 * clockGenStart(1, SRC_GCLKIN, ...): GENCTRL.SRC = GCLKIN makes the GCLK_IO an input
 * (samr21.txt:4990). WRPMUX is required for PMUX to be written.
 */
static void clockGclkInPin(void)
{
    Samr21PortGroup *g2 = SAMR21_PORT_G(2);

    g2->DIRCLR = PORT_PIN(16);
    g2->WRCONFIG = PORT_WRCONFIG_HWSEL | PORT_WRCONFIG_WRPINCFG | PORT_WRCONFIG_WRPMUX |
                   PORT_WRCONFIG_PMUX(PORT_MUX_PC16F_GCLK_IO1) | PORT_WRCONFIG_INEN |
                   PORT_WRCONFIG_PMUXEN | PORT_WRCONFIG_PINMASK(PORT_PIN(16) >> 16);
}

/*
 * Runtime half of the reference guard: is the reference knowable, and is reference x
 * MUL inside Table 42-40? `clkmForced` is the only trustworthy knowledge of the pad
 * rate: TRX_CTRL_0.CLKM_CTRL is shadowed across the radio's reset (at86rf233 9.6.4
 * note 1), so an unlanded write leaves the rate unknown and a lock asked for under it
 * buys only a bounded timeout.
 */
static int clockDfllReferenceOk(int clkmForced)
{
    uint32_t ref;
    uint32_t out;

    if (!clkmForced)
        return 0;

    ref = CLK_DFLL_REF_HZ;

    if (ref < DFLL_REF_MIN_HZ || ref > DFLL_REF_MAX_HZ)
        return 0;

    out = ref * DFLL_MUL_CLOSED;
    return (int)(out >= DFLL_OUT_MIN_HZ && out <= DFLL_OUT_MAX_HZ);
}

/* Bounded, WDT-fed wait for one DFLL lock flag: coarse first, then fine, the vendor's
 * order (halLowLevelInit.c:898,:900). Legal here because this is closed loop: errata
 * 11938 (samr21.txt:58636-58640) forbids monitoring the PCLKSR DFLL bits in USB clock
 * recovery mode. */
static int clockWaitDfllBit(uint32_t bit, uint16_t rsn)
{
    uint32_t n = 0;

    while (!(SAMR21_SYSCTRL->PCLKSR & bit))
    {
        Clock_BringupPump();
        Wdt_FeedPoll(n);

        if (++n >= CLK_LOCK_LIMIT)
        {
            DBG(DBG_COMP_CLOCK, rsn);
            return 0;
        }
    }

    return 1;
}

static int clockWaitDfllLock(void)
{
    if (!clockWaitDfllBit(SYSCTRL_PCLKSR_DFLLLCKC, DBG_CLOCK_RSN_DFLL_NOLOCK))
        return 0;

    if (!clockWaitDfllBit(SYSCTRL_PCLKSR_DFLLLCKF, DBG_CLOCK_RSN_DFLL_NOLOCK))
        return 0;

    return 1;
}

/* The multiplier the DFLL is asked to produce, so the probe can distinguish a DFLL
 * locked to the wrong reference from one not tracking its reference at all. */
/* The SOF span (hal.h): a host SOF is 1 kHz, so cycles between two SOF samples is
 * f_CPU = cycles x 1000 with no host timestamp. A delta, not an absolute stamp:
 * bringupCycles saturates at 268 M cycles in the 4096-cycle unit. */



/*
 * DFLL48M closed loop from the RF233 CLKM pad: the regime the released app on this
 * board runs (halLowLevelInit.c:864-903). Vendor word set: INTFLAG = 0xD0, wait
 * PCLKSR.DFLLRDY, DFLLCTRL = 0 when ENABLE is clear, DFLLMUL = 0x1C3F05DC (MUL 1500 |
 * FSTEP 63 | CSTEP 7), DFLLCTRL = 0x0006 (ENABLE|MODE), then wait LCKC, wait LCKF. No
 * DFLLVAL write (DFLLVAL.COARSE starts at reset 0, samr21.txt:10009-10028); no BPLCKC,
 * no CCDIS, no USBCRM, no ONDEMAND - which satisfies errata 9905
 * (samr21.txt:57353-57356) the vendor's way. One deviation: DFLLCTRL is written 0
 * first, because the GCF bootloader can have left a USBCRM DFLL armed. `rsn` is the
 * stage record; 1 when every write synchronized, the caller still waits the lock.
 */
/* ch0 (GCLK_DFLL48M_REF) <- gen1 in the released image's shape: one 16-bit CLKCTRL
 * store, no STATUS poll and no CLKCTRL readback in between. The bounded, feeding
 * STATUS-only wait after the store is kept (never a CLKCTRL read while it
 * synchronises), because a GCLK write into an open window stalls the bus. */
static int clockRefChannelVendor(void)
{
    uint32_t n = 0;

    (void)SAMR21_SYSCTRL->DFLLCTRL;
    SAMR21_SYSCTRL->DFLLCTRL = 0;
    SAMR21_GCLK->CLKCTRL = (uint16_t)(GCLK_CLKCTRL_ID(GCLK_CLKCTRL_ID_DFLL48) |
                                      GCLK_CLKCTRL_GEN(CLK_GEN_DFLL_REF) | GCLK_CLKCTRL_CLKEN);

    while (SAMR21_GCLK->STATUS & GCLK_STATUS_SYNCBUSY)
    {
        Clock_BringupPump();
        Wdt_FeedPoll(n);

        if (++n >= CLK_WAIT_LIMIT)
            return 0;
    }

    return 1;
}

static int clockDfllClosedLoop(uint16_t rsn)
{
    const uint32_t mul = SYSCTRL_DFLLMUL_CSTEP(DFLL_CSTEP_VENDOR)   /* CSTEP 7  of max 31 */
                         | SYSCTRL_DFLLMUL_FSTEP(DFLL_FSTEP_VENDOR) /* FSTEP 63 of max 1023 */
                         | SYSCTRL_DFLLMUL_MUL(DFLL_MUL_CLOSED);    /* reference x MUL = 48 MHz */
    const uint32_t ctrl = SYSCTRL_DFLLCTRL_ENABLE | SYSCTRL_DFLLCTRL_MODE; /* 0x0006 */

    /* The vendor's flag clear, before anything is polled (halLowLevelInit.c:867-868): the
   * lock latch comes out of this flag domain, so a stale latch must not answer it. */
    SAMR21_SYSCTRL->INTFLAG =
        SYSCTRL_INTFLAG_DFLLRDY | SYSCTRL_INTFLAG_DFLLLCKF | SYSCTRL_INTFLAG_DFLLLCKC;

    SAMR21_SYSCTRL->DFLLCTRL = 0;

    if (!clockWaitDfllRdy())
    {
        DBG(DBG_COMP_CLOCK, rsn);
        return 0;
    }

    /* Each DFLL register write is preceded by a DFLLRDY handshake: "any write to
   * any of the DFLL48M control registers while DFLLRDY is zero will be ignored"
   * (samr21.txt:8408-8414). */
    SAMR21_SYSCTRL->DFLLMUL = mul;

    if (!clockWaitDfllRdy())
    {
        DBG(DBG_COMP_CLOCK, rsn);
        return 0;
    }

    SAMR21_SYSCTRL->DFLLCTRL = ctrl; /* and none after it either: no DFLLVAL */

    if (!clockWaitDfllRdy())
    {
        DBG(DBG_COMP_CLOCK, rsn);
        return 0;
    }

    DBG(DBG_COMP_CLOCK, rsn); /* the settle: MUL 1500 programmed, closed loop */
    return 1;
}

/* DFLL48M in USBCRM - the FALLBACK regime, never the primary: the 48 MHz the GCF
 * bootloader itself runs on this PCB (halLowLevelInit.c:590-599): control 0, MUL,
 * DFLLVAL, then ENABLE|USBCRM|CCDIS in one write. No MODE, no BPLCKC (USBCRM ignores
 * it, samr21.txt:7995-7996), QLDIS left at reset 0 ("must be cleared",
 * samr21.txt:7997), CCDIS set (samr21.txt:7997-7998), and no lock wait - errata 11938
 * (samr21.txt:58636-58640) forbids monitoring PCLKSR in USBCRM. The trim replaces the
 * lock: DFLLVAL.COARSE becomes the final coarse value (samr21.txt:7995-7997), so
 * DFLLVAL comes from the calibration row.
 *
 * No GCLK write happens here: "The SOF signal from USB device will be used as
 * reference clock (CLK_DFLL_REF), ignoring the selected generic clock reference"
 * (15.6.7.1, samr21.txt:7992-7993) - no reference at all until the host sends a SOF.
 * `rsn` is the stage record; 1 when every write synchronized, 0 only if SYSCTRL itself
 * never answers. */
static int clockDfllUsbcrm(uint16_t rsn)
{
    /* MUL 0xBB80 with the BTL's CSTEP/FSTEP on this board (clock.c:396-398,:909-917):
   * production writes MUL alone, but a zero maximum step would freeze the fine
   * tracking. */
    const uint32_t mul = SYSCTRL_DFLLMUL_CSTEP(0x1Fu / 4u) /* CSTEP 7 of max 31 */
                         | SYSCTRL_DFLLMUL_FSTEP(10u)      /* FSTEP 10 of max 1023 */
                         | SYSCTRL_DFLLMUL_MUL(DFLL_MUL_USBCRM);
    const uint32_t val =
        SYSCTRL_DFLLVAL_FINE(clockDfllFine()) | SYSCTRL_DFLLVAL_COARSE(clockDfllCoarse());
    const uint32_t ctrl = SYSCTRL_DFLLCTRL_ENABLE | SYSCTRL_DFLLCTRL_USBCRM |
                          SYSCTRL_DFLLCTRL_CCDIS; /* samr21.txt:7997-7998; no MODE, no BPLCKC */

    /* Start from a known state: the BTL's system_clock_deinit leaves the DFLL disabled
   * with its configuration zeroed (clock.c:1077-1090); writing 0 is the vendor's
   * "Reset/Disable the control register after power on reset" (halLowLevelInit.c:593). */
    SAMR21_SYSCTRL->DFLLCTRL = 0;

    if (!clockWaitDfllRdy())
    {
        DBG(DBG_COMP_CLOCK, rsn);
        return 0;
    }

    /* Errata 9905 (samr21.txt:57353-57356): ONDEMAND must be zero before the DFLL is
   * configured; the ENABLE-first write clears it (halLowLevelInit.c:875 then :892). */
    SAMR21_SYSCTRL->DFLLCTRL = SYSCTRL_DFLLCTRL_ENABLE;

    if (!clockWaitDfllRdy())
    {
        DBG(DBG_COMP_CLOCK, rsn);
        return 0;
    }

    SAMR21_SYSCTRL->DFLLMUL = mul;
    SAMR21_SYSCTRL->DFLLVAL = val;

    /* Off, then the recipe word in one write (the BTL's order); nothing after it waits
   * for a lock. */
    SAMR21_SYSCTRL->DFLLCTRL = 0;

    if (!clockWaitDfllRdy())
    {
        DBG(DBG_COMP_CLOCK, rsn);
        return 0;
    }

    SAMR21_SYSCTRL->DFLLCTRL = ctrl;

    if (!clockWaitDfllRdy())
    {
        DBG(DBG_COMP_CLOCK, rsn);
        return 0;
    }

    DBG(DBG_COMP_CLOCK, rsn); /* the settle: the board is in USBCRM now */
    return 1;
}

/* How the watchdog's clock was found. Three answers: an inherited channel must not be
 * touched, a foreign one is rebuilt, and a window that did not answer must not be
 * written to again. */
typedef enum
{
    CLOCK_PROBE_INHERITED, /* CLKCTRL 0x03 = GEN 4 + CLKEN: leave the tree alone */
    CLOCK_PROBE_FOREIGN,   /* something else: restore gen4 + ch3 */
    CLOCK_PROBE_STUCK      /* the CLKCTRL window did not settle: no further GCLK write */
} Clock_Probe;

/*
 * Read-only probe of the watchdog's own clock channel (CLKCTRL core ID 0x03, Table
 * 13-4, samr21.txt:5513). The GCF bootloader puts the watchdog on generator 4 and never
 * takes it off: ID 0x03 -> GEN 0x04 with CLKEN 1 (de_btl/src/wdt.c:37-43) on gen4 =
 * OSCULP32K /16 RUNSTDBY (conf_clocks.h:163-167), and system_clock_deinit() skips this
 * channel (clock.c:758-763) - nothing to repair. Selecting the ID points no channel at
 * any generator, so it cannot aim a write at an edgeless source; it can still hit a
 * block that will not synchronize, which is what CLOCK_PROBE_STUCK is for (errata
 * 10416, samr21.txt:57169-57178).
 */
static Clock_Probe clockWdtChannelProbe(void)
{
    uint16_t ch;

    if (!clockWaitSync())
    {
        DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_WDT_STUCK);
        return CLOCK_PROBE_STUCK;
    }

    *((volatile uint8_t *)&SAMR21_GCLK->CLKCTRL) = GCLK_CLKCTRL_ID_WDT;

    if (!clockWaitSync())
    {
        DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_WDT_STUCK);
        return CLOCK_PROBE_STUCK;
    }

    ch = SAMR21_GCLK->CLKCTRL;

    if ((ch & GCLK_CLKCTRL_GEN_MASK) == GCLK_CLKCTRL_GEN(CLK_GEN_WDT) && (ch & GCLK_CLKCTRL_CLKEN))
    {
        DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_WDT_INHERIT); /* inherited: write nothing */
        return CLOCK_PROBE_INHERITED;
    }

    /* Not the bootloader's state: the entry is off or on another generator. Rebuilding it
   * restores the clock - an unclocked watchdog turns any hang into an unrecoverable
   * board. */
    DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_WDT_RESTORE);
    return CLOCK_PROBE_FOREIGN;
}

/* CLKCTRL_GCLK_WDT (instance 3) -> generator 4, probe-then-configure, reached only when
 * the probe said NOT inherited. Three 16-bit RMW writes with a sync wait between each
 * (12.3.2.2, samr21.txt:4748-4750: "To write again to the same core register in the
 * same module, user must wait for the end of synchronization or the write will be
 * discarded").
 *
 * GEN[11:8] is a four-bit FIELD, so it is assigned, never OR-ed: OR-ing into the readback
 * echo (GEN = 2 for this entry after any reset, Table 13-5) gave 2|4 = 6, generator 6,
 * whose reset GENCTRL is SRC = 0 = XOSC - and the ConBee II has no crystal on XOSC. A
 * channel on an edgeless generator is the never-completing synchronisation of errata
 * 10416 and leaves the watchdog with no clock at all: no timeout, no recovery into the
 * bootloader window, ever.
 */
static int clockWdtChannel(void)
{
    if (!clockWaitSync())
    {
        DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_WDT_CH);
        return 0;
    }

    *((volatile uint8_t *)&SAMR21_GCLK->CLKCTRL) = GCLK_CLKCTRL_ID_WDT;

    if (!clockWaitSync())
    {
        DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_WDT_CH);
        return 0;
    }

    SAMR21_GCLK->CLKCTRL = (uint16_t)((SAMR21_GCLK->CLKCTRL & (uint16_t)~GCLK_CLKCTRL_GEN_MASK) |
                                      GCLK_CLKCTRL_GEN(CLK_GEN_WDT));

    if (!clockWaitSync())
    {
        DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_WDT_CH);
        return 0;
    }

    SAMR21_GCLK->CLKCTRL |= GCLK_CLKCTRL_CLKEN;

    if (!clockWaitSync())
        DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_WDT_CH);

    return 1;
}

/* Restore the watchdog's clock (gen4 <- OSCULP32K /16 RUNSTDBY + ch3 -> gen4). The
 * divisor is the BTL's (conf_clocks.h:166 -> 2048 Hz); production uses /32
 * (halLowLevelInit.c:683). A failure means the GCLK block is stuck: the caller must not
 * issue any further GCLK write. */
static int clockWdtClockStart(void)
{
    if (!clockGenStart(CLK_GEN_WDT,
                       GCLK_GENCTRL_SRC_OSCULP32K,
                       16u,
                       1u,
                       GCLK_GENCTRL_RUNSTDBY,
                       DBG_CLOCK_RSN_GEN4))
        return 0;

    return clockWdtChannel();
}

/* Arm the watchdog: CONFIG.PER 11 (2^14 cycles of gen4 = ULP32K/16 = 2048 Hz -> 8.0 s)
 * and ALWAYON, then feed. CONFIG is enable-protected, so the write goes in while ENABLE
 * is 0; if the bootloader already set ALWAYON no application write can change the period
 * or switch the timer off (samr21.txt:11168, Table 16-4). Arm only - the channel was
 * settled in step 6. */
static void clockWdtArm(void)
{
    if ((SAMR21_WDT->CTRL & WDT_CTRL_ALWAYSON) == 0)
    {
        SAMR21_WDT->CTRL = 0;     /* ENABLE=0, WEN=0, ALWAYSON=0 */
        SAMR21_WDT->INTENCLR = 0; /* the BTL writes EW=0 */
        SAMR21_WDT->CONFIG =
            (SAMR21_WDT->CONFIG & (uint8_t)~WDT_CONFIG_PER(0x1Fu)) | WDT_CONFIG_PER(11u);
        SAMR21_WDT->CTRL |= WDT_CTRL_ALWAYSON;

        if (!clockWaitWdtSync())
        {
            DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_WDT_ON); /* timeout: this line */
        }
    }

    Wdt_Feed();
    DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_WDT_ON); /* the stage settled */
}


/* Band ladder, in ms: the furthest stage reached, read out as a bus gap by
 * Clock_FaultReport(). Nominal
 * hold x ~1.74 = bus time (measured on a ConBee II), so 3800 ms is 6.6 s, inside the
 * 8.19 s watchdog period; that period is the "the report never ran" state. Default 4200
 * = no band ever stamped. */
static uint16_t bcMarkMs = 4200u; /* furthest stage reached; unstamped default */

/* The USB clock pair, vendor-shaped: GEN5 <- DFLL48M /1 (halLowLevelInit.c:822-828:
 * GENDIV 0x00000005, then GENCTRL 0x00030705 = ID5, SRC 7, GENEN|IDC), then GCLK_USB
 * (channel ID 6) <- GEN5 with CLKEN (halUsb.c:938 writes CLKCTRL 0x4506, one 16-bit
 * store, immediately before the USBC SWRST). NOT the CPU's generator 0: 13.6.2.3
 * (samr21.txt:5094-5100) makes gen0 GCLK_MAIN, and the app never puts a peripheral on
 * it. Returns 1 when the channel committed, 0 on a GCLK wait timeout, after which no
 * further GCLK core-register write may follow (12.3.2.2 + errata 10416). */
static int clockUsbClockStart(void)
{
    uint32_t flags = GCLK_GENCTRL_IDC; /* the vendor's GENCTRL extras, nothing else */

    if (!clockGenStart(CLK_GEN_USB, GCLK_GENCTRL_SRC_DFLL48M, 1u, 0u, flags, DBG_CLOCK_RSN_GEN5))
        return 0;


    if (clockChannelRoute(GCLK_CLKCTRL_ID_USB, CLK_GEN_USB))
    {
        DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_CH_USB);
        return 1;
    }

    DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_CH_USB_NOT_ROUTED);
    return 0;
}

/* 48 MHz flash wait states. RWS 3, the production value at HAL_48MHz
 * (halLowLevelInit.c:327); our tree used 2, the bootloader's value (conf_clocks.h:53).
 * NVMCTRL CTRLB RWS[3:0] is a plain wait-state count in bits [4:1], other fields
 * preserved (samr21.txt:18281-18284), set before the AHB frequency changes
 * (samr21.txt:17631-17633). */
static void clockFlashWaitStates(void)
{
    const uint16_t ctrlb = SAMR21_NVMCTRL->CTRLB;

    SAMR21_NVMCTRL->CTRLB =
        (uint16_t)((ctrlb & (uint16_t)~NVMCTRL_CTRLB_RWS_MASK) | NVMCTRL_CTRLB_RWS(3));
}

/* ---- Diagnostic breadcrumb ----
 * The CPU frequency as this file reached it: 8 000 000 from step 4 on, 48 000 000 once
 * step 8 committed. Updated at exactly one place (step 8), where a board that did not
 * reach 48 MHz can be told apart from one that did. */
static uint32_t clockCpuHz = 8000000u;

/* Cost of one hold iteration, in core clock cycles. MEASURED: the emulator's icount
 * timeline reports the loop body at 6.004 instructions (ldr/subs/adds/str/cmp/bne,
 * read out of the fault report's hold). A Cortex-M0+ takes at least one cycle per
 * instruction and the loop runs from flash at RWS 3, so a real board's hold is at least
 * the nominal band - the safe direction. */
#define HOLD_CYCLES_PER_ITER 6u
#define HOLD_MEASURED_CYCLES_PER_ITER 6u

/* The constant the hold divides by and the number the emulator measured must be the
 * same number. */
typedef char holdIterationCostMatchesMeasurement
    [(HOLD_CYCLES_PER_ITER == HOLD_MEASURED_CYCLES_PER_ITER) ? 1 : -1];

/* Feed the watchdog every ~this many milliseconds of the hold, so the reset at the end
 * is OUR reset (attributable) and not the watchdog's. Each feed is also a timestamped
 * event in the emulator's trace. */
#define HOLD_FEED_MS 100u

static volatile uint32_t holdSpend; /* volatile: -Os may not delete the delay */

/* The delay primitive of the hold. Unconditional: the fault path uses it too. */
static void clockHoldSpend(uint32_t iters)
{
    while (iters-- != 0u)
        holdSpend++;
}

/*
 * Hold for `ms`, then reset the core. Diagnostic and blunt: a board that would otherwise
 * sit silent leaves a gap of a known length on the bus, then hands itself back to the
 * bootloader window. ok: our descriptor is enumerable for 2000 ms, then gone, then the
 * bootloader window, then our image again. !ok: nothing of ours is ever on the bus -
 * bootloader, 5000 ms absence, bootloader. Both end in HAL_SystemReset(), an enumerable,
 * re-flashable window with nobody touching the stick.
 */
/* The hold itself, shared verbatim by the bring-up trace and the fault path:
 * cycle-counted against the CPU frequency this file actually reached, WDT-fed every
 * ~100 ms, and otherwise silent (no DBG, no queue, no flash) - on the fault path the tail
 * of the fault report's hold must not run. */
static void clockHoldRun(uint16_t ms)
{
    uint32_t cyclesPerMs = clockCpuHz / 1000u; /* 48000 or 8000 */
    uint32_t remain = (uint32_t)ms * cyclesPerMs;
    uint32_t chunk = HOLD_FEED_MS * cyclesPerMs;

    while (remain > chunk)
    {
        clockHoldSpend(chunk / HOLD_CYCLES_PER_ITER);
        remain -= chunk;
        Wdt_Feed();
    }

    clockHoldSpend(remain / HOLD_CYCLES_PER_ITER);
}


/* The core system reset, WITHOUT HAL_SystemReset's Nvram_StatusFlush prefix: on the
 * fault path that row write is the hazard, not the report (a flash command may already be
 * in flight). Same AIRCR VECTKEY|SYSRESETREQ reset the breadcrumb uses, minus the
 * write. */
#define CLOCK_SCB_AIRCR_ADDR 0xE000ED0Cu
#define CLOCK_SCB_AIRCR_RESET 0x05FA0004u

static void clockCoreReset(void)
{
    HAL_Dsb();
    *(volatile uint32_t *)CLOCK_SCB_AIRCR_ADDR = CLOCK_SCB_AIRCR_RESET;
    HAL_Dsb();

    /* Should never run: the store resets the core. If AIRCR were ignored, idling here
   * lets the inherited watchdog reset the board. */
    for (;;)
        HAL_Wfi();
}

/* Fault-path read-out: hold for the band that was reached, then reset the core into the
 * bootloader window. Emits no debug record and writes/flushes nothing: the record
 * machinery ends in a flash write, and re-entering it on top of a half-finished flash
 * command is a second fault inside HardFault, which on a Cortex-M0+ is a Lockup. What
 * the fault path can still say is *time*. */
void Clock_FaultReport(void)
{
    clockHoldRun(bcMarkMs);
    clockCoreReset();
}

/* ---- DIE-REVISION READ-OUT, asked of DSU ----
 * Does CLKM reach generator 1's GCLKIN pad, or generator 5's? Figure 35-1
 * (samr21.txt:44113) draws PC16(F)/CLKM into GCLK GENERATOR 1, whose note 3 (:44145)
 * says "Die revision A uses GCLK GENERATOR 5"; the pin figure's note 4 (:536) and the
 * CLKM row (:856,:867) say revision A uses GCLK_IO[5]; errata 13152 (:57142-57146) says
 * CLKM is connected to GCLK/IO[5] on PC16 and cannot be GCLK_MAIN. 13.6.2.3 (:5091-5095)
 * + Table 13-7: each GCLKGEN[x] has its own GCLK_IO[x] pad, so on a revision-A die
 * `gen1 <- GCLKIN` is aimed at a pad CLKM is not connected to. The die is in
 * DSU.DID.REVISION[11:8] (samr21.txt:3793-3794), readable by the CPU (11.11.2.3).
 *
 * The answer is the LENGTH OF THE HOLD (the bus gap): REVISION 0 -> 1000 ms (revision
 * A), REVISION >= 1 -> 3000 ms (revision B or later, PC16 = GCLK_IO1 -> generator 1),
 * unreadable -> 2000 ms. Records cannot reach the host from here; the gap is the only
 * channel on the board. */
#define CLOCK_UPDATE_FLAG_ADDR 0x00004FFFu
#define CLOCK_UPDATE_FLAG_SET 0xA3u

/* One band per answer (see the buckets above). */
#define CLOCK_DSU_BAND_REV_A 1000u     /* die revision A: CLKM -> GCLK_IO5, gen5 */
#define CLOCK_DSU_BAND_READ_BAD 2000u  /* DSU did not answer: no verdict, fix the read */
#define CLOCK_DSU_BAND_REV_NEWER 3000u /* revision B or later: CLKM -> GCLK_IO1, gen1 */

/* Byte 0x4FFF is the value 0xA3 the GCF BTL writes before jumping to a freshly flashed
 * app (flashLoader.c:277,:286-290). HAL_Init cleared it long before this call, so on this
 * build it reads 0 and the image is boot-independent; the volatile keeps the read. */
static volatile int bisectUpdateFlagWasSet;

/* What DSU said, in statics so it outlives the hold and is readable by anything that can
 * see RAM. The board itself can only say the band. */
static volatile uint32_t dsuDidFirst;  /* DSU.DID @ 0x41002000 + 0x0018, first read */
static volatile uint32_t dsuDidSecond; /* second read: a flaky read is not a revision */
static volatile uint32_t dsuRomPid1;   /* +0x1FE4: 0xFC = Atmel part carrying a DSU */
static volatile uint32_t dsuRomPid2;   /* +0x1FE8: the DSU's own revision, NOT the die's */
static volatile uint32_t dsuRomCid3;   /* +0x1FFC: 0xB1 = Atmel JEP-106 */

void Clock_DieRevisionReadOut(void)
{
    uint32_t band;
    int didSane;

    bisectUpdateFlagWasSet =
        ((*(const volatile uint8_t *)CLOCK_UPDATE_FLAG_ADDR) == CLOCK_UPDATE_FLAG_SET) ? 1 : 0;

    /* The update-flag clear stays FIRST, ahead of any hold: an uncleared flag makes the
   * bootloader erase app page 0 at the next boot (eternal window). Already cleared by
   * HAL_Init, so this is a row read, no NVM command. */
    Nvram_ClearUpdateFlag();

    /* DSU has no handshake and no synchronisation window: it is not one of the
   * common-synchronizer peripherals (12.3, :4585-4591), it has no SYNCBUSY, and DID is a
   * plain R/32 (Table 11-7, samr21.txt:3229-3233). DID is read twice: two reads that
   * disagree are not a fact about the die. PID1/CID3 anchor that 0x41002000 is the block
   * we think it is (samr21.txt:4110,:4152-4156,:4444). */
    dsuDidFirst = SAMR21_DSU->DID;
    dsuRomPid1 = DSU_ROM_PID1;
    dsuRomPid2 = DSU_ROM_PID2;
    dsuRomCid3 = DSU_ROM_CID3;
    dsuDidSecond = SAMR21_DSU->DID;

    didSane =
        (dsuDidFirst != 0u && dsuDidFirst != 0xFFFFFFFFu && dsuDidFirst == dsuDidSecond) ? 1 : 0;

    if (!didSane)
    {
        band = CLOCK_DSU_BAND_READ_BAD; /* ~3.35 s: the read path is what needs fixing */
        DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_DSU_READ_BAD);
    }
    else if (DSU_DID_REVISION(dsuDidFirst) == 0u)
    {
        band = CLOCK_DSU_BAND_REV_A; /* ~1.90 s: die revision A */
        DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_DSU_REV_A);
    }
    else
    {
        band = CLOCK_DSU_BAND_REV_NEWER; /* ~4.80 s: revision B or later */
        DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_DSU_REV_NEWER);
    }

    clockHoldRun(band);
    clockCoreReset(); /* does not return */
}

/* ---- The candidate fix, put to the test ----
 * That session settled which generator CLKM reaches (DID.REVISION >= 1 -> generator 1) and
 * that the released vendor image writes GENCTRL = 0x10101 in ONE store, no byte-select,
 * no `|=` read-modify-write. The standing candidate for the `gen1 <- GCLKIN` stall was
 * the shape of our clockGenStart, now rewritten to the vendor's. The hold instrument was
 * used at two bands and is not called in the plain fixed tree: band 2200 ms after
 * clockGenStart(gen1 <- GCLKIN) returned -> 3.67 s on 27/27 boots; band 3400 ms after
 * clockChannelRoute(ch0 <- gen1) committed -> never observed, 13/13 boots sat at
 * 10.03-10.56 s (the unfed-watchdog cluster). */
#define CLOCK_PROBE_BAND_GEN1_COMPLETES 2200u
#define CLOCK_PROBE_BAND_REFCH_COMPLETES 3400u

/* 8-vs-48 discriminator: the WDT is fed only while the 48 MHz bring-up succeeded, so a
 * failed bring-up WDT-resets instead of sitting as an invisible zombie. */
static int wdtFeedGate;

static void systickInit(uint32_t cpuHz);

int Clock_Init(void)
{
    /* Return codes (hal.h): 1 = 48 MHz closed loop (the vendor regime, locked), 2 = 48 MHz
   * on the USBCRM free-run (the fallback), 0 = still on the 8 MHz OSC8M. The terminal
   * scratch mark names the regime: 08 = closed loop, 09 = fallback (hal_samr21.c). */
    int ok = 1;
    /* Set as soon as a GCLK wait times out: that block is frozen, so the only GCLK writes
   * left are the ones that would hang the core. */
    int gclkStuck = 0;
    /* Whether the CLKM pad is *proven*: the return value of three read-backs - PART_NUM
   * 0x0B, TRX_STATUS holding 0x08, TRX_CTRL_0 holding the written 0x04 (at86rf233 9.6.4
   * note 1, at86rf233.txt:13881-13895). Gates step 8 and the closed-loop guard. */
    int clkmForced = 0;
    /* The closed loop's lock, LCKC and LCKF both seen inside CLK_LOCK_LIMIT: decides
   * whether the CPU switch and the USB generator are pointed at a locked clock or a
   * free-run. */
    int locked = 0;

    /* 1. Clear the stale SYSCTRL flags BOD33RDY|BOD33DET and all three DFLL flags
   * (DFLLRDY|DFLLLCKF|DFLLLCKC = the 0xD0 the released app writes before a closed-loop
   * start, halLowLevelInit.c:867-868): the PCLKSR status bits latch from the INTFLAG
   * domain, so the waits below are meaningful only from a cleared flag. Same state the BTL
   * leaves (clock.c:1049-1050). */
    Clock_Stamp(DBG_TIME_RSN_ENTRY);

    SAMR21_SYSCTRL->INTFLAG = SYSCTRL_INTFLAG_BOD33RDY | SYSCTRL_INTFLAG_BOD33DET |
                              SYSCTRL_INTFLAG_DFLLRDY | SYSCTRL_INTFLAG_DFLLLCKF |
                              SYSCTRL_INTFLAG_DFLLLCKC;

    /* 2. Flash wait states 3, the production 48 MHz value (halLowLevelInit.c:327), before
   * the CPU switch. */
    clockFlashWaitStates();

    /* 3. OSC32K (BTL: the CALIB write, then the config, then enable). */
    SAMR21_SYSCTRL->OSC32K =
        SAMR21_SYSCTRL->OSC32K | SYSCTRL_OSC32K_CALIB(FUSE_OSC32K_CAL(FUSES_OTP4_WORD));
    SAMR21_SYSCTRL->OSC32K = SAMR21_SYSCTRL->OSC32K | SYSCTRL_OSC32K_EN1K | SYSCTRL_OSC32K_EN32K |
                             SYSCTRL_OSC32K_STARTUP(1u) | SYSCTRL_OSC32K_ONDEMAND |
                             SYSCTRL_OSC32K_RUNSTDBY;
    SAMR21_SYSCTRL->OSC32K |= SYSCTRL_OSC32K_ENABLE;

    /* 4. OSC8M: PRESC 0 (the reset 3 is /8), ONDEMAND (as the BTL, conf_clocks.h:61),
   * enable (halLowLevelInit.c:331-333). SYSCTRL writes are plain APB: they commit. */
    SAMR21_SYSCTRL->OSC8M =
        (SAMR21_SYSCTRL->OSC8M &
         (uint32_t)~(SYSCTRL_OSC8M_PRESC(0x3u) | SYSCTRL_OSC8M_RUNSTDBY | SYSCTRL_OSC8M_ONDEMAND)) |
        SYSCTRL_OSC8M_PRESC(0u) | SYSCTRL_OSC8M_ONDEMAND;
    SAMR21_SYSCTRL->OSC8M |= SYSCTRL_OSC8M_ENABLE;

    /* 5. GEN0 <- OSC8M /1 IN PLACE (halLowLevelInit.c:336-343), the first generator action
   * and the replacement for the old GCLK SWRST: no vendor source ever writes
   * GCLK_CTRL.SWRST, and SWRST was what put the watchdog's channel back on GEN 2 and
   * switched GEN1/4/5/6 off. Re-driving the generator the CPU runs on gets the same
   * known-good state without taking anything else down. The GCLK APBA gate is written
   * rather than inherited (APBAMASK reset 0x7F already has it); OSC8MRDY is waited on
   * after the generator write, in the vendor's order. */
    SAMR21_PM->APBAMASK |= PM_APBAMASK_GCLK;

    if (!clockGenStart(
            CLK_GEN_CPU, GCLK_GENCTRL_SRC_OSC8M, 1u, 0u, GCLK_GENCTRL_RUNSTDBY, DBG_CLOCK_RSN_GEN0))
    {
        ok = 0;
        gclkStuck = 1;
    }
    else if (!clockWaitOsc8mRdy())
    {
        /* OSC8M itself did not report ready: the CPU is fetching from it, but nothing
     * further is promised. Not a stuck block, so the probe and DFLL leg still run. */
        DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_GEN0);
    }

    /* Stage 03 = "GEN0 is ours and the watchdog's clock was looked at"; stage 04 is the
   * closed-loop lock (LCKC+LCKF), written at step 9c. */
    if (ok)
        Nvram_StatusStage(NVRAM_STG_CLOCKS_GEN);

    /* 6. The watchdog: probe, do not rebuild. See clockWdtChannelProbe. */
    if (!gclkStuck)
    {
        Clock_Probe probe = clockWdtChannelProbe();

        if (probe == CLOCK_PROBE_FOREIGN)
        {
            if (!clockWdtClockStart())
            {
                gclkStuck = 1;
                ok = 0;
            }
        }
        else if (probe == CLOCK_PROBE_STUCK)
        {
            /* The window did not settle: 12.3.2.2 discards a further write and errata 10416
       * stalls the core on it, and the watchdog may have lost its clock too. No GCLK write
       * may follow, so the CPU switch and peripheral channels are skipped and the recovery
       * reset brings the bootloader window back. */
            gclkStuck = 1;
            ok = 0;
        }
        /* CLOCK_PROBE_INHERITED: nothing is written. gen4, ch3 and everything the
     * bootloader left running stay as they are. */
    }

    Clock_Stamp(DBG_TIME_RSN_WDT_PROBE);

    /* 7. Wake the RF233 and put CLKM at the vendor's 8 MHz: phySetFreq() writes TRX_CTRL_0
   * with clkmCtrl = CLK_CTRL_8MHZ (phyInit.c:129, at86rf233.h:203). It must precede the
   * GCLKIN probe - it gives the reference a frequency at all, and CLKM_CTRL is shadowed
   * across the radio's own reset, so the pad rate is not readable (at86rf233 9.6.4
   * note 1). `clkmForced` gates the whole step-8 chain: pointing a generator at a pad
   * nobody has proven to have edges is exactly the write this board stalls on. An
   * unproven wake records DBG_CLOCK_RSN_GCLKIN_UNPROVEN and hands the tree to the USBCRM
   * fallback. */
    if (!gclkStuck)
    {
        clkmForced = Rf_WakeAndForceClkm();

        if (!clkmForced)
            DBG(DBG_COMP_RF, DBG_RF_RSN_WAKE_FAIL);
        else
        {
        }
    }

    Clock_Stamp(DBG_TIME_RSN_WAKE);

    /* 8. The DFLL reference chain the released app builds in halRouteClkm: PC16 -> PORT
   * function 5, generator 1 <- GCLKIN with GENDIV.DIV 250 and DIVSEL 0 (0x0000FA01 then
   * 0x00010101 - GENEN only, no OE, no RUNSTDBY, no IDC), 8 MHz / 250 = 32 000 Hz, then
   * ch0 GCLK_DFLL48M_REF <- gen1 (CLKCTRL 0x4100). clockGenStart is both the liveness
   * probe and the commit proof: a pad with no edges never completes the sync. GATED on
   * `clkmForced`: SRC = GCLKIN selects the input pad (Table 13-7, samr21.txt:5752-5758)
   * on PC16/GCLK_IO1 (13.6.2.3, :5094-5095). Skipping step 8 costs the closed loop, not
   * the board: 9a refuses the reference and 9d brings up 48 MHz on USBCRM. */
    if (!gclkStuck && clkmForced)
    {
        clockGclkInPin();
        DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_PC16_MUX);

        /* The die-revision read-out is NOT re-run here: it answered (REVISION >= 1, so
     * gen1 is the right generator), and its clockCoreReset() would make the write below
     * unreachable. */

        if (!clockGenStart(
                CLK_GEN_REF, GCLK_GENCTRL_SRC_GCLKIN, CLK_GEN_REF_DIV, 0u, 0u, DBG_CLOCK_RSN_GEN1))
        {
            ok = 0;
            gclkStuck = 1; /* the sync did not settle: no further GCLK write may follow */
        }
#ifdef RCP_TX_TIMING
        else if (!clockGenStart(CLK_GEN_DFLL_REF, GCLK_GENCTRL_SRC_GCLKGEN1, CLK_DFLL_REF_DIV, 0u, 0u, DBG_CLOCK_RSN_GEN1))
        {
            ok = 0;
            gclkStuck = 1;
        }
        else
        {
            clockCrystalUp = 1;
        }
#endif
    }
    else if (!gclkStuck)
    {
        /* The pad is not proven, so no generator may be pointed at GCLKIN. `ok` is untouched
     * (the fallback is a real 48 MHz) and ch0 <- gen1 is skipped; USBCRM ignores the
     * selected reference anyway (15.6.7.1, samr21.txt:7992-7993). */
        DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_GCLKIN_UNPROVEN);
    }

    if (ok && clkmForced && !clockRefChannelVendor())
    {
        /* ch0 did not commit with GEN+CLKEN readable: the DFLL's GCLK reference is dead
     * (USBCRM does not care, 15.6.7.1, samr21.txt:7992-7993), but it is a timed-out GCLK
     * synchronisation, so no further GCLK write may follow. */
        DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_DFLL_REF);
        ok = 0;
        gclkStuck = 1;
    }
    else if (ok && clkmForced)
    {
        DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_DFLL_REF); /* the settle line */
    }

    Clock_Stamp(DBG_TIME_RSN_REF_CHAIN);

    /* 9. The DFLL48M: primary = the released app's closed loop (9b), fallback = USBCRM
   * (9d). The guard runs first so an illegal reference x MUL can never be programmed
   * (32 000 x 1500 and 31 250 x 1536 are the same 48 MHz; 8 000 000/32 or 1 000 000/250
   * are not). */
    locked = 0;

    /* 9a. Reference guard (runtime half; the build-time half is the typedef above). */
    if (ok && !clockDfllReferenceOk(clkmForced))
        DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_DFLL_REF_OUT);

    /* 9b. Closed loop: INTFLAG 0xD0, DFLLCTRL 0, DFLLMUL 1500|63|7, no DFLLVAL,
   * DFLLCTRL ENABLE|MODE (halLowLevelInit.c:867-892). */
    if (ok && clockDfllReferenceOk(clkmForced) && clockDfllClosedLoop(DBG_CLOCK_RSN_DFLL_ON))
    {

        /* 9c. The lock, coarse then fine, bounded by CLK_LOCK_LIMIT instead of the vendor's
     * `while`. Datasheet tLOCK 100 us typ / 500 us max (Table 42-40,
     * samr21.txt:54930-54936); a lock not arrived in a few ms is not arriving. */
        if (clockWaitDfllLock())
        {
            locked = 1;
            Nvram_StatusStage(NVRAM_STG_CLOCKS_DFLL); /* stage 04 = LCKC+LCKF held */
        }
    }

    /* 9d. Fallback, never the primary: the USBCRM free-run the GCF bootloader itself runs
   * on this PCB. Reached only when the closed loop did not configure or did not lock. */
    if (ok && !locked)
    {
        if (!clockDfllUsbcrm(DBG_CLOCK_RSN_DFLL_USBCRM))
        {
            ok = 0; /* SYSCTRL itself did not answer: no 48 MHz in either regime */
        }
        else
        {
            Nvram_StatusStage(NVRAM_STG_CLOCKS_USBCRM);
        }
    }

    Clock_Stamp(DBG_TIME_RSN_DFLL_DONE);

    /* 10. GEN0 <- DFLL48M /1 with IDC, nothing else (halLowLevelInit.c:799 writes GENCTRL
   * 0x00030700 = ID0|SRC(DFLL48M)|GENEN|IDC), reached only after halCoarseLock/halFineLock,
   * i.e. the CPU is repointed at an already locked generator. 13.6.2.6
   * (samr21.txt:5137-5141): "If clock source B is not ready, the generic clock generator
   * will continue running with clock source A" - so a switch issued before a lock cannot be
   * told from one that worked, and the commit readback is the only evidence. Flash wait
   * states have been 3 since step 2. */
    if (ok && !clockGenStart(CLK_GEN_CPU,
                             GCLK_GENCTRL_SRC_DFLL48M,
                             1u,
                             0u,
                             GCLK_GENCTRL_IDC,
                             DBG_CLOCK_RSN_CPU_48MHZ))
    {
        ok = 0;
        gclkStuck = 1;
    }
    else if (ok)
    {
        clockCpuHz = CPU_FREQ_HZ; /* the breadcrumb's clock, and SysTick's */
        ok = locked ? 1 : 2;      /* 1 = locked closed loop, 2 = USBCRM free-run */
    }

    Clock_Stamp(DBG_TIME_RSN_CPU_SWITCH);

    /* 9. WDT arm-only safety net (the BTL's hwWdtInit): PER 11 + ALWAYON when
   * ALWAYON is not already latched, then feed. NOT the channel: step 6 decided. */
    if (ok)
        clockWdtArm();

    /* 10b. GEN5 <- DFLL48M /1 then GCLK_USB <- gen5 (halLowLevelInit.c:822-828 +
   * halUsb.c:938), before Usbc_Init's own USBC SWRST. Non-fatal: a generator or channel
   * that does not commit degrades that peripheral, never the CPU clock, and this is the
   * LAST GCLK work Clock_Init does, so a frozen block here cannot be made worse. */
    if (ok)
        (void)clockUsbClockStart();

    Clock_Stamp(DBG_TIME_RSN_USB_CLOCK);

    /* 11. 1 ms tick either way; on failure the app lives (silently) at 8 MHz and stops
   * feeding the WDT from the tick (the wdtFeedGate discriminator), so the 8 MHz case ends
   * in a known gap instead of an 8 s watchdog bite. */
    wdtFeedGate = ok;

    /* Frozen GCLK block: no clock work can succeed, the watchdog may have lost its clock,
   * and an 8 MHz app without USB is unreachable without a power cycle. A core system reset
   * re-runs the GCF bootloader, whose window is enumerable and re-flashable (measured: 14
   * windows in a row). */
    if (gclkStuck)
    {
        DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_RECOVERY_RESET);
        HAL_SystemReset();
    }

    systickInit(ok ? CPU_FREQ_HZ : 8000000u);
    Wdt_Feed();

    if (ok)
        Nvram_StatusStage(NVRAM_STG_CLOCKS_DONE);

    Clock_Stamp(DBG_TIME_RSN_CLOCKS_EXIT);

    return ok;
}

/* ---- SysTick: 1 ms tick, feeds the watchdog once per second ----
 * tickMs is free-running (monotonic ms since boot); the 1 s watchdog feed keeps its own
 * 0..999 counter, so no modulo (the M0+ has no hardware divide, `tickMs % 1000` would pull
 * a libgcc helper into every tick). tickMs wraps after 2^32 ms = 49.7 d; consumers compare
 * modularly or convert to seconds where stored. */
static uint32_t tickMs;      /* monotonic ms since boot */
#ifdef RCP_TX_TIMING
static int clockCounterUp; /* the radio clock counts the crystal */
#endif
static uint32_t tickMsOfSec; /* ms counted towards the 1 s WDT feed */

static void systickInit(uint32_t cpuHz)
{
    /* LOAD is 24-bit: 48 MHz / 1 kHz - 1 fits; 8 MHz / 1 kHz - 1 too. */
    SAMR21_SYSTICK->LOAD = (cpuHz / TICK_HZ) - 1;
    SAMR21_SYSTICK->VAL = 0;
    SAMR21_SYSTICK->CTRL = SYSTICK_CTRL_CLKSRC | SYSTICK_CTRL_TICKINT | SYSTICK_CTRL_ENABLE;
}

void SysTick_Handler(void)
{
    /* Bracketed like every other vector: the HAL's handler-mode depth is what
   * makes Nvram_StatusStage() queue instead of programming flash here. */
    HAL_IrqEnter();
    /* IRQ critical section: nothing below may preempt itself. */
    HAL_IrqLock();
    /* A SysTick vector IS one LOAD+1 processor-clock period, so credit a whole period
   * instead of measuring VAL: a pump at the wrap sees a VAL delta of ~0 (the sawtooth
   * sampled at the same phase), losing the duration it is synchronized to. The baseline is
   * re-taken after the credit so finer pumps still measure their own partial deltas. */
    Clock_BringupTick();
    tickMs++;

    if (++tickMsOfSec >= 1000u)
    {
        tickMsOfSec = 0;

        if (wdtFeedGate)
            Wdt_Feed();
    }

    HAL_IrqUnlock();
    HAL_IrqExit();
}

void Tick_GetMs(uint32_t *ms)
{
    *ms = tickMs;
}

uint32_t Tick_CycleStamp(void)
{
    return SAMR21_SYSTICK->VAL & SYSTICK_COUNT_MASK;
}

/* VAL counts down from LOAD and reloads, so the elapsed count is stamp - now, plus one period
 * when the counter reloaded in between. */
uint32_t Tick_CyclesSince(uint32_t stamp)
{
    uint32_t now = SAMR21_SYSTICK->VAL & SYSTICK_COUNT_MASK;

    if (stamp >= now)
        return stamp - now;

    return stamp + ((SAMR21_SYSTICK->LOAD & SYSTICK_COUNT_MASK) + 1u) - now;
}

#ifdef RCP_TX_TIMING
/* TC4 and TC5 paired as one 32-bit counter on generator 1, so it counts the crystal, undivided from there.
 * With continuous synchronisation a read returns the latest count at once. Called once the tree runs. A
 * tree that fell back to USB clock recovery has no CLKM to count, and the clock is the tick's. */
void Tick_ClockStart(void)
{
    uint32_t n = 0u;

    if (!clockCrystalUp)
        return;

    SAMR21_PM->APBCMASK |= PM_APBCMASK_TC4 | PM_APBCMASK_TC5;

    if (!clockChannelRoute(GCLK_CLKCTRL_ID_TC4_TC5, CLK_GEN_REF))
        return;

    SAMR21_TC4->CTRLA = TC_CTRLA_MODE_COUNT32 | TC_CTRLA_ENABLE;

    while (SAMR21_TC4->STATUS & TC_STATUS_SYNCBUSY)
    {
        if (++n >= CLK_WAIT_LIMIT)
            return;
    }

    SAMR21_TC4->READREQ = TC_READREQ_RCONT | TC_READREQ_RREQ | TC_READREQ_ADDR_COUNT;
    clockCounterUp = 1;
}

uint32_t Tick_ClockUs(void)
{
    return clockCounterUp ? SAMR21_TC4->COUNT : tickMs * 1000u;
}
#endif

void Delay_Ms(uint32_t ms)
{
    uint32_t end = tickMs + ms;
    /* Modular compare: survives a 32-bit ms wrap mid-delay. */
    while ((int32_t)(tickMs - end) < 0)
    {
    }
}
