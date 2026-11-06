/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* tick_plan.h — what it takes to get a whole millisecond out of a clock (docs/hardware_mg21.md
 * §10.5).
 *
 * SysTick's reload field is 24 bits, and it counts cycles of whatever clock it is given. A
 * millisecond of this board's 38.4 MHz core clock is 38 400 cycles, which fits with room to spare —
 * the field tops out at 16 777 215 cycles, a 436 ms period. A millisecond of LFRCO, at 32.768 kHz, is
 * 32.768 cycles, which is not a whole number at all: LFRCO is not a millisecond source, and the
 * shortest exact whole-millisecond period it can produce is 125 ms. That is the kind of fact a port
 * from a board with an 8 MHz start clock gets wrong by instinct, so it is arithmetic under a suite
 * rather than a comment.
 *
 * Pure arithmetic, no registers: hal/mg21/mg21_clock.c asks these questions and writes what they
 * answer into SysTick. */
#ifndef MG21_TICK_PLAN_H
#define MG21_TICK_PLAN_H

#include <stdint.h>

/* The coarsest period worth asking for. A board's deadlines are in milliseconds; a period longer than
 * a second is a clock this firmware would have to interpolate rather than count. */
#define TICK_PLAN_MAX_MS 1000u

/* A plan that was refused is kept apart from a plan that works: 1 = out holds a plan. */
typedef struct
{
    uint32_t cycles;      /* clock cycles in one SysTick period */
    uint32_t load;        /* the value for SysTick->LOAD: cycles - 1 */
    uint32_t msPerPeriod; /* milliseconds one period is worth */
} Tick_Plan;

/* One exact plan for one wanted period length: srcHz * wantMs / 1000 must be a whole number of
 * cycles, at most 16 777 216 of them, and wantMs at least 1. Refused otherwise. */
int TickPlan_Period(uint32_t srcHz, uint32_t wantMs, Tick_Plan *out);

/* The shortest exact period, in whole milliseconds, that this clock can produce at all: the smallest
 * wantMs at or below maxMs that TickPlan_Period accepts. Refused when no whole-millisecond period is
 * exact within maxMs. */
int TickPlan_Shortest(uint32_t srcHz, uint32_t maxMs, Tick_Plan *out);

#endif /* MG21_TICK_PLAN_H */
