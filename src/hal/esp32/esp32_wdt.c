/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* esp32_wdt.c — the watchdogs an ESP32 app inherits, and the one it keeps (TRM ch. 15).

The ROM arms the RTC watchdog and MWDT0 in flash-boot mode; ESP-IDF's bootloader ends both, and by its
default re-arms the RTC watchdog with a 9 s reset for the app to take over (docs/hardware_esp32.md §12.3).
This image switches the RTC watchdog off and keeps MWDT0, the timer group's watchdog: stage 0 resets the
system after ESP32_WDT_HOLD_MS, counted in milliseconds from the crystal (the timer group's default
watchdog clock, 40 MHz divided by 40 000). The super watchdog is left as the bootloader set it, fed by
the hardware.

Every write to a watchdog is preceded by its write key and followed by locking it again (TRM 15.2.2.3);
a configuration change lands with CONF_UPDATE_EN. */
#include <stdint.h>

#include "dbg.h"
#include "esp32_regs.h"
#include "esp32_wdt.h"
#include "hal.h"

#define TIMG0(offset) ESP32_REG(ESP32_TIMG0_BASE, (offset))
#define LP_WDT(offset) ESP32_REG(ESP32_LP_WDT_BASE, (offset))

#define WDT_PRESCALE (ESP32_XTAL_HZ / 1000u)
/* The reset pulse lengths, 3.2 us each: the value ESP-IDF's watchdog driver programs. */
#define WDT_RESET_LENGTH 7u

static int wdtLooping;

int Wdt_Init(void)
{
    uint32_t rwdt;

    rwdt = LP_WDT(ESP32_LP_WDT_CONFIG0);
    LP_WDT(ESP32_LP_WDT_WPROTECT) = ESP32_WDT_WKEY;
    LP_WDT(ESP32_LP_WDT_CONFIG0) = rwdt & ~(ESP32_LP_WDT_CONFIG0_EN | ESP32_LP_WDT_CONFIG0_FLASHBOOT_MOD_EN);
    LP_WDT(ESP32_LP_WDT_WPROTECT) = 0u;

    if ((rwdt & ESP32_LP_WDT_CONFIG0_EN) != 0u)
        Debug_Emit(DBG_COMP_WDT, (uint16_t)(rwdt >> 16), DBG_WDT_RSN_ESP32_RWDT_OFF);

    TIMG0(ESP32_TIMG_WDTWPROTECT) = ESP32_WDT_WKEY;
    TIMG0(ESP32_TIMG_WDTCONFIG0) = ESP32_TIMG_WDTCONFIG0_CONF_UPDATE_EN;
    TIMG0(ESP32_TIMG_WDTCONFIG1) = (WDT_PRESCALE << ESP32_TIMG_WDTCONFIG1_CLK_PRESCALE_SHIFT)
                                   | ESP32_TIMG_WDTCONFIG1_DIVCNT_RST;
    TIMG0(ESP32_TIMG_WDTCONFIG2) = ESP32_WDT_HOLD_MS;
    TIMG0(ESP32_TIMG_WDTCONFIG0) = ESP32_TIMG_WDTCONFIG0_EN
                                   | (ESP32_WDT_STAGE_RESET_SYSTEM << ESP32_TIMG_WDTCONFIG0_STG0_SHIFT)
                                   | (WDT_RESET_LENGTH << ESP32_TIMG_WDTCONFIG0_SYS_RESET_LENGTH_SHIFT)
                                   | (WDT_RESET_LENGTH << ESP32_TIMG_WDTCONFIG0_CPU_RESET_LENGTH_SHIFT)
                                   | ESP32_TIMG_WDTCONFIG0_CONF_UPDATE_EN;
    TIMG0(ESP32_TIMG_WDTFEED) = 1u;
    TIMG0(ESP32_TIMG_WDTWPROTECT) = 0u;

    Debug_Emit(DBG_COMP_WDT, (uint16_t)ESP32_WDT_HOLD_MS, DBG_WDT_RSN_ARMED);
    return 1;
}

void Wdt_Feed(void)
{
    TIMG0(ESP32_TIMG_WDTWPROTECT) = ESP32_WDT_WKEY;
    TIMG0(ESP32_TIMG_WDTFEED) = 1u;
    TIMG0(ESP32_TIMG_WDTWPROTECT) = 0u;
}

void Wdt_LoopStart(void)
{
    wdtLooping = 1;
}

void Wdt_FeedPoll(uint32_t round)
{
    if (!wdtLooping && (round & (ESP32_WDT_FEED_EVERY - 1u)) == (ESP32_WDT_FEED_EVERY - 1u))
        Wdt_Feed();
}
