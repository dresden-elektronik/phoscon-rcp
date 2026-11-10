/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* esp32_irq.c — the interrupt matrix and the PLIC of the ESP32-C6 and -H2 (esp32_irq.h).

The matrix maps each peripheral source to one of the CPU's 32 lines: the map register of source n is at
INTMTX + 4n and holds the line number. The PLIC then enables, types and prioritises each line, and masks
those below its threshold (TRM 1.6.2, 10.3.3). The controller's registers sit on the APB bus, so a change
ends with a fence, the controller's own suggested sequence (TRM 1.6.3.1); this file runs with interrupts
locked during bring-up, which is the other half of that sequence. */
#include <stdint.h>

#include "esp32_irq.h"
#include "esp32_regs.h"
#include "hal.h"

#define IRQ_LINES 32u
/* The core-local lines, which the PLIC does not route (TRM 1.6.1). */
#define IRQ_LINES_LOCAL ((1u << 0) | (1u << 3) | (1u << 4) | (1u << 7))

static void (*irqHandler[IRQ_LINES])(void);
static uint16_t irqSpurious;

int Esp32Irq_Route(uint32_t source, uint32_t line, uint32_t priority, void (*handler)(void))
{
    uint32_t bit;

    if (line >= IRQ_LINES || handler == 0 || priority == 0u || priority > 15u)
        return 0;

    bit = 1u << line;

    if ((bit & IRQ_LINES_LOCAL) != 0u)
        return 0;

    irqHandler[line] = handler;
    ESP32_REG(ESP32_INTMTX_BASE, 4u * source) = line;
    ESP32_REG(ESP32_PLIC_BASE, ESP32_PLIC_TYPE) &= ~bit;
    ESP32_REG(ESP32_PLIC_BASE, ESP32_PLIC_PRI0 + 4u * line) = priority;
    ESP32_REG(ESP32_PLIC_BASE, ESP32_PLIC_THRESH) = ESP32_PLIC_THRESH_ENABLED;
    ESP32_REG(ESP32_PLIC_BASE, ESP32_PLIC_ENABLE) |= bit;
    HAL_Dsb();
    return 1;
}

uint32_t Esp32Irq_EnabledLines(void)
{
    return ESP32_REG(ESP32_PLIC_BASE, ESP32_PLIC_ENABLE);
}

/* Interrupt context, entered from the trap entry with MIE clear. A line with no handler is counted and
 * masked: a level source nobody clears would otherwise take every cycle. */
__attribute__((section(".iram.text.esp32_irq"))) void Esp32Irq_Dispatch(uint32_t mcause)
{
    uint32_t line = mcause & (IRQ_LINES - 1u);

    HAL_IrqEnter();

    if (irqHandler[line] != 0)
    {
        irqHandler[line]();
    }
    else
    {
        irqSpurious++;
        ESP32_REG(ESP32_PLIC_BASE, ESP32_PLIC_ENABLE) &= ~(1u << line);
        HAL_Dsb();
    }

    HAL_IrqExit();
}
