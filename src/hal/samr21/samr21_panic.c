/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* Panic path. No LED: hold for the band the board reached, then core-reset into
 * the bootloader window. It emits no record and writes no flash - a fault in
 * handler mode on a half-finished flash command cannot escalate on the M0+ and
 * becomes a Lockup, so the fault's identity is dropped and its report is time. */
#include <stdint.h>

#include "hal.h"
#include "hal_diag.h"
#include "dbg.h"

/* Cycle-counted, WDT-fed hold then SYSRESETREQ, with no DBG and no NVM access
 * (samr21_clock.c). */
extern void Clock_FaultReport(void);

/* Enter the record path at most once per panic: a second fault in handler mode
 * is a Lockup, so a repeat must only wait for the reset. */
static int panicEntered;

void Panic_Enter(uint16_t component, uint16_t line, uint16_t reason)
{
    if (panicEntered != 0)
    {
        /* Already reported; idle unfed, letting the inherited watchdog reset us. */
        for (;;)
            HAL_Wfi();
    }

    panicEntered = 1;

    /* No record can be delivered from here, so the fault identity is dropped. */
    (void)component;
    (void)line;
    (void)reason;

    /* Stop the SysTick feed; the hold below feeds the WDT explicitly, so the
   * reset that arrives is ours, at a known time. */
    HAL_IrqLock();

    /* Terminal path, usually handler mode: hold for the band reached, feeding the
   * WDT as it goes, then reset the core into the bootloader window. */
    Clock_FaultReport();
}

/* Default exception vector: NMI/HardFault/BusFault/UsageFault all land here. */
void def_handler(void)
{
    Panic_Enter(DBG_COMP_CORE, 0, DBG_CORE_RSN_EXCEPTION);

    /* Unreachable by design, but it must exist: Panic_Enter must not fall through
   * into the next function in the image. */
    for (;;)
        HAL_Wfi();
}

/* The startup table names both host-link vectors (startup/startup_samr21.S) whatever the
 * board is, so the one this board does not drive needs a body. It is weak: the link's own
 * driver - samr21_usb_device.c for CDC, samr21_uart.c for a SERCOM USART - defines the
 * strong one, and only that one is linked. Nothing can reach these: their NVIC line is
 * never enabled on a board that does not have the link. */
__attribute__((weak)) void USB_Handler(void)
{
    def_handler();
}

__attribute__((weak)) void SERCOM5_Handler(void)
{
    def_handler();
}
