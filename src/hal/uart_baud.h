/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* uart_baud.h — the SERCOM USART line-rate divisor (datasheet Table 23-2).
 *
 * Portable core, not glue: no register, no clock, no HAL - the reference frequency and
 * the wanted rate arrive as arguments, so the host suite can pin the exact value the
 * silicon will be programmed with. The SERCOM USART driver calls it and writes the
 * result to BAUD. */
#ifndef UART_BAUD_H
#define UART_BAUD_H

#include <stdint.h>

/* 16x oversampling, arithmetic baud generation: SAMPR 0, and its sample count
 * (Table 24-10, Table 23-2). */
#define UART_BAUD_SAMPLES 16u

/* The 16-bit BAUD register value for `refHz` against `baud`:
 *   BAUD = 65536 (1 - S*baud/ref), S = UART_BAUD_SAMPLES.
 * 0 = not a rate, or beyond what that reference can sample (baud >= ref/S, which is
 * also the case that would need BAUD 0, i.e. the whole reference divided by S). */
int UartBaud_Register(uint32_t refHz, uint32_t baud, uint16_t *out);

#endif
