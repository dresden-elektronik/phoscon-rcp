/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* tick_plan.c — the millisecond arithmetic (tick_plan.h).
 *
 * A whole millisecond is 1000 periods of the source per second, so cycles = srcHz * ms / 1000, and
 * the question is only whether that is a whole number and whether it fits the reload field. The
 * multiply is done as a whole part plus a scaled remainder so no 64-bit intermediate is needed on a
 * Cortex-M0-class core (docs/hardware_mg21.md §10.3 keeps this tree's MG21 arithmetic 32-bit where it
 * can be). */
#include <stdint.h>

#include "tick_plan.h"

/* SysTick's reload field, 24 bits (mg21_regs.h holds the same bound as the core register's mask, and
 * the mg21_regs suite checks it against CMSIS). */
#define TICK_PLAN_RELOAD_MAX 0x00FFFFFFu

#define TICK_PLAN_MS 1000u

/* The largest number of cycles one SysTick period can count. */
#define TICK_PLAN_MAX_CYCLES (TICK_PLAN_RELOAD_MAX + 1u)

static uint32_t planGcd(uint32_t a, uint32_t b)
{
    while (b != 0u)
    {
        uint32_t r = a % b;
        a = b;
        b = r;
    }

    return a;
}

int TickPlan_Period(uint32_t srcHz, uint32_t wantMs, Tick_Plan *out)
{
    uint32_t whole;
    uint32_t rest;
    uint32_t scaled;
    uint32_t cycles;

    if (out == 0 || wantMs == 0u || srcHz == 0u)
        return 0;

    rest = srcHz % TICK_PLAN_MS;

    /* srcHz * wantMs has to be a multiple of 1000, and only the remainder of srcHz can spoil it.
     * 999 * 1000 is the largest product this term reaches, so 32 bits are enough. */
    if (rest != 0u && ((rest * wantMs) % TICK_PLAN_MS) != 0u)
        return 0;

    whole = (srcHz / TICK_PLAN_MS) * wantMs;
    scaled = (rest * wantMs) / TICK_PLAN_MS;
    cycles = whole + scaled;

    if (cycles == 0u || cycles > TICK_PLAN_MAX_CYCLES)
        return 0;

    out->cycles = cycles;
    out->load = cycles - 1u;
    out->msPerPeriod = wantMs;
    return 1;
}

int TickPlan_Shortest(uint32_t srcHz, uint32_t maxMs, Tick_Plan *out)
{
    uint32_t wantMs;

    if (srcHz == 0u)
        return 0;

    /* ms is exact when 1000 divides srcHz * ms, which first happens at ms = 1000 / gcd(srcHz, 1000):
     * a clock that already contains 1000 needs only 1 ms, and LFRCO, whose gcd with 1000 is 8, needs
     * 125. */
    wantMs = TICK_PLAN_MS / planGcd(srcHz, TICK_PLAN_MS);

    if (wantMs > maxMs)
        return 0;

    return TickPlan_Period(srcHz, wantMs, out);
}
