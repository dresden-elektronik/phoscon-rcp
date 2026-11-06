/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* test_avr_clock_us.c — the ConBee I / RaspBee I microsecond clock's arithmetic (avr_clock_us.h). */
#include <stdint.h>

#include "avr_clock_us.h"
#include "tu.h"

TU_TEST(sixteen_cycles_are_a_microsecond)
{
    TU_EQ(AvrClock_Us(0u, 0u, 0), 0u);
    TU_EQ(AvrClock_Us(0u, 15u, 0), 0u);
    TU_EQ(AvrClock_Us(0u, 16u, 0), 1u);
    TU_EQ(AvrClock_Us(0u, 0xFFFFu, 0), 4095u);
}

TU_TEST(a_wrap_is_4096_microseconds)
{
    TU_EQ(AvrClock_Us(1u, 0u, 0), 4096u);
    TU_EQ(AvrClock_Us(3u, 160u, 0), 12298u);
}

TU_TEST(a_wrap_not_yet_counted_is_added)
{
    TU_EQ(AvrClock_Us(1u, 16u, 1), 8193u);
}

TU_TEST(a_flag_raised_after_the_count_was_read_adds_nothing)
{
    TU_EQ(AvrClock_Us(1u, 0xFFF0u, 1), 4096u + 4095u);
}

TU_TEST(the_clock_never_steps_back_across_a_wrap)
{
    uint32_t before = AvrClock_Us(7u, 0xFFFFu, 0);
    uint32_t pending = AvrClock_Us(7u, 0u, 1);
    uint32_t counted = AvrClock_Us(8u, 0u, 0);

    TU_CHECK(pending > before);
    TU_EQ(pending, counted);
}

TU_TEST(it_wraps_at_two_to_the_32_with_no_jump)
{
    uint32_t wraps = (uint32_t)(0x100000000uLL >> 12);

    TU_EQ(AvrClock_Us(wraps - 1u, 0xFFFFu, 0), 0xFFFFFFFFu);
    TU_EQ(AvrClock_Us(wraps, 0u, 0), 0u);
    TU_EQ((uint32_t)(AvrClock_Us(wraps, 16u, 0) - AvrClock_Us(wraps - 1u, 0xFFF0u, 0)), 2u);
}

static const Tu_Test tests[] = {
    {"sixteen_cycles_are_a_microsecond", sixteen_cycles_are_a_microsecond},
    {"a_wrap_is_4096_microseconds", a_wrap_is_4096_microseconds},
    {"a_wrap_not_yet_counted_is_added", a_wrap_not_yet_counted_is_added},
    {"a_flag_raised_after_the_count_was_read_adds_nothing", a_flag_raised_after_the_count_was_read_adds_nothing},
    {"the_clock_never_steps_back_across_a_wrap", the_clock_never_steps_back_across_a_wrap},
    {"it_wraps_at_two_to_the_32_with_no_jump", it_wraps_at_two_to_the_32_with_no_jump},
};

int main(void)
{
    Tu_Run("avr_clock_us", tests, (int)(sizeof(tests) / sizeof(tests[0])));
    return 0;
}
