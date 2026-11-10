/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* esp32_wdt.h — the ESP32 family's watchdog, the board-private half of hal/hal.h's Wdt_* contract. */
#ifndef ESP32_WDT_H
#define ESP32_WDT_H

#include <stdint.h>

/* Feed once every 64 rounds of a bounded wait, the pace of the other boards. */
#define ESP32_WDT_FEED_EVERY 64u

/* Take the RTC watchdog the bootloader may have left armed off, and arm MWDT0 to reset the system after
 * ESP32_WDT_HOLD_MS without a feed. 1 = armed. */
#define ESP32_WDT_HOLD_MS 2000u
int Wdt_Init(void);

/* Bounded-wait feeding, only until Wdt_LoopStart (hal/hal.h). */
void Wdt_FeedPoll(uint32_t round);

#endif /* ESP32_WDT_H */
