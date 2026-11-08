/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* avr_wdt.h — the ConBee I / RaspBee I watchdog's board-private contract.
 *
 * `Wdt_Feed()` is in hal/hal.h because every bounded wait and the main loop call it through the
 * SoC-services contract. What is this board's own: the arming, and the feed pacing of a bounded wait.
 *
 * The RaspBee I bootloader switches the watchdog off when it finds the watchdog reset flag, and never
 * arms it before the jump otherwise (docs/hardware_avr.md §11.2), so this image starts unprotected
 * and arms its own. A watchdog reset re-enters the bootloader, which is also how HAL_SystemReset
 * leaves (hal/avr/hal_avr.c). */
#ifndef AVR_WDT_H
#define AVR_WDT_H

#include <stdint.h>

/* Feed once every 64 rounds of a bounded wait, the way the other boards pace it. */
#define AVR_WDT_FEED_EVERY 64u

/* 1 = armed at 2 s and read back so; 0 = the timed write did not take and the image runs unprotected,
 * which the DBG_WDT_RSN_AVR_REFUSED record says. */
int Wdt_Init(void);

/* Bounded-wait feeding, only until Wdt_LoopStart (hal/hal.h). */
void Wdt_FeedPoll(uint32_t round);

/* The shortest period, about 16 ms, and no feeding from here on: the reset path. Never returns. */
void Wdt_ResetNow(void);

#endif /* AVR_WDT_H */
