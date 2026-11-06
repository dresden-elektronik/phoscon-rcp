/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* avr_baud.c — the USART divisor, in 32 bits (avr_baud.h).
 *
 * For each sampling factor S the nearest divisor is q = (f + S*baud/2) / (S*baud), UBRR = q - 1, and
 * its error is |rate - baud| / baud in basis points. S*baud is bounded by f before it is formed, so
 * nothing wraps for any oscillator the part runs at; the error is formed only once the difference is
 * below 5 % of the rate, which keeps the scaled difference inside 32 bits. No 64-bit arithmetic: the
 * target has no cheap helper for it. */
#include <stdint.h>

#include "avr_baud.h"

#define AVR_BAUD_S_NORMAL 16u
#define AVR_BAUD_S_DOUBLE 8u

uint32_t AvrBaud_RateHz(uint32_t fOscHz, uint16_t ubrr, int doubleSpeed)
{
    uint32_t div = (doubleSpeed ? AVR_BAUD_S_DOUBLE : AVR_BAUD_S_NORMAL) * ((uint32_t)ubrr + 1u);

    return (fOscHz + div / 2u) / div;
}

/* 1 = a divisor in the field; *errBp is its error in basis points. */
static int baudTry(uint32_t fOscHz, uint32_t baud, int doubleSpeed, uint16_t *ubrr, uint32_t *errBp)
{
    uint32_t s = doubleSpeed ? AVR_BAUD_S_DOUBLE : AVR_BAUD_S_NORMAL;
    uint32_t samples;
    uint32_t q;
    uint32_t rate;
    uint32_t diff;

    if (baud > fOscHz / s) /* the divided oscillator cannot sample this rate */
        return 0;

    samples = s * baud;
    q = (fOscHz + samples / 2u) / samples;

    if (q < 1u || q - 1u > AVR_BAUD_UBRR_MAX)
        return 0;

    rate = AvrBaud_RateHz(fOscHz, (uint16_t)(q - 1u), doubleSpeed);
    diff = (rate > baud) ? rate - baud : baud - rate;

    if (diff > baud / 20u)
        return 0;

    *ubrr = (uint16_t)(q - 1u);
    *errBp = (diff * 10000u + baud / 2u) / baud;
    return 1;
}

int AvrBaud_Ubrr(uint32_t fOscHz, uint32_t baud, uint16_t *ubrr, int *doubleSpeed)
{
    uint16_t normal = 0u;
    uint16_t dbl = 0u;
    uint32_t normalErr = 0u;
    uint32_t dblErr = 0u;
    int normalOk;
    int dblOk;

    if (ubrr == 0 || doubleSpeed == 0 || fOscHz == 0u || baud == 0u)
        return 0;

    normalOk = baudTry(fOscHz, baud, 0, &normal, &normalErr) && normalErr <= AVR_BAUD_ERR_NORMAL_BP;
    dblOk = baudTry(fOscHz, baud, 1, &dbl, &dblErr) && dblErr <= AVR_BAUD_ERR_DOUBLE_BP;

    if (normalOk && (!dblOk || normalErr <= dblErr))
    {
        *ubrr = normal;
        *doubleSpeed = 0;
        return 1;
    }

    if (dblOk)
    {
        *ubrr = dbl;
        *doubleSpeed = 1;
        return 1;
    }

    return 0;
}
