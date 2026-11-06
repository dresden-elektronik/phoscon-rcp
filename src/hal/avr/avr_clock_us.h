/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* avr_clock_us.h — the microsecond clock's arithmetic (docs/hardware_avr.md §11.7).
 *
 * Portable core, not glue, so the host suite pins it: Timer/Counter1 runs free at 16 MHz and wraps every
 * 4 096 us, and its overflow vector counts the wraps. A read made with the vector held off can find the
 * timer already wrapped and the wrap not yet counted; the pending flag and a small count say so, and a
 * large count with the flag set wrapped after the count was read. */
#ifndef AVR_CLOCK_US_H
#define AVR_CLOCK_US_H

#include <stdint.h>

#define AVR_CLOCK_US_WRAP_SHIFT 12u
#define AVR_CLOCK_US_CYCLE_SHIFT 4u
#define AVR_CLOCK_US_HALF 0x8000u

static inline uint32_t AvrClock_Us(uint32_t wraps, uint16_t count, int wrapPending)
{
    if (wrapPending && count < AVR_CLOCK_US_HALF)
        wraps++;

    return (wraps << AVR_CLOCK_US_WRAP_SHIFT) + (uint32_t)(count >> AVR_CLOCK_US_CYCLE_SHIFT);
}

#endif
