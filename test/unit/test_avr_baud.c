/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* test_avr_baud.c — AvrBaud_Ubrr / AvrBaud_RateHz (docs/testing.md §2).
 *
 * The divisor decides whether the stick and its host agree on a bit rate, and a wrong one is a link
 * that talks at a rate neither side asked for. The expected values are the datasheet's own: the
 * "Examples of UBRRn Settings" table gives UBRR and the error for a 16 MHz and an 8 MHz oscillator, and
 * the RaspBee I bootloader programs UBRR0 = 12 for 38 400 at 8 MHz (docs/hardware_avr.md §11.2). */
#include <stdint.h>

#include "avr_baud.h"
#include "tu.h"

#define F_16M 16000000u /* cbrb1: the transceiver crystal, prescaler cleared */
#define F_8M 8000000u   /* the bootloader's and the vendor application's clock */

TU_TEST(the_board_rate_at_16_mhz)
{
    uint16_t ubrr = 0xFFFFu;
    int u2x = -1;

    TU_EQ(AvrBaud_Ubrr(F_16M, 38400u, &ubrr, &u2x), 1);
    TU_EQ(ubrr, 25u); /* the table: 38.4k at 16 MHz, U2Xn = 0, UBRR 25, +0.2 % */
    TU_EQ(u2x, 0);
    TU_EQ(AvrBaud_RateHz(F_16M, ubrr, u2x), 38462u);
}

TU_TEST(the_bootloaders_rate_at_8_mhz)
{
    uint16_t ubrr = 0u;
    int u2x = -1;

    /* Both modes give 38 462 here; normal speed tolerates more and wins the tie. */
    TU_EQ(AvrBaud_Ubrr(F_8M, 38400u, &ubrr, &u2x), 1);
    TU_EQ(ubrr, 12u);
    TU_EQ(u2x, 0);
}

TU_TEST(a_rate_outside_the_recommended_error_is_refused)
{
    uint16_t ubrr = 0xABCDu;
    int u2x = 7;

    /* The table: 115.2k at 16 MHz is UBRR 8 at -3.5 % or UBRR 16 with U2Xn at +2.1 %; neither is inside
     * the +-2.0 % / +-1.5 % recommended for eight data bits. */
    TU_EQ(AvrBaud_Ubrr(F_16M, 115200u, &ubrr, &u2x), 0);
    TU_EQ(ubrr, 0xABCDu);
    TU_EQ(u2x, 7);
    TU_EQ(AvrBaud_RateHz(F_16M, 8u, 0), 111111u);
    TU_EQ(AvrBaud_RateHz(F_16M, 16u, 1), 117647u);
}

TU_TEST(double_speed_when_only_it_reaches_the_rate)
{
    uint16_t ubrr = 0u;
    int u2x = 0;

    /* 2 Mbit/s at 16 MHz: normal speed tops out at 1 Mbit/s, double speed with UBRR 0 is exact. */
    TU_EQ(AvrBaud_Ubrr(F_16M, 2000000u, &ubrr, &u2x), 1);
    TU_EQ(ubrr, 0u);
    TU_EQ(u2x, 1);

    /* 57.6k at 16 MHz, Table 23-16: UBRR 16 at +2.1 % normal is outside +-2.0 %, UBRR 34 at -0.8 % double
     * is inside: double speed by error alone. */
    TU_EQ(AvrBaud_Ubrr(F_16M, 57600u, &ubrr, &u2x), 1);
    TU_EQ(ubrr, 34u);
    TU_EQ(u2x, 1);

    /* 76.8k at 16 MHz: UBRR 12 at +0.2 % normal, UBRR 25 at +0.2 % double - a tie, normal wins. */
    TU_EQ(AvrBaud_Ubrr(F_16M, 76800u, &ubrr, &u2x), 1);
    TU_EQ(ubrr, 12u);
    TU_EQ(u2x, 0);
}

TU_TEST(exact_rates_and_the_field_bound)
{
    uint16_t ubrr = 0u;
    int u2x = 1;

    TU_EQ(AvrBaud_Ubrr(F_16M, 250000u, &ubrr, &u2x), 1);
    TU_EQ(ubrr, 3u);
    TU_EQ(u2x, 0);

    TU_EQ(AvrBaud_Ubrr(F_16M, 1000000u, &ubrr, &u2x), 1);
    TU_EQ(ubrr, 0u);
    TU_EQ(u2x, 0);

    /* 300 bit/s needs UBRR 3332: inside the 12-bit field. 200 bit/s needs 4999 normal, 9999 double. */
    TU_EQ(AvrBaud_Ubrr(F_16M, 300u, &ubrr, &u2x), 1);
    TU_EQ(ubrr, 3332u);
    TU_EQ(AvrBaud_Ubrr(F_16M, 200u, &ubrr, &u2x), 0);
}

TU_TEST(nonsense_is_refused)
{
    uint16_t ubrr = 0u;
    int u2x = 0;

    TU_EQ(AvrBaud_Ubrr(0u, 38400u, &ubrr, &u2x), 0);
    TU_EQ(AvrBaud_Ubrr(F_16M, 0u, &ubrr, &u2x), 0);
    TU_EQ(AvrBaud_Ubrr(F_16M, 38400u, 0, &u2x), 0);
    TU_EQ(AvrBaud_Ubrr(F_16M, 38400u, &ubrr, 0), 0);
    TU_EQ(AvrBaud_Ubrr(F_16M, 4000000u, &ubrr, &u2x), 0); /* above f/8 */
}

static const Tu_Test tests[] = {
    {"the_board_rate_at_16_mhz", the_board_rate_at_16_mhz},
    {"the_bootloaders_rate_at_8_mhz", the_bootloaders_rate_at_8_mhz},
    {"a_rate_outside_the_recommended_error_is_refused", a_rate_outside_the_recommended_error_is_refused},
    {"double_speed_when_only_it_reaches_the_rate", double_speed_when_only_it_reaches_the_rate},
    {"exact_rates_and_the_field_bound", exact_rates_and_the_field_bound},
    {"nonsense_is_refused", nonsense_is_refused},
};

int main(void)
{
    Tu_Run("avr_baud", tests, (int)(sizeof(tests) / sizeof(tests[0])));
    return 0;
}
