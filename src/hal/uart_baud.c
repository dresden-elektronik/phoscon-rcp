/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* uart_baud.c — the arithmetic baud divisor (Table 23-2).
 *
 * BAUD = 65536 (1 - S*baud/ref). The product S*baud*65536 does not fit 32 bits, so the
 * scaled division runs bit by bit: the remainder stays below the divisor, so doubling it
 * cannot overflow - which is also why the caller's rate must stay under ref/S. */
#include <stdint.h>

#include "uart_baud.h"

/* (num * 65536) / den, for num < den. */
static uint32_t scale65536(uint32_t num, uint32_t den)
{
    uint32_t rem = num;
    uint32_t q = 0u;
    uint32_t i;

    for (i = 0; i < 16u; ++i)
    {
        q <<= 1;
        rem <<= 1;

        if (rem >= den)
        {
            rem -= den;
            q |= 1u;
        }
    }

    return q;
}

int UartBaud_Register(uint32_t refHz, uint32_t baud, uint16_t *out)
{
    uint32_t samples;

    if (out == 0 || baud == 0u || refHz == 0u)
        return 0;

    samples = baud * UART_BAUD_SAMPLES;

    if (samples < baud || samples >= refHz) /* the product must not wrap either */
        return 0;

    *out = (uint16_t)(65536u - scale65536(samples, refHz));
    return 1;
}
