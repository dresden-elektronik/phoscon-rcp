/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* test_harness.h — the calls the vendored AES suite makes (unit/test_aes.c), kept unmodified. */
#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include <stdint.h>

void Test_Begin(const char *name);
void Test_Expect(int ok, const char *what);
void Test_ExpectOctets(const char *what, const uint8_t *got, uint32_t gotLen, const uint8_t *want, uint32_t wantLen);
uint32_t Test_ParseHex(const char *hex, uint8_t *out, uint32_t max);
int Test_Report(void);

#endif
