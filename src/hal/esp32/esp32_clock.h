/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* esp32_clock.h — the ESP32 family's time base, the board-private half of hal/hal.h's Tick_* contract. */
#ifndef ESP32_CLOCK_H
#define ESP32_CLOCK_H

#include <stdint.h>

/* SYSTIMER's unit 0 counts at 16 MHz from the crystal whatever the CPU runs at; its comparator 0 raises
 * the millisecond. 1 = the alarm is armed. */
int Clock_Init(void);

/* Switch the millisecond from the bring-up pump to the vector: once, just before interrupts open. */
void Clock_TickCountedByIrq(int on);

/* The bring-up read-backs as witness records. */
void Clock_WitnessReport(void);

/* The CPU clock the PCR's selection and dividers make, in Hz; 0 for a selection this layer does not
 * know. */
uint32_t Esp32Clock_CpuHz(void);

/* Run the CPU at the chip's fastest clock when the bootloader left the root on the PLL; nothing on any
 * other source. 1 = the CPU runs at ESP32_CPU_HZ_MAX. */
int Esp32Clock_CpuToMax(void);

#endif /* ESP32_CLOCK_H */
