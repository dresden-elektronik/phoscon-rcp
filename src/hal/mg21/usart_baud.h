/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* usart_baud.h — the EFR32 Series 2 USART line-rate divisor (docs/hardware_mg21.md §10).
 *
 * Portable core, not glue: no register, no clock, no HAL. The reference frequency, the
 * wanted rate and the oversampling factor arrive as arguments, so the host suite can pin the
 * exact CLKDIV the silicon will be programmed with — the same reason the SERCOM side has
 * hal/uart_baud.c (docs/testing.md §2). The USART driver in hal/mg21/ calls it and writes
 * the result to CLKDIV.
 *
 * The USART divides its input clock by (1 + CLKDIV/256) and samples each bit that many
 * oversampling periods, so
 *
 *     CLKDIV = 256 * ( ref / (oversample * baud) - 1 )
 *
 * CLKDIV carries 15 integer bits and 5 fractional bits (bits 22..3 of the register; bits
 * 2..0 are not implemented), so the divisor is a multiple of 1/32. Rounding to the nearest
 * representable divisor is what keeps the achieved rate on both sides of the target instead
 * of always slow. */
#ifndef MG21_USART_BAUD_H
#define MG21_USART_BAUD_H

#include <stdint.h>

/* Oversampling factors the USART offers for an asynchronous frame. The value is the
 * multiplier the divisor equation uses; the driver maps it to the CTRL field encoding. */
#define USART_BAUD_OVS_16 16u
#define USART_BAUD_OVS_8 8u
#define USART_BAUD_OVS_6 6u
#define USART_BAUD_OVS_4 4u

/* CLKDIV's layout: 5 implemented fractional bits, so the field is 256 * (divisor - 1)
 * rounded to a multiple of 8, and the largest divisor the field can hold. */
#define USART_BAUD_CLKDIV_SCALE 256u
#define USART_BAUD_CLKDIV_STEP 8u
#define USART_BAUD_CLKDIV_MAX 0x7FFFF8u

/* CLKDIV for `refHz` against `baud` at `oversample`. 0 when the combination is not
 * realisable: an oversampling factor the part does not offer, a rate the divided reference
 * cannot sample (baud > ref/oversample), or a divisor wider than the field. */
int UsartBaud_ClkDiv(uint32_t refHz, uint32_t baud, uint32_t oversample, uint32_t *out);

/* The rate the USART actually shifts when CLKDIV is programmed with `clkdiv`:
 * f = ref / (oversample * (1 + clkdiv/256)), rounded to the nearest hertz. The driver
 * records this next to the requested rate so a board's witnesses show the error rather
 * than hiding it. */
uint32_t UsartBaud_RateHz(uint32_t refHz, uint32_t oversample, uint32_t clkdiv);

#endif /* MG21_USART_BAUD_H */
