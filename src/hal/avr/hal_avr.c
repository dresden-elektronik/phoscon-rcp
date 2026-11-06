/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* Board glue for the ConBee I / RaspBee I: startup hand-off, HAL lifecycle (hal/hal.h), and the
 * reset that hands the board to the bootloader's window.
 *
 * The host link is the board's (hal/com.h, named by targets/cbrb1/board.cmake), so this file names no
 * link. The reset is the watchdog's: the RaspBee I bootloader is entered on every reset (BOOTRST), and
 * its own reset command is a short watchdog period, so a watchdog reset is the path its update dialog
 * is known to start from (docs/hardware_avr.md §11.2, §11.10). */
#include <stdint.h>

#include "avr_clock.h"
#include "avr_panic.h"
#include "avr_regs.h"
#include "avr_uart.h"
#include "avr_wdt.h"
#include "com.h"
#include "dbg.h"
#include "hal.h"

extern int main(void);

/* Handler depth. The core does not nest vectors (an entered vector runs with I clear and nothing here
 * sets it), so this is 0 or 1, but it is the same contract as the other boards (hal/hal.h). */
static volatile uint8_t halIrqDepth;

void HAL_IrqEnter(void)
{
    halIrqDepth++;
}

void HAL_IrqExit(void)
{
    if (halIrqDepth != 0u)
        halIrqDepth--;
}

int HAL_InIrqContext(void)
{
    return halIrqDepth != 0u;
}

/* Entered from .init9 (startup/startup_avr.S) with interrupts masked, the stack set, and .data and .bss
 * already done by libgcc's .init4 routines: on this core the copy needs the extended program-memory
 * load, which libgcc has and C does not. The reset cause is read before anything can change it and
 * cleared, so the next boot's is its own. */
void HAL_PreMain(void)
{
    uint8_t mcusr = AVR_REG8(AVR_MCUSR);

    AVR_REG8(AVR_MCUSR) = 0u;

    DBG(DBG_COMP_STARTUP, DBG_STARTUP_RSN_BOOT);
    Debug_Emit(DBG_COMP_WITNESS, mcusr, DBG_WITNESS_RSN_AVR_MCUSR);
    Panic_ReportPrevious();
    (void)main();

    for (;;)
    {
    }
}

int HAL_Init(void)
{
    int regime;

    regime = Clock_Init();

    if (regime == AVR_CLOCK_DOWN)
    {
        /* No millisecond: no deadline can be kept and the link cannot be polled. The watchdog is not
         * armed on this path, so the image idles where the bootloader's next reset finds it. */
        DBG(DBG_COMP_APP, DBG_APP_RSN_HELLO);
        return 0;
    }

    (void)Wdt_Init();

    if (!COM_Init())
        DBG(DBG_COMP_APP, DBG_APP_RSN_LINK_INIT_FAIL);

    /* HAL_Wfi's sleep enters the mode SMCR names: idle, where Timer/Counter0 and USART0 keep running
     * and either wakes the core. */
    AVR_REG8(AVR_SMCR) = AVR_BIT(AVR_SE);

    Clock_WitnessReport();

    Clock_TickCountedByIrq();
    HAL_IrqUnlock();

    return regime;
}

/* A reset into the bootloader: interrupts off, the last octet out, then the shortest watchdog period.
 * The bootloader finds WDRF, switches the watchdog off and opens its window (§11.10). */
void HAL_SystemReset(void)
{
    HAL_IrqLock();
    Uart_Release();
    Wdt_ResetNow();
}
