/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mg21_wdt.c — arming the ConBee III watchdog to a period this image can feed.
 *
 * Glue only (docs/coding-rules.md §4): the period is arithmetic already pinned by the mg21_regs
 * suite, and what is left here is the order the part wants (reference manual 26.4/26.5, and the
 * clock select in 8.4): stop the counter before rewriting its configuration, name the clock in CMU
 * through the watchdog lock, then enable and feed.
 *
 * The bootloader armed WDOG0 at 8 s and never disables it, so the counter is running before this
 * image is entered; the watchdog this board runs under is the one written here (docs/hardware_mg21.md §10.7). */
#include <stdint.h>

#include "dbg.h"
#include "hal.h"
#include "mg21_regs.h"
#include "mg21_wdt.h"

/* A clock-domain crossing on this part is short; a wait that outlasts this many polls means the
 * watchdog clock select never took, which is a clock problem, not a watchdog one. */
#define WDT_SYNC_LIMIT 0x4000u

static uint32_t wdtPeriodSel;
static int wdtArmed;
/* 1 once the main loop runs (hal/hal.h); read from vectors too. */
static volatile int wdtLooping;

/* 1 when WDOG0->SYNCBUSY has cleared every bit in `mask` inside the limit. */
static int wdtWaitSync(uint32_t mask, uint16_t rsn)
{
    uint32_t n;

    for (n = 0; n < WDT_SYNC_LIMIT; ++n)
    {
        if ((MG21_WDOG0->SYNCBUSY & mask) == 0u)
            return 1;

        Wdt_FeedPoll(n);
    }

    DBG(DBG_COMP_WDT, rsn);
    return 0;
}

int Wdt_Init(void)
{
    uint32_t cfg;
    int onLowClock;

    if (!wdtWaitSync(MG21_WDOG_SYNCBUSY_CMD, DBG_WDT_RSN_SYNC_TIMEOUT))
        return 0;

    /* LFRCO is the part's own selection for WDOG0CLKCTRL and is what makes the 2 s period 2 s. It
     * has no enable bit in this generation (docs/hardware_mg21.md §10.3): it runs when something
     * asks for it, so RDY is the proof that something asked. The thing asking is the bootloader's
     * watchdog, which runs on LFRCO, so RDY is read here, while that watchdog is still counting. ULFRCO
     * is always on and 32 times slower, which is one selection lower for the same two seconds. */
    onLowClock = ((MG21_LFRCO->STATUS & MG21_LFRCO_STATUS_RDY) != 0u) ? 1 : 0;

    /* Stop the counter before its clock select changes: the bootloader's watchdog is running, and the
     * part promises nothing for a clock switched under a running counter. */
    MG21_WDOG0->LOCK = MG21_WDOG_LOCK_LOCKKEY_UNLOCK;

    if (MG21_WDOG0->EN != 0u)
    {
        if (!wdtWaitSync(MG21_WDOG0->SYNCBUSY, DBG_WDT_RSN_SYNC_TIMEOUT))
            return 0;

        MG21_WDOG0->EN = 0u;
    }

    MG21_CMU->WDOGLOCK = MG21_CMU_LOCKKEY_UNLOCK;

    if (onLowClock != 0)
    {
        MG21_CMU->WDOG0CLKCTRL = MG21_CMU_WDOG0CLKCTRL_CLKSEL_LFRCO;
        wdtPeriodSel = MG21_WDOG_PERSEL_2S;
    }
    else
    {
        MG21_CMU->WDOG0CLKCTRL = MG21_CMU_WDOG0CLKCTRL_CLKSEL_ULFRCO;
        wdtPeriodSel = MG21_WDOG_PERSEL_2S_ULFRCO;
        DBG(DBG_COMP_WDT, DBG_WDT_RSN_CLOCK_FALLBACK);
    }

    /* A timeout resets the core (WDOGRSTDIS clear): that reset is this board's recovery path, the
     * same one a hung ConBee II gets back into the bootloader window with. Running in EM2 keeps it
     * alive while the image waits for the host; blocking in EM4 keeps it from resetting a board that
     * is being powered down. */
    cfg = MG21_WDOG_CFG_EM2RUN | MG21_WDOG_CFG_EM4BLOCK |
          (wdtPeriodSel << MG21_WDOG_CFG_PERSEL_SHIFT);
    MG21_WDOG0->CFG = cfg;
    MG21_WDOG0->CMD = MG21_WDOG_CMD_CLEAR;
    MG21_WDOG0->EN = MG21_WDOG_EN_EN;

    if (!wdtWaitSync(MG21_WDOG_SYNCBUSY_CMD, DBG_WDT_RSN_SYNC_TIMEOUT))
        return 0;

    wdtArmed = 1;
    Debug_Emit(DBG_COMP_WDT, (uint16_t)wdtPeriodSel, DBG_WDT_RSN_ARMED);
    return 1;
}

uint32_t Wdt_PeriodSel(void)
{
    return wdtPeriodSel;
}

int Wdt_IsArmed(void)
{
    return wdtArmed;
}

/* A feed already crossing into the watchdog's clock domain restarts the period when it lands, so a
 * second one is skipped rather than queued (26.5). The counter itself is not stalled by this. */
void Wdt_Feed(void)
{
    if ((MG21_WDOG0->SYNCBUSY & MG21_WDOG_SYNCBUSY_CMD) != 0u)
        return;

    MG21_WDOG0->CMD = MG21_WDOG_CMD_CLEAR;
}

void Wdt_LoopStart(void)
{
    wdtLooping = 1;
}

/* Only while booting (hal/hal.h). */
void Wdt_FeedPoll(uint32_t round)
{
    if (!wdtLooping && (round & (MG21_WDT_FEED_EVERY - 1u)) == (MG21_WDT_FEED_EVERY - 1u))
        Wdt_Feed();
}
