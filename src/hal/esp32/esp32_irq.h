/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* esp32_irq.h — routing a peripheral's interrupt source to a CPU line, and the dispatch the trap entry
 * calls (docs/hardware_esp32.md §12.3).
 *
 * This is the interrupt contract of the family layer, implemented once per controller kind: esp32_irq.c
 * for the PLIC of the C6 and H2. A part with a CLIC (the C5) would bring its own file behind this header.
 *
 * Every line is level-type: each source this layer routes holds its request until its own driver clears
 * it, so the controller has nothing to acknowledge. Lines 0, 3, 4 and 7 are the core's local sources and
 * are refused. */
#ifndef ESP32_IRQ_H
#define ESP32_IRQ_H

#include <stdint.h>

/* The CPU lines this layer hands out, one per driver. */
#define ESP32_LINE_TICK 1u
#define ESP32_LINE_USJ 2u
#define ESP32_LINE_RADIO 5u

/* Route `source` (esp32_chip.h ESP32_SRC_*) to CPU `line` at `priority` (1..15) and enable it; the
 * handler runs in interrupt context with interrupts off. Only during bring-up, with interrupts locked.
 * 1 = routed, 0 = a line this layer does not hand out. */
int Esp32Irq_Route(uint32_t source, uint32_t line, uint32_t priority, void (*handler)(void));

/* The interrupt controller's enabled lines, for the bring-up witnesses. */
uint32_t Esp32Irq_EnabledLines(void);

/* Called by the trap entry (startup/startup_esp32.S) with mcause; not for C callers. */
void Esp32Irq_Dispatch(uint32_t mcause);

#endif /* ESP32_IRQ_H */
