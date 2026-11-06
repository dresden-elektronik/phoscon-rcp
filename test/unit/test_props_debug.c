/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* test_props_debug.c - debug-record packing (src/rcp/props_debug.c). */
#include <stdio.h>
#include <string.h>

#include "props_debug.h"

static int failures;

static void check(const char *what, int ok)
{
    if (!ok)
    {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static void testCapacity(void)
{
    /* The batch bound is the value budget, not the 332-byte escaped frame: 160 - 2 over 8. */
    check("max records is 19", PROPS_DEBUG_MAX == 19u);
    check("one batch fits the value budget",
          PROPS_DEBUG_HEADER + PROPS_DEBUG_MAX * PROPS_DEBUG_RECORD <= PROPS_DEBUG_VALUE_MAX);
    check("one more record would not fit",
          PROPS_DEBUG_HEADER + (PROPS_DEBUG_MAX + 1u) * PROPS_DEBUG_RECORD >
              PROPS_DEBUG_VALUE_MAX);
    /* The budget is restated in props_debug.h to keep it free of the radio HAL. */
    check("budget matches RCP_VALUE_MAX (160)", PROPS_DEBUG_VALUE_MAX == 160u);
}

static void testPack(void)
{
    uint8_t recs[PROPS_DEBUG_MAX * PROPS_DEBUG_RECORD];
    uint8_t out[PROPS_DEBUG_HEADER + PROPS_DEBUG_MAX * PROPS_DEBUG_RECORD];
    uint16_t n;
    uint16_t i;
    int same;

    for (i = 0u; i < (uint16_t)sizeof(recs); ++i)
    {
        recs[i] = (uint8_t)(i + 1u);
    }

    check("empty batch refused", PropsDebug_Pack(out, sizeof(out), recs, 0u) == 0u);
    check("over-long batch refused",
          PropsDebug_Pack(out, sizeof(out), recs, PROPS_DEBUG_MAX + 1u) == 0u);
    check("undersized buffer refused",
          PropsDebug_Pack(out, PROPS_DEBUG_HEADER + 3u * PROPS_DEBUG_RECORD - 1u, recs, 3u) == 0u);
    check("null out refused", PropsDebug_Pack(0, sizeof(out), recs, 1u) == 0u);

    n = PropsDebug_Pack(out, sizeof(out), recs, 3u);
    check("three records pack to 26 bytes", n == 26u);
    check("count header is little-endian u16", out[0] == 3u && out[1] == 0u);
    same = 1;
    for (i = 0u; i < 3u * PROPS_DEBUG_RECORD; ++i)
    {
        if (out[PROPS_DEBUG_HEADER + i] != recs[i])
            same = 0;
    }
    check("records are copied verbatim", same);

    n = PropsDebug_Pack(out, sizeof(out), recs, PROPS_DEBUG_MAX);
    check("a full batch packs exactly", n == PROPS_DEBUG_HEADER + PROPS_DEBUG_MAX * PROPS_DEBUG_RECORD);
    check("a full batch reports its own count",
          out[0] == (uint8_t)(PROPS_DEBUG_MAX & 0xFFu) && out[1] == (uint8_t)(PROPS_DEBUG_MAX >> 8));
}

static void testEnable(void)
{
    uint8_t on = 0x7Fu;
    uint8_t one = 1u;
    uint8_t zero = 0u;
    uint8_t two[2] = {1u, 0u};

    check("an empty write changes nothing", PropsDebug_EnableValue(0, 0u, &on) == 1 && on == 0x7Fu);
    on = 0x7Fu;
    check("u8 1 enables", PropsDebug_EnableValue(&one, 1u, &on) == 1 && on == 1u);
    on = 0x7Fu;
    check("u8 0 disables", PropsDebug_EnableValue(&zero, 1u, &on) == 1 && on == 0u);
    on = 0x7Fu;
    check("two bytes refused", PropsDebug_EnableValue(two, 2u, &on) == 0 && on == 0x7Fu);
    check("null out refused", PropsDebug_EnableValue(&one, 1u, 0) == 0);
}

int main(void)
{
    testCapacity();
    testPack();
    testEnable();
    printf("props_debug: %s (%d failure(s))\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
