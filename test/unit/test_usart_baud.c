/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* test_usart_baud.c — UsartBaud_ClkDiv / UsartBaud_RateHz (docs/testing.md §2).
 *
 * The divisor is the number that decides whether two ends of a wire agree on a bit rate, and
 * nothing downstream of it reports the mistake: a wrong CLKDIV is a link that talks, at a
 * rate neither side asked for. The ConBee III's USART expresses the divisor as 256 * (divide
 * - 1) in a field with five fractional bits, which is a different equation from the SAMR21's
 * SERCOM BAUD (that one has its own suite, hal/uart_baud.c).
 *
 * Expectations come from the equation, and the achieved rate is recomputed from the register
 * value the helper returns, so the suite pins the value, the direction and size of the error,
 * and the cases where no register value expresses what was asked. */
#include <stdint.h>

#include "tu.h"
#include "usart_baud.h"

#define REF_CB3 38400000u /* the ConBee III's system clock: HFXO at 38.4 MHz, PCLK undivided */

/* The field value the equation asks for, unrounded: CLKDIV = 256*(divide - 1), where
 * divide = ref/(oversample*baud). Written as a difference, not as a ratio, because the -256
 * is part of the encoding. */
static long long idealClkDiv(uint32_t refHz, uint32_t baud, uint32_t oversample)
{
    long long scaled = (long long)refHz * USART_BAUD_CLKDIV_SCALE
                       / ((long long)baud * (long long)oversample);

    return scaled - (long long)USART_BAUD_CLKDIV_SCALE;
}

static int accepted(uint32_t refHz, uint32_t baud, uint32_t oversample, uint32_t *clkdiv)
{
    return UsartBaud_ClkDiv(refHz, baud, oversample, clkdiv);
}

/* The nearest value the field can actually hold: a multiple of CLKDIV_STEP within
 * +-STEP/2 of the ideal one. */
static uint32_t nearest(uint32_t refHz, uint32_t baud, uint32_t oversample)
{
    long long ideal = idealClkDiv(refHz, baud, oversample);
    long long step = (long long)USART_BAUD_CLKDIV_STEP;
    long long q = (ideal + step / 2) / step;

    return (uint32_t)(q * step);
}

TU_TEST(board_rate)
{
    uint32_t clkdiv = 0u;
    uint32_t got;

    TU_CHECK(accepted(REF_CB3, 115200u, USART_BAUD_OVS_16, &clkdiv));
    TU_EQ(clkdiv, nearest(REF_CB3, 115200u, USART_BAUD_OVS_16));
    got = UsartBaud_RateHz(REF_CB3, USART_BAUD_OVS_16, clkdiv);
    TU_NOTE("achieved baud at 115200/38.4 MHz", (long)got);
    /* Within half a percent of what was asked. */
    TU_CHECK(got > 114600u && got < 115800u);
}

TU_TEST(exact_when_it_divides)
{
    uint32_t clkdiv = 1u;

    /* A reference that is exactly oversample * baud divides by one, and the field reads 0. */
    TU_CHECK(accepted(1843200u, 115200u, USART_BAUD_OVS_16, &clkdiv));
    TU_EQ(clkdiv, 0u);
    TU_EQ(UsartBaud_RateHz(1843200u, USART_BAUD_OVS_16, clkdiv), 115200u);
}

TU_TEST(error_bounded_across_rates)
{
    static const uint32_t rates[] = {1200u, 9600u, 19200u, 38400u, 57600u, 115200u, 230400u,
                                     460800u, 921600u, 1500000u};
    unsigned i;

    for (i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        uint32_t clkdiv = 0u;
        uint32_t rate;
        unsigned long long want = rates[i];
        unsigned long long err;

        if (!accepted(REF_CB3, rates[i], USART_BAUD_OVS_16, &clkdiv)) {
            /* A rate the reference cannot sample at 16x is a refusal, not an error. */
            TU_CHECK(want > REF_CB3 / USART_BAUD_OVS_16);
            continue;
        }
        rate = UsartBaud_RateHz(REF_CB3, USART_BAUD_OVS_16, clkdiv);
        err = rate > want ? rate - want : want - rate;
        /* Never more than 0.6 % off, and never expressed in a field wider than the part has. */
        TU_CHECK(err * 1000ull < want * 6ull);
        TU_CHECK(clkdiv <= USART_BAUD_CLKDIV_MAX);
    }
}

TU_TEST(coarser_oversampling_divides_more)
{
    uint32_t d16 = 0u, d8 = 0u;

    /* Sampling each bit eight times instead of sixteen spends twice as many reference cycles
     * per bit, so the divisor grows - and the ceiling on the rate doubles with it. */
    TU_CHECK(accepted(REF_CB3, 115200u, USART_BAUD_OVS_16, &d16));
    TU_CHECK(accepted(REF_CB3, 115200u, USART_BAUD_OVS_8, &d8));
    TU_CHECK(d8 > d16);
    TU_CHECK(!accepted(REF_CB3, 3000000u, USART_BAUD_OVS_16, &d16)); /* above ref/16 */
    TU_CHECK(accepted(REF_CB3, 3000000u, USART_BAUD_OVS_8, &d8));    /* inside ref/8 */
}

TU_TEST(refusals)
{
    uint32_t d = 0u;

    TU_EQ(accepted(REF_CB3, 0u, USART_BAUD_OVS_16, &d), 0);
    TU_EQ(accepted(0u, 115200u, USART_BAUD_OVS_16, &d), 0);
    TU_EQ(accepted(REF_CB3, 115200u, USART_BAUD_OVS_16, 0), 0);
    TU_EQ(accepted(REF_CB3, 115200u, 12u, &d), 0);  /* not an oversampling the part offers */
    TU_EQ(accepted(REF_CB3, 115200u, 0u, &d), 0);
    TU_EQ(accepted(REF_CB3, REF_CB3 + 1u, USART_BAUD_OVS_16, &d), 0); /* above the sample limit */
    /* A rate so slow that the divisor would not fit the field. */
    TU_EQ(accepted(REF_CB3, 1u, USART_BAUD_OVS_16, &d), 0);
    /* A reference so large that 32*ref would wrap the helper's 32-bit step. */
    TU_EQ(accepted(200000000u, 115200u, USART_BAUD_OVS_16, &d), 0);
}

TU_TEST(monotone)
{
    uint32_t prev = 0u;
    uint32_t i;

    for (i = 1u; i < 8u; i++) {
        uint32_t d = 0u;
        TU_CHECK(accepted(REF_CB3, 115200u * i, USART_BAUD_OVS_16, &d));
        if (i > 1u)
            TU_CHECK(d < prev); /* faster rate, smaller divisor */
        prev = d;
    }
}

static const Tu_Test tests[] = {
    {"board_rate", board_rate},
    {"exact_when_it_divides", exact_when_it_divides},
    {"error_bounded_across_rates", error_bounded_across_rates},
    {"coarser_oversampling_divides_more", coarser_oversampling_divides_more},
    {"refusals", refusals},
    {"monotone", monotone},
};

int main(void)
{
    Tu_Run("usart_baud", tests, (int)(sizeof(tests) / sizeof(tests[0])));
    return 0;
}
