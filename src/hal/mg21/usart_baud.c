/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* usart_baud.c — the Series 2 USART divisor, in 32 bits.
 *
 * CLKDIV = 256 (ref/(S*baud) - 1) with five implemented fractional bits, so the field is
 * always a multiple of 8 and the equation is evaluated as
 *
 *     q      = (32*ref + S*baud/2) / (S*baud)      q = 32*divisor, rounded
 *     CLKDIV = (q - 32) * 8
 *
 * The rounding term in the numerator is what makes the divisor the nearest representable one
 * rather than the next-wider one. 32*ref must not overflow, which bounds the reference this
 * helper accepts; the divisor field bounds the narrowest rate. No 64-bit arithmetic: the
 * target has no helper for it and the scaling above is exact for any reference the part can
 * produce. */
#include <stdint.h>

#include "usart_baud.h"

/* Largest reference for which 32*ref + (S*baud)/2 stays inside 32 bits. S*baud is itself
 * below ref by the sampling rule, so half of it is below ref/2 and the sum is below 33*ref. */
#define USART_BAUD_REF_MAX 130000000u

static int oversampleOk(uint32_t oversample)
{
    return oversample == USART_BAUD_OVS_16 || oversample == USART_BAUD_OVS_8
           || oversample == USART_BAUD_OVS_6 || oversample == USART_BAUD_OVS_4;
}

int UsartBaud_ClkDiv(uint32_t refHz, uint32_t baud, uint32_t oversample, uint32_t *out)
{
    uint32_t samples;
    uint32_t q;

    if (out == 0 || baud == 0u || refHz == 0u || refHz > USART_BAUD_REF_MAX)
        return 0;
    if (!oversampleOk(oversample))
        return 0;

    samples = baud * oversample;
    if (samples < baud || samples > refHz) /* the product must not wrap, and the rate must be
                                              sampleable at this oversampling factor */
        return 0;

    q = (32u * refHz + samples / 2u) / samples;
    if (q < 32u) /* divisor below 1: the reference is not slow enough to be divided down to
                    this rate */
        return 0;

    *out = (q - 32u) * USART_BAUD_CLKDIV_STEP;
    if (*out > USART_BAUD_CLKDIV_MAX)
        return 0;

    return 1;
}

uint32_t UsartBaud_RateHz(uint32_t refHz, uint32_t oversample, uint32_t clkdiv)
{
    uint32_t num;
    uint32_t den;

    if (refHz == 0u || oversample == 0u)
        return 0u;

    /* ref / (S * (1 + clkdiv/256)) = (ref/S) * 256 / (256 + clkdiv), in that order to keep
     * the products inside 32 bits. Dividing ref first costs at most one hertz of rounding. */
    num = (refHz / oversample) * USART_BAUD_CLKDIV_SCALE;
    den = USART_BAUD_CLKDIV_SCALE + clkdiv;

    if (den == 0u)
        return 0u;

    return num / den;
}
