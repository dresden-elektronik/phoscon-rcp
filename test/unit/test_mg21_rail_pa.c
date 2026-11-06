/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* test_mg21_rail_pa.c — the xG21 transmit power curves and the dBm-to-level lookup
   (hal/mg21/rail/mg21_rail_pa.h).

   The expected levels were computed by an independent implementation of the SDK plugin's algorithm
   (start at the segment the 200/40 deci-dBm spacing names, step to the next while the level is not above
   that segment's ceiling, +92 rounding, caps), over the same published segment table; the properties
   (monotonic in the request, never above a mode's range, a mode with no curve gives the lowest level)
   hold whatever the table. */

#include <stdint.h>

#include "mg21_rail_pa.h"
#include "tu.h"

TU_TEST(levels_at_the_documented_points)
{
    /* HP: 20 dBm is the top of the curve, 10 dBm the third segment. */
    TU_EQ(Mg21RailPa_Level(MG21_RAIL_PA_HP, 200), 159);
    TU_EQ(Mg21RailPa_Level(MG21_RAIL_PA_HP, 100), 36);
    TU_EQ(Mg21RailPa_Level(MG21_RAIL_PA_HP, 0), 10);
    TU_EQ(Mg21RailPa_Level(MG21_RAIL_PA_HP, -100), 3);

    /* MP: the 10 dBm amplifier of this board's part. */
    TU_EQ(Mg21RailPa_Level(MG21_RAIL_PA_MP, 100), 68);
    TU_EQ(Mg21RailPa_Level(MG21_RAIL_PA_MP, 0), 10);
    TU_EQ(Mg21RailPa_Level(MG21_RAIL_PA_MP, -100), 3);

    /* LP: 0 dBm tops out at the last segment's ceiling. */
    TU_EQ(Mg21RailPa_Level(MG21_RAIL_PA_LP, 0), 44);
    TU_EQ(Mg21RailPa_Level(MG21_RAIL_PA_LP, -100), 10);
}

TU_TEST(requests_outside_the_curve_are_clamped)
{
    TU_EQ(Mg21RailPa_Level(MG21_RAIL_PA_MP, 300), Mg21RailPa_Level(MG21_RAIL_PA_MP, 100));
    TU_EQ(Mg21RailPa_Level(MG21_RAIL_PA_MP, 20000), 68);
    TU_EQ(Mg21RailPa_Level(MG21_RAIL_PA_HP, -400), 1);
    TU_EQ(Mg21RailPa_Level(MG21_RAIL_PA_MP, -32768), 1);
    TU_EQ(Mg21RailPa_Level(MG21_RAIL_PA_LP, -400), 0);
    TU_EQ(Mg21RailPa_MaxDeciDbm(MG21_RAIL_PA_MP), 100);
    TU_EQ(Mg21RailPa_MinDeciDbm(MG21_RAIL_PA_MP), -260);
}

TU_TEST(a_mode_without_a_curve_gives_the_lowest_level)
{
    TU_EQ(Mg21RailPa_Level(3, 100), 0);
    TU_EQ(Mg21RailPa_Level(4, 100), 0);
    TU_EQ(Mg21RailPa_Level(-1, 100), 0);
    TU_EQ(Mg21RailPa_MaxDeciDbm(4), 0);
}

TU_TEST(the_level_never_falls_as_the_request_rises)
{
    int mode;
    int p;

    for (mode = MG21_RAIL_PA_HP; mode <= MG21_RAIL_PA_LP; ++mode)
    {
        uint8_t prev = Mg21RailPa_Level(mode, Mg21RailPa_MinDeciDbm(mode));

        for (p = Mg21RailPa_MinDeciDbm(mode); p <= Mg21RailPa_MaxDeciDbm(mode); p += 10)
        {
            uint8_t now = Mg21RailPa_Level(mode, (int16_t)p);

            TU_CHECK(now >= prev);
            prev = now;
        }
    }
}

static const Tu_Test tests[] = {
    {"levels_at_the_documented_points", levels_at_the_documented_points},
    {"requests_outside_the_curve_are_clamped", requests_outside_the_curve_are_clamped},
    {"a_mode_without_a_curve_gives_the_lowest_level", a_mode_without_a_curve_gives_the_lowest_level},
    {"the_level_never_falls_as_the_request_rises", the_level_never_falls_as_the_request_rises},
};

int main(void)
{
    Tu_Run("mg21_rail_pa", tests, (int)(sizeof(tests) / sizeof(tests[0])));
    return 0;
}
