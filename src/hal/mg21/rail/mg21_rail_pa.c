/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mg21_rail_pa.c — the xG21 2.4 GHz power curves and the dBm-to-level lookup (mg21_rail_pa.h).
 *
 * The numbers are the SDK's own: sl_rail_util_pa_curves.h of the pinned Simplicity SDK release
 * (docs/build-and-flash.md §10) publishes, per amplifier, eight line segments { highest raw level the
 * segment covers, slope, intercept } with level = (slope * deciDbm + intercept) / 1000, plus the range of
 * powers the fit covers. The lookup is written here: start at the segment the curve family's spacing
 * suggests (200 deci-dBm at the top, 40 per segment), then move to the next segment for as long as the
 * level it gives is not above that segment's own ceiling, because a segment is only valid up to its
 * ceiling; add 92 before dividing, the SDK's measured allowance that keeps the result at or below the
 * requested power; cap at the segment's ceiling and at the mode's level range. */
#include <stdint.h>

#include "mg21_rail_pa.h"

#define PA_SEGMENTS 8u
#define PA_FIRST_DECI_DBM 200
#define PA_STEP_DECI_DBM 40
#define PA_ROUNDING 92

typedef struct
{
    uint16_t maxLevel; /* the highest raw level this segment covers; 65535 = the segment is unused */
    uint16_t slope;
    int32_t intercept;
} PaSegment;

typedef struct
{
    int16_t maxDeciDbm;
    int16_t minDeciDbm;
    uint8_t minLevel;
    uint8_t maxLevel;
    PaSegment seg[PA_SEGMENTS];
} PaCurve;

static const PaCurve paCurves[] = {
    /* HP: RAIL_PA_CURVES_2P4_HP_VBAT, levels 1..180 */
    {200, -260, 1u, 180u,
     {{180, 2187, -278405}, {77, 740, -43289}, {44, 418, -5245}, {26, 259, 6835},
      {17, 163, 10581}, {10, 94, 10330}, {6, 58, 8737}, {4, 24, 5747}}},
    /* MP: RAIL_PA_CURVES_2P4_MP_VBAT, levels 1..90 */
    {100, -260, 1u, 90u,
     {{65535, 0, 0}, {65535, 0, 0}, {90, 1779, -109403}, {33, 419, -351},
      {17, 166, 10115}, {10, 97, 9948}, {6, 59, 8440}, {3, 21, 5037}}},
    /* LP: RAIL_PA_CURVES_2P4_LP, levels 0..64 */
    {0, -260, 0u, 64u,
     {{65535, 0, 0}, {65535, 0, 0}, {65535, 0, 0}, {65535, 0, 0},
      {64, 1560, 54004}, {44, 688, 48701}, {23, 242, 31756}, {12, 65, 17054}}}};

#define PA_MODES ((int)(sizeof(paCurves) / sizeof(paCurves[0])))

int16_t Mg21RailPa_MaxDeciDbm(int mode)
{
    return (mode >= 0 && mode < PA_MODES) ? paCurves[mode].maxDeciDbm : 0;
}

int16_t Mg21RailPa_MinDeciDbm(int mode)
{
    return (mode >= 0 && mode < PA_MODES) ? paCurves[mode].minDeciDbm : 0;
}

static uint32_t paSegmentLevel(const PaSegment *s, int32_t deciDbm)
{
    int32_t v = s->intercept + (int32_t)s->slope * deciDbm;

    if (v < 0)
        v = 0;

    return ((uint32_t)v + PA_ROUNDING) / 1000u;
}

uint8_t Mg21RailPa_Level(int mode, int16_t deciDbm)
{
    const PaCurve *c;
    int32_t p = deciDbm;
    int32_t idx;
    uint32_t level;

    if (mode < 0 || mode >= PA_MODES)
        return 0u;

    c = &paCurves[mode];

    if (p > c->maxDeciDbm)
        p = c->maxDeciDbm;
    else if (p < c->minDeciDbm)
        p = c->minDeciDbm;

    idx = (PA_FIRST_DECI_DBM - p) / PA_STEP_DECI_DBM;

    if (idx < 0)
        idx = 0;
    else if (idx > (int32_t)PA_SEGMENTS - 1)
        idx = (int32_t)PA_SEGMENTS - 1;

    level = paSegmentLevel(&c->seg[idx], p);

    while ((uint32_t)idx + 1u < PA_SEGMENTS && level <= c->seg[idx + 1].maxLevel)
    {
        ++idx;
        level = paSegmentLevel(&c->seg[idx], p);
    }

    if (level > c->seg[idx].maxLevel)
        level = c->seg[idx].maxLevel;

    if (level > c->maxLevel)
        level = c->maxLevel;

    if (level < c->minLevel)
        level = c->minLevel;

    return (uint8_t)level;
}
