/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* test_tick_plan.c — the ConBee III's millisecond, as arithmetic (tick_plan.h,
 * docs/hardware_mg21.md §10.3).
 *
 * SysTick counts cycles of whatever clock it is given, in a 24-bit field. On the ConBee II a
 * millisecond is 48 000 counts of the core clock and the tick is unremarkable; here the same idea has
 * to be checked against the field, because the numbers are bigger and the low-frequency oscillator is
 * not a millisecond source at all - 32.768 kHz is 32.768 cycles per millisecond, and the shortest
 * whole-millisecond period it can produce is 125 ms. Getting that wrong does not look wrong: the tick
 * runs, and every deadline in the image is quietly off by a factor. */
#include <stdint.h>

#include "mg21_regs.h"
#include "tick_plan.h"
#include "tu.h"

/* The clocks this board can put under SysTick. */
#define HZ_HFXO MG21_BOARD_HFXO_HZ   /* 38 400 000: what the board runs on */
#define HZ_FSRCO MG21_FSRCO_HZ       /* 20 000 000: what it comes up on */
#define HZ_LFRCO MG21_LFRCO_HZ       /* 32 768: the low-energy oscillator */
#define HZ_ULFRCO MG21_ULFRCO_HZ     /* 1 000: the always-on one */

/* cycles * 1000 == srcHz * msPerPeriod is the property that makes a plan exact: the period is a
 * whole number of cycles and a whole number of milliseconds at the same time. */
static void planIsExact(uint32_t srcHz, const Tick_Plan *plan)
{
    uint64_t cycles = plan->cycles;

    TU_CHECK(cycles * 1000u == (uint64_t)srcHz * plan->msPerPeriod);
    TU_CHECK(plan->load + 1u == plan->cycles);
    TU_CHECK(plan->cycles <= MG21_SYSTICK_RELOAD_MAX + 1u);
    TU_CHECK(plan->msPerPeriod >= 1u);
}

TU_TEST(a_millisecond_of_the_core_clock_fits_the_register)
{
    Tick_Plan plan;

    /* 38 400 counts, against a field that holds 16 777 216: the reload is not the limit here, and the
     * limit is a period of 436 ms. Stating the ceiling is the point - a longer period would be
     * refused rather than truncated. */
    TU_CHECK(HZ_HFXO / 1000u <= MG21_SYSTICK_RELOAD_MAX);
    TU_EQ(((uint64_t)(MG21_SYSTICK_RELOAD_MAX + 1u) * 1000u) / HZ_HFXO, 436u);

    TU_CHECK(TickPlan_Period(HZ_HFXO, 1u, &plan));
    TU_EQ(plan.cycles, 38400u);
    TU_EQ(plan.load, 38399u);
    TU_EQ(plan.msPerPeriod, 1u);
    planIsExact(HZ_HFXO, &plan);

    TU_CHECK(TickPlan_Period(HZ_FSRCO, 1u, &plan));
    TU_EQ(plan.cycles, 20000u);
    TU_EQ(plan.load, 19999u);
    planIsExact(HZ_FSRCO, &plan);

    TU_CHECK(TickPlan_Shortest(HZ_HFXO, TICK_PLAN_MAX_MS, &plan));
    TU_EQ(plan.msPerPeriod, 1u);
    TU_CHECK(TickPlan_Shortest(HZ_FSRCO, TICK_PLAN_MAX_MS, &plan));
    TU_EQ(plan.msPerPeriod, 1u);
}

TU_TEST(the_low_frequency_oscillator_is_not_a_millisecond_source)
{
    Tick_Plan plan;

    /* 32.768 cycles per millisecond is not a number SysTick can be given. */
    TU_CHECK(!TickPlan_Period(HZ_LFRCO, 1u, &plan));
    TU_CHECK(!TickPlan_Period(HZ_LFRCO, 2u, &plan));
    TU_CHECK(!TickPlan_Period(HZ_LFRCO, 8u, &plan));

    /* The first whole millisecond count it can do is 125 ms: 4096 cycles of 32.768 kHz, exactly. A
     * firmware that wanted LFRCO as its board clock would be counting quarters of a second and
     * calling it milliseconds. */
    TU_CHECK(TickPlan_Period(HZ_LFRCO, 125u, &plan));
    TU_EQ(plan.cycles, 4096u);
    TU_EQ(plan.load, 4095u);
    planIsExact(HZ_LFRCO, &plan);

    TU_CHECK(TickPlan_Shortest(HZ_LFRCO, TICK_PLAN_MAX_MS, &plan));
    TU_EQ(plan.msPerPeriod, 125u);
    TU_EQ(plan.cycles, 4096u);

    /* Asked for anything shorter than what it can do, it is refused rather than rounded. */
    TU_CHECK(!TickPlan_Shortest(HZ_LFRCO, 100u, &plan));

    /* ULFRCO is the opposite case: exactly one cycle per millisecond, so the whole field is unused
     * and the resolution is the oscillator's own - 1 kHz with a +-10 % spread (docs/hardware_mg21.md
     * §10.5). Exact arithmetic, coarse clock. */
    TU_CHECK(TickPlan_Period(HZ_ULFRCO, 1u, &plan));
    TU_EQ(plan.cycles, 1u);
    TU_EQ(plan.load, 0u);
    planIsExact(HZ_ULFRCO, &plan);
}

TU_TEST(the_bound_is_the_register_itself)
{
    Tick_Plan plan;

    /* The last period the field holds, and the first one it does not: 16 777 216 cycles is
     * LOAD = 0xFFFFFF exactly, and one cycle more has nowhere to be written. A bound copied from a
     * datasheet page, or a "round" number like 1 MiB, passes every other case in this file and is
     * wrong here. */
    TU_CHECK(TickPlan_Period(16777216u, 1000u, &plan));
    TU_EQ(plan.cycles, 16777216u);
    TU_EQ(plan.load, MG21_SYSTICK_RELOAD_MAX);
    planIsExact(16777216u, &plan);

    TU_CHECK(!TickPlan_Period(16777217u, 1000u, &plan));

    /* And a mid-range plan a smaller bound would refuse: 1 800 000 cycles is comfortably inside the
     * field. */
    TU_CHECK(TickPlan_Period(2000000u, 900u, &plan));
    TU_EQ(plan.cycles, 1800000u);
    planIsExact(2000000u, &plan);
}

TU_TEST(the_boundaries)
{
    Tick_Plan plan;

    /* A period longer than the register is refused, not truncated: 38 400 000 cycles for 1000 ms is
     * more than 16 777 216. */
    TU_CHECK(!TickPlan_Period(HZ_HFXO, 1000u, &plan));
    TU_CHECK(!TickPlan_Shortest(HZ_HFXO, 1000u, &plan) || plan.msPerPeriod == 1u);

    /* A clock with no whole cycles in a millisecond at all, at either end. */
    TU_CHECK(!TickPlan_Period(999u, 1u, &plan));
    TU_CHECK(!TickPlan_Period(0u, 1u, &plan));
    TU_CHECK(!TickPlan_Period(HZ_HFXO, 0u, &plan));

    /* 1500 Hz has no whole cycle in 1 ms but has 3 in 2 ms. */
    TU_CHECK(!TickPlan_Period(1500u, 1u, &plan));
    TU_CHECK(TickPlan_Period(1500u, 2u, &plan));
    TU_EQ(plan.cycles, 3u);
    planIsExact(1500u, &plan);

    /* Nothing is written when a plan is refused, and a null answer is a refusal, not a crash. */
    plan.cycles = 0xA5A5u;
    plan.msPerPeriod = 0xA5u;
    TU_CHECK(!TickPlan_Period(HZ_LFRCO, 1u, &plan));
    TU_EQ(plan.cycles, 0xA5A5u);
    TU_CHECK(!TickPlan_Period(HZ_HFXO, 1u, 0));
    TU_CHECK(!TickPlan_Shortest(HZ_LFRCO, 100u, 0));
}

static const Tu_Test tests[] = {
    {"a_millisecond_of_the_core_clock_fits_the_register", a_millisecond_of_the_core_clock_fits_the_register},
    {"the_low_frequency_oscillator_is_not_a_millisecond_source", the_low_frequency_oscillator_is_not_a_millisecond_source},
    {"the_bound_is_the_register_itself", the_bound_is_the_register_itself},
    {"the_boundaries", the_boundaries},
};

int main(void)
{
    Tu_Run("tick_plan", tests, (int)(sizeof(tests) / sizeof(tests[0])));
    return 0;
}
