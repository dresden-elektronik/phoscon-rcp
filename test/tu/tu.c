/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* tu.c — the harness itself (test/tu/tu.h); host-side, so stdio is allowed. */
#include <stdio.h>
#include <stdlib.h>

#include "tu.h"

static int tuFailed;
static int tuChecks;
static const char *tuCurrentTest;

void Tu_Check(int ok, const char *what, const char *file, int line)
{
    tuChecks++;

    if (!ok)
    {
        tuFailed++;
        printf(
            "    FAIL %s:%d: %s  (in %s)\n", file, line, what, tuCurrentTest ? tuCurrentTest : "?");
    }
}

void Tu_CheckEq(long got, long want, const char *what, const char *file, int line)
{
    tuChecks++;

    if (got != want)
    {
        tuFailed++;
        printf("    FAIL %s:%d: %s  got %ld, want %ld  (in %s)\n",
               file,
               line,
               what,
               got,
               want,
               tuCurrentTest ? tuCurrentTest : "?");
    }
}

void Tu_Note(const char *what, long value)
{
    printf("    note %s = %ld\n", what, value);
}

void Tu_Run(const char *suite, const Tu_Test *tests, int count)
{
    int i;
    int failedBefore;

    /* Line-buffered: a suite that dies mid-run must still show the test it died in. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("== %s: %d test(s)\n", suite, count);

    for (i = 0; i < count; ++i)
    {
        failedBefore = tuFailed;
        tuCurrentTest = tests[i].name;
        tests[i].fn();
        printf("  %-44s %s\n", tests[i].name, (tuFailed == failedBefore) ? "ok" : "FAILED");
    }

    printf("== %s: %d check(s), %d failure(s)\n", suite, tuChecks, tuFailed);
    exit(tuFailed == 0 ? 0 : 1);
}
