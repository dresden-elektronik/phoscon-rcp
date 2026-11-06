/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mg21_panic.c — the ConBee III fault path, and the one thing it leaves behind.

A board this tree has never run has no console and no LED: what a fault means has to survive to the
next boot, where a host can read it. A soft reset (the watchdog's, hal_mg21.c) does not clear SRAM, so the fault's
identity is left in a `.noinit` word pair that the startup does not initialise, and the next boot
reports it as a record before anything else can overwrite it. A power cycle loses it, which is the
same limit the ConBee II's post-mortem scratch row has, without that board's flash wear.

Panic itself emits nothing: a fault may have happened in handler mode, in the middle of the thing it
is reporting, and a second fault on the way out is a lockup with no report at all. The hold then
feeds the watchdog so the reset that follows is the image's own, at a known time (hal/mg21/mg21_clock.c
provides the hold). */
#include <stdint.h>

#include "dbg.h"
#include "hal.h"
#include "mg21_clock.h"
#include "mg21_regs.h"

/* The mark: a signature, then the record's own component / line / reason. Its own signature because
 * on a cold boot this memory is whatever the previous run or the bootloader left in it. */
#define PANIC_MARK_SIGNATURE 0xC0DEu
#define PANIC_MARK_WORDS 4u

static volatile uint16_t panicMark[PANIC_MARK_WORDS] __attribute__((section(".noinit")));

static int panicEntered;

/* HAL_PreMain calls this once, before the main loop can change anything. */
void Panic_ReportPrevious(void);

void Panic_ReportPrevious(void)
{
    if (panicMark[0] != PANIC_MARK_SIGNATURE)
        return;

    Debug_Emit(DBG_COMP_CORE, panicMark[2], DBG_CORE_RSN_REPLAYED);
    Debug_Emit(DBG_COMP_WITNESS, panicMark[1], DBG_WITNESS_RSN_MG21_FAULT_COMPONENT);
    Debug_Emit(DBG_COMP_WITNESS, panicMark[3], DBG_WITNESS_RSN_MG21_FAULT_DETAIL);

    panicMark[0] = 0u;
}

void Panic_Enter(uint16_t component, uint16_t line, uint16_t reason)
{
    if (panicEntered != 0)
    {
        for (;;)
            HAL_Wfi();
    }

    panicEntered = 1;

    /* Interrupts off first: the mark is written by one writer at a time, and a fault handler that
     * faults while writing it must not be able to preempt itself. */
    HAL_IrqLock();

    panicMark[0] = 0u;
    panicMark[1] = component;
    panicMark[2] = line;
    panicMark[3] = reason;
    panicMark[0] = PANIC_MARK_SIGNATURE;

    Clock_FaultReport();
    HAL_SystemReset();

    for (;;)
        HAL_Wfi();
}

/* Every vector the startup table hands to def_handler lands here (startup/startup_mg21.S), and the
 * named fault vectors name themselves so the report says which one was taken. */
void def_handler(void)
{
    Panic_Enter(DBG_COMP_CORE, 0u, DBG_CORE_RSN_EXCEPTION);

    for (;;)
        HAL_Wfi();
}

void NMI_Handler(void)
{
    Panic_Enter(DBG_COMP_CORE, 0u, DBG_CORE_RSN_NMI);

    for (;;)
        HAL_Wfi();
}

void HardFault_Handler(void)
{
    Panic_Enter(DBG_COMP_CORE, 0u, DBG_CORE_RSN_HARDFAULT);

    for (;;)
        HAL_Wfi();
}

void MemManage_Handler(void)
{
    Panic_Enter(DBG_COMP_CORE, 0u, DBG_CORE_RSN_MEMFAULT);

    for (;;)
        HAL_Wfi();
}

void BusFault_Handler(void)
{
    Panic_Enter(DBG_COMP_CORE, 0u, DBG_CORE_RSN_BUSFAULT);

    for (;;)
        HAL_Wfi();
}

void UsageFault_Handler(void)
{
    Panic_Enter(DBG_COMP_CORE, 0u, DBG_CORE_RSN_USAGEFAULT);

    for (;;)
        HAL_Wfi();
}

void SVCall_Handler(void)
{
    Panic_Enter(DBG_COMP_CORE, 0u, DBG_CORE_RSN_SVCALL);

    for (;;)
        HAL_Wfi();
}

void DebugMonitor_Handler(void)
{
    Panic_Enter(DBG_COMP_CORE, 0u, DBG_CORE_RSN_DEBUGMONITOR);

    for (;;)
        HAL_Wfi();
}
