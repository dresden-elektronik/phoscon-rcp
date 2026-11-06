/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* test_harness.c — see test_harness.h. */
#include <stdio.h>

#include "test_harness.h"

static const char *harnessName;
static uint32_t harnessChecks;
static uint32_t harnessFailed;

void Test_Begin(const char *name)
{
    harnessName = name;
    harnessChecks = 0u;
    harnessFailed = 0u;
}

void Test_Expect(int ok, const char *what)
{
    harnessChecks++;

    if (!ok)
    {
        harnessFailed++;
        printf("FAIL %s: %s\n", harnessName, what);
    }
}

void Test_ExpectOctets(const char *what, const uint8_t *got, uint32_t gotLen, const uint8_t *want, uint32_t wantLen)
{
    uint32_t i;
    int same = gotLen == wantLen;

    for (i = 0u; same && i < gotLen; ++i)
    {
        if (got[i] != want[i])
            same = 0;
    }

    Test_Expect(same, what);
}

static int harnessNibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';

    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;

    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;

    return -1;
}

uint32_t Test_ParseHex(const char *hex, uint8_t *out, uint32_t max)
{
    uint32_t n = 0u;

    while (hex[0] != '\0' && hex[1] != '\0' && n < max)
    {
        int hi = harnessNibble(hex[0]);
        int lo = harnessNibble(hex[1]);

        if (hi < 0 || lo < 0)
            break;

        out[n++] = (uint8_t)((hi << 4) | lo);
        hex += 2;
    }

    return n;
}

int Test_Report(void)
{
    printf("%s: %lu checks, %lu failed\n", harnessName, (unsigned long)harnessChecks, (unsigned long)harnessFailed);
    return harnessFailed != 0u;
}
