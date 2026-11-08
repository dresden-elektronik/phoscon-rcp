/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* The bootloader starts the watchdog with ALWAYSON set; it cannot be disabled
 * on the SAMR21, so a hung app is reset back into the bootloader window. */
#include "dbg.h"
#include "hal.h"
#include "hal_diag.h"
#include "samr21_regs.h"

/* CLEAR is write-synchronised to the slow watchdog clock, and a write issued while the
 * previous one is still synchronising stalls the bus until it completes: "the CPU will be
 * stalled and interrupts will be pending as long as the bus is stalled" (samr21 datasheet
 * 16.6.5). That stall is the ~21 000 cycles a back-to-back feed costs (docs/hardware_samr21.md §4), and
 * it blocks every vector, the radio's included. A feed still in flight restarts the period
 * when it lands, so a busy feed is skipped; STATUS is not synchronised and reads at bus speed. */
void Wdt_Feed(void)
{
    if (SAMR21_WDT->STATUS & WDT_STATUS_SYNCBUSY)
        return;

    SAMR21_WDT->CLEAR = WDT_CLEAR;
}

/* 1 once the main loop runs (hal/hal.h); read from vectors too. */
static volatile int wdtLooping;

void Wdt_LoopStart(void)
{
    wdtLooping = 1;
}

/* Bounded waits feed every 64th round, not every poll, and only while booting. */
void Wdt_FeedPoll(uint32_t round)
{
    if (!wdtLooping && (round & 0x3Fu) == 0x3Fu)
        Wdt_Feed();
}
