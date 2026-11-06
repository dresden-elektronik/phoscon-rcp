/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* avr_panic.c — the ConBee I / RaspBee I fault path, and the one thing it leaves behind.
 *
 * The same shape as the ConBee III's (hal/mg21/mg21_panic.c), written for this core: the fault's
 * identity goes into a `.noinit` word set that the startup neither copies nor clears, the reset is the
 * watchdog's (HAL_SystemReset), and the next boot reports the mark as records before anything else can
 * overwrite it. A power cycle loses it. Whether the bootloader, which runs between the two images,
 * leaves that SRAM alone is not measured: it does not clear SRAM (docs/hardware_avr.md §11.2), and the
 * mark sits above this image's .bss, clear of the bootloader's globals (targets/cbrb1/atmega256rfr2.ld).
 *
 * Panic itself emits nothing: a fault may have happened in the middle of the record queue. The AVR has
 * no fault vectors; what lands here is a vector no driver claimed (startup/startup_avr.S, def_handler)
 * and the image's own Panic_Enter calls. */
#include <stdint.h>

#include "avr_panic.h"
#include "dbg.h"
#include "hal.h"

#define PANIC_MARK_SIGNATURE 0xC0DEu
#define PANIC_MARK_WORDS 4u

static volatile uint16_t panicMark[PANIC_MARK_WORDS] __attribute__((section(".noinit")));

static uint8_t panicEntered;

void Panic_ReportPrevious(void)
{
    if (panicMark[0] != PANIC_MARK_SIGNATURE)
        return;

    Debug_Emit(DBG_COMP_CORE, panicMark[2], DBG_CORE_RSN_REPLAYED);
    Debug_Emit(DBG_COMP_WITNESS, panicMark[1], DBG_WITNESS_RSN_AVR_FAULT_COMPONENT);
    Debug_Emit(DBG_COMP_WITNESS, panicMark[3], DBG_WITNESS_RSN_AVR_FAULT_DETAIL);

    panicMark[0] = 0u;
}

void Panic_Enter(uint16_t component, uint16_t line, uint16_t reason)
{
    HAL_IrqLock();

    if (panicEntered == 0u)
    {
        panicEntered = 1u;
        panicMark[0] = 0u;
        panicMark[1] = component;
        panicMark[2] = line;
        panicMark[3] = reason;
        panicMark[0] = PANIC_MARK_SIGNATURE;
    }

    HAL_SystemReset();

    for (;;)
    {
    }
}

/* The target of every vector slot no C file defines (__bad_interrupt). Entered with interrupts masked
 * by the core and never left. */
void def_handler(void);

void def_handler(void)
{
    Panic_Enter(DBG_COMP_CORE, 0u, DBG_CORE_RSN_EXCEPTION);
}
