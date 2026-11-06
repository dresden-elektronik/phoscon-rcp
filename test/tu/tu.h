/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* tu.h — the in-tree C99 test harness (docs/testing.md §2). */
#ifndef TU_H
#define TU_H

typedef void (*Tu_Fn)(void);

typedef struct
{
    const char *name;
    Tu_Fn fn;
} Tu_Test;

void Tu_Run(const char *suite, const Tu_Test *tests, int count);

void Tu_Check(int ok, const char *what, const char *file, int line);
void Tu_CheckEq(long got, long want, const char *what, const char *file, int line);
void Tu_Note(const char *what, long value);

#define TU_TEST(name) static void name(void)
#define TU_CHECK(cond) Tu_Check(((cond) ? 1 : 0), #cond, __FILE__, __LINE__)
#define TU_EQ(got, want)                                                                           \
    Tu_CheckEq((long)(got), (long)(want), #got " == " #want, __FILE__, __LINE__)
#define TU_NE(got, notwant)                                                                        \
    Tu_Check(((long)(got) != (long)(notwant)) ? 1 : 0, #got " != " #notwant, __FILE__, __LINE__)
#define TU_NOTE(what, value) Tu_Note((what), (long)(value))

#endif
