/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* test_uart_baud.c — UartBaud_Register suite (docs/testing.md §2).
 *
 * The divisor is the one number on a UART link that nothing else can catch: program a wrong
 * BAUD and both ends still talk, at a rate neither of them asked for, and the damage shows up
 * as garbled frames far away from the register write. So this arithmetic is covered here,
 * against the datasheet's own equation, not against a copy of the code.
 *
 * The expectations are computed here in 64-bit from Table 23-2 (BAUD = 65536 (1 - S*baud/ref),
 * S = 16) and the achieved rate from f = (ref/S) (1 - BAUD/65536), which is what the SERCOM
 * then shifts. What the suite pins is the value, the direction of the error (never faster than
 * asked), the bound on its size, and the refusals. */
#include "uart_baud.h"
#include "tu.h"

#include <stdint.h>

#define SAMPLES ((unsigned long long)UART_BAUD_SAMPLES)

static unsigned long long expectedReg(uint32_t refHz, uint32_t baud)
{
    unsigned long long scaled = (unsigned long long)baud * SAMPLES;

    return 65536ull - (scaled * 65536ull) / refHz;
}

/* The rate the SERCOM would actually shift with this register value:
 * f = (ref/S) (1 - reg/65536). */
static unsigned long long actualHz(uint32_t refHz, uint16_t reg)
{
    return ((unsigned long long)refHz * (65536ull - (unsigned long long)reg)) /
           (SAMPLES * 65536ull);
}

/* One quantum of the arithmetic divisor at this reference: BAUD steps by 1, the rate
 * by ref/(S*65536). Truncation can cost at most one step, plus the one hertz the
 * integer division above floors away. */
static unsigned long long quantumHz(uint32_t refHz)
{
    return ((unsigned long long)refHz) / (SAMPLES * 65536ull) + 2ull;
}

static int accepted(uint32_t refHz, uint32_t baud, uint16_t *reg)
{
    return UartBaud_Register(refHz, baud, reg);
}

TU_TEST(the_board_line_rate_lands_on_the_register_the_datasheet_predicts)
{
    uint16_t reg = 0xFFFFu;

    TU_EQ(accepted(48000000u, 115200u, &reg), 1);
    TU_EQ(reg, expectedReg(48000000u, 115200u));
    TU_EQ(reg, 63020u);
    TU_NOTE("achieved", actualHz(48000000u, reg));
    TU_CHECK(115200ull - actualHz(48000000u, reg) <= quantumHz(48000000u));
}

TU_TEST(the_vendor_line_rate_comes_from_the_same_arithmetic)
{
    uint16_t reg = 0xFFFFu;

    TU_EQ(accepted(48000000u, 38400u, &reg), 1);
    TU_EQ(reg, expectedReg(48000000u, 38400u));
    TU_CHECK(38400ull - actualHz(48000000u, reg) <= quantumHz(48000000u));
}

TU_TEST(a_lower_reference_just_needs_a_different_divisor)
{
    uint16_t reg = 0xFFFFu;

    /* 8 MHz is the tree HAL_Init reports when the DFLL never locked: the rate is still
     * samplable there, which is why the reference is an argument and not a constant. */
    TU_EQ(accepted(8000000u, 115200u, &reg), 1);
    TU_EQ(reg, expectedReg(8000000u, 115200u));
    TU_CHECK(115200ull - actualHz(8000000u, reg) <= quantumHz(8000000u));
}

TU_TEST(a_rate_is_never_overshot)
{
    uint32_t baud;
    unsigned long long worst = 0ull;

    for (baud = 1200u; baud <= 1000000u; baud = (baud < 9600u) ? baud * 2u : baud + 2400u)
    {
        uint16_t reg = 0u;
        unsigned long long got;
        unsigned long long short_by;

        if (!accepted(48000000u, baud, &reg))
            continue;

        got = actualHz(48000000u, reg);

        /* Truncation can only make the SERCOM slower than asked. */
        TU_CHECK(got <= (unsigned long long)baud);

        short_by = (unsigned long long)baud - got;

        if (short_by > worst)
            worst = short_by;

        /* At most one step of the divisor, whatever the rate: the deficit is a fixed hertz
         * figure here, so the *relative* error is what grows at low rates - and 45 Hz is
         * nothing against the few per cent a UART frame budgets over ten bits. */
        TU_CHECK(short_by <= quantumHz(48000000u));
    }

    TU_NOTE("worst deficit over the sweep, hertz", (long)worst);
}

TU_TEST(a_higher_rate_needs_a_smaller_divisor)
{
    uint16_t slow = 0u;
    uint16_t fast = 0u;

    TU_EQ(accepted(48000000u, 9600u, &slow), 1);
    TU_EQ(accepted(48000000u, 230400u, &fast), 1);
    TU_CHECK(fast < slow);
}

TU_TEST(a_rate_the_reference_cannot_sample_is_refused)
{
    uint16_t reg = 0x5Au;

    /* Table 23-2: arithmetic mode needs baud <= ref/16. At the equality BAUD would have to be
     * 0 - the whole reference over 16 - which is not a UART this firmware wants, so the
     * boundary refuses rather than wrapping to 65536. */
    TU_EQ(accepted(48000000u, 3000000u, &reg), 0);
    TU_EQ(reg, 0x5Au);

    TU_EQ(accepted(48000000u, 3000001u, &reg), 0);
    TU_EQ(accepted(8000000u, 1000000u, &reg), 0);
}

TU_TEST(no_rate_and_no_reference_are_not_a_divisor)
{
    uint16_t reg = 0x5Au;

    TU_EQ(accepted(48000000u, 0u, &reg), 0);
    TU_EQ(accepted(0u, 115200u, &reg), 0);
    TU_EQ(accepted(0u, 0u, &reg), 0);
    TU_EQ(reg, 0x5Au);
}

TU_TEST(a_wrapping_sample_count_is_refused_not_wrapped)
{
    uint16_t reg = 0x5Au;

    /* baud * 16 must not wrap 32 bits and then read as a legal small number. */
    TU_EQ(accepted(48000000u, 0xFFFFFFFFu, &reg), 0);
    TU_EQ(accepted(48000000u, 0x0FFFFFFFu, &reg), 0);
    TU_EQ(reg, 0x5Au);
}

TU_TEST(a_null_output_is_not_written)
{
    TU_EQ(UartBaud_Register(48000000u, 115200u, 0), 0);
}

int main(void)
{
    const Tu_Test tests[] = {
        {"the_board_line_rate_lands_on_the_register_the_datasheet_predicts",
         the_board_line_rate_lands_on_the_register_the_datasheet_predicts},
        {"the_vendor_line_rate_comes_from_the_same_arithmetic",
         the_vendor_line_rate_comes_from_the_same_arithmetic},
        {"a_lower_reference_just_needs_a_different_divisor",
         a_lower_reference_just_needs_a_different_divisor},
        {"a_rate_is_never_overshot", a_rate_is_never_overshot},
        {"a_higher_rate_needs_a_smaller_divisor", a_higher_rate_needs_a_smaller_divisor},
        {"a_rate_the_reference_cannot_sample_is_refused",
         a_rate_the_reference_cannot_sample_is_refused},
        {"no_rate_and_no_reference_are_not_a_divisor", no_rate_and_no_reference_are_not_a_divisor},
        {"a_wrapping_sample_count_is_refused_not_wrapped",
         a_wrapping_sample_count_is_refused_not_wrapped},
        {"a_null_output_is_not_written", a_null_output_is_not_written}};

    Tu_Run("uart_baud", tests, (int)(sizeof(tests) / sizeof(tests[0])));
    return 0;
}
