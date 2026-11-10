/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* esp32_panic.c — the ESP32 fault path, and the one thing it leaves behind.

The shape is the ConBee III's (hal/mg21/mg21_panic.c): a panic emits nothing, because it may be in the
middle of the thing it reports, and writes a mark into `.noinit` RAM instead, which the software reset
that follows does not clear and the bootloader does not load over. The next boot reports it before
anything else. An exception adds the faulting pc, which is what a map file turns into a function. A power
cycle loses the mark. */
#include <stdint.h>

#include "dbg.h"
#include "esp32_panic.h"
#include "hal.h"

#define PANIC_MARK_SIGNATURE 0xC0DEu
#define PANIC_MARK_WORDS 6u

static volatile uint16_t panicMark[PANIC_MARK_WORDS] __attribute__((section(".noinit")));

static int panicEntered;

void Panic_ReportPrevious(void)
{
    if (panicMark[0] != PANIC_MARK_SIGNATURE)
        return;

    Debug_Emit(DBG_COMP_CORE, panicMark[2], DBG_CORE_RSN_REPLAYED);
    Debug_Emit(DBG_COMP_WITNESS, panicMark[1], DBG_WITNESS_RSN_ESP32_FAULT_COMPONENT);
    Debug_Emit(DBG_COMP_WITNESS, panicMark[3], DBG_WITNESS_RSN_ESP32_FAULT_DETAIL);
    Debug_Emit(DBG_COMP_WITNESS, panicMark[4], DBG_WITNESS_RSN_ESP32_FAULT_PC_LO);
    Debug_Emit(DBG_COMP_WITNESS, panicMark[5], DBG_WITNESS_RSN_ESP32_FAULT_PC_HI);

    panicMark[0] = 0u;
}

static void panicMarkAndReset(uint16_t component, uint16_t line, uint16_t reason, uint32_t pc)
{
    if (panicEntered != 0)
    {
        for (;;)
            HAL_Wfi();
    }

    panicEntered = 1;
    HAL_IrqLock();

    panicMark[0] = 0u;
    panicMark[1] = component;
    panicMark[2] = line;
    panicMark[3] = reason;
    panicMark[4] = (uint16_t)pc;
    panicMark[5] = (uint16_t)(pc >> 16);
    panicMark[0] = PANIC_MARK_SIGNATURE;

    HAL_SystemReset();

    for (;;)
        HAL_Wfi();
}

void Panic_Enter(uint16_t component, uint16_t line, uint16_t reason)
{
    panicMarkAndReset(component, line, reason, 0u);
}

/* The exception code (mcause, its low bits: 1 instruction access fault, 2 illegal instruction, 5 load
 * access fault, 7 store access fault, ...) is the reason; the line field carries mtval's low half, the
 * address or instruction that faulted. */
void Esp32Trap_Exception(uint32_t mcause, uint32_t mepc, uint32_t mtval)
{
    panicMarkAndReset(DBG_COMP_CORE, (uint16_t)mtval, (uint16_t)mcause, mepc);
}
