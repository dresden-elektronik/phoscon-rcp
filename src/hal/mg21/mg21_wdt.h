/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mg21_wdt.h — the ConBee III watchdog's board-private contract.
 *
 * `Wdt_Feed()` is in hal/hal.h because every bounded wait and the main loop call it through the
 * SoC-services contract. What is this board's own: the arming, and which clock the arming ended on.
 *
 * The shipped bootloader arms WDOG0 at its own 8 s period and never disables it
 * (the deSiLabs bootloader's de_BTL_wdt.c), so an application here
 * inherits a running watchdog and re-arms it to a period it can actually feed (docs/hardware_mg21.md §10.7). */
#ifndef MG21_WDT_H
#define MG21_WDT_H

#include <stdint.h>

#include "mg21_regs.h"

/* Feed once every 64 rounds of a bounded wait, the way the SAMR21 side paces its feeds. */
#define MG21_WDT_FEED_EVERY 64u

/* 1 = armed and counting; 0 = the register crossings did not complete, and the image is running
 * unprotected, which is a condition the caller has to be able to see. */
int Wdt_Init(void);

/* 1 when the watchdog is armed and counting. */
int Wdt_IsArmed(void);

/* The timeout selection in force: MG21_WDOG_PERSEL_2S on LFRCO, or MG21_WDOG_PERSEL_2S_ULFRCO when
 * LFRCO never reported ready. */
uint32_t Wdt_PeriodSel(void);

/* Bounded-wait feeding. */
void Wdt_FeedPoll(uint32_t round);

#endif /* MG21_WDT_H */
