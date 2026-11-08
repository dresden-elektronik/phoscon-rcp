/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* avr_wdt.c — arming the ConBee I / RaspBee I watchdog, feeding it, and resetting with it.
 *
 * Glue only (docs/coding-rules.md §4). WDTCSR takes a new prescaler or a cleared WDE only within four
 * cycles of a store with WDCE and WDE both set (datasheet, watchdog timer chapter, timed sequence);
 * the two stores are constants with interrupts held off, which -Os emits back to back, and the value
 * is read back rather than trusted. While MCUSR.WDRF is set the hardware holds WDE on, so the flag is
 * cleared first.
 *
 * The feed is the watchdog-reset instruction, reached through avr-gcc's __builtin_avr_wdr: the counter
 * restarts on that instruction and on nothing else the part offers, and a compiler built-in is not
 * assembly in this tree (docs/coding-rules.md §1). */
#include <stdint.h>

#include "avr_regs.h"
#include "avr_wdt.h"
#include "dbg.h"
#include "hal.h"

/* WDP[3:0] = 0111: 2 s at the watchdog's 128 kHz oscillator. */
#define WDT_PERIOD_2S (AVR_BIT(AVR_WDP2) | AVR_BIT(AVR_WDP1) | AVR_BIT(AVR_WDP0))
#define WDT_PERIOD_MASK (AVR_BIT(AVR_WDP3) | AVR_BIT(AVR_WDP2) | AVR_BIT(AVR_WDP1) | AVR_BIT(AVR_WDP0))

/* 1 once the main loop runs (hal/hal.h); read from vectors too. One byte, so no torn read. */
static volatile uint8_t wdtLooping;

static void wdtWrite(uint8_t value)
{
    uint8_t sreg = AvrOwn_LockSave();

    __builtin_avr_wdr();
    AVR_REG8(AVR_WDTCSR) = (uint8_t)(AVR_BIT(AVR_WDCE) | AVR_BIT(AVR_WDE));
    AVR_REG8(AVR_WDTCSR) = value;
    AvrOwn_LockRestore(sreg);
}

int Wdt_Init(void)
{
    uint8_t want = (uint8_t)(AVR_BIT(AVR_WDE) | WDT_PERIOD_2S);
    uint8_t got;

    AVR_REG8(AVR_MCUSR) = (uint8_t)(AVR_REG8(AVR_MCUSR) & (uint8_t)~AVR_BIT(AVR_WDRF));
    wdtWrite(want);

    got = (uint8_t)(AVR_REG8(AVR_WDTCSR) & (uint8_t)(AVR_BIT(AVR_WDE) | AVR_BIT(AVR_WDIE) | WDT_PERIOD_MASK));

    if (got != want)
    {
        Debug_Emit(DBG_COMP_WDT, got, DBG_WDT_RSN_AVR_REFUSED);
        return 0;
    }

    Debug_Emit(DBG_COMP_WDT, got, DBG_WDT_RSN_ARMED);
    return 1;
}

void Wdt_Feed(void)
{
    __builtin_avr_wdr();
}

void Wdt_LoopStart(void)
{
    wdtLooping = 1u;
}

/* Only while booting (hal/hal.h). */
void Wdt_FeedPoll(uint32_t round)
{
    if (wdtLooping == 0u && (round & (AVR_WDT_FEED_EVERY - 1u)) == (AVR_WDT_FEED_EVERY - 1u))
        Wdt_Feed();
}

/* WDP = 0000 is the shortest period. Interrupts stay off: nothing runs again before the reset. */
void Wdt_ResetNow(void)
{
    HAL_IrqLock();
    wdtWrite(AVR_BIT(AVR_WDE));

    for (;;)
    {
    }
}
