/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* avr_baud.h — the ATmega256RFR2 USART line-rate divisor (docs/hardware_avr.md §11.2).
 *
 * Portable core, not glue: no register, no clock, no HAL. The oscillator frequency and the wanted rate
 * arrive as arguments, so the host suite can pin the exact UBRR the USART will be programmed with - the
 * same reason the SERCOM and the EFR32 USART have their own helpers. The UART driver in hal/avr/ calls it
 * and writes the result to UBRRn and the U2Xn bit.
 *
 * The USART divides the oscillator by 16 (normal speed) or 8 (U2Xn = 1, double speed) and by UBRR + 1
 * (datasheet Table 23-1), so
 *
 *     BAUD = f / (S * (UBRR + 1)),   S = 16 or 8,   UBRR 0..4095 (12 bits)
 *
 * Rounding UBRR to the nearest divisor keeps the achieved rate on both sides of the target. A rate is
 * accepted only inside the receiver error the datasheet recommends for 8 data bits and no parity:
 * +-2.0 % at normal speed (Table 23-2), +-1.5 % at double speed (Table 23-3). Normal speed tolerates more,
 * so it wins a tie. */
#ifndef AVR_BAUD_H
#define AVR_BAUD_H

#include <stdint.h>

#define AVR_BAUD_UBRR_MAX 4095u
#define AVR_BAUD_ERR_NORMAL_BP 200u /* basis points: +-2.0 %, Table 23-2, D = 8 */
#define AVR_BAUD_ERR_DOUBLE_BP 150u /* +-1.5 %, Table 23-3, D = 8 */

/* UBRR and the U2Xn choice for `fOscHz` against `baud`: the mode with the smaller error, normal speed on
 * a tie. 0 when neither mode reaches the rate inside its recommended error, or the divisor does not fit
 * the 12-bit field; the outputs are left alone then. */
int AvrBaud_Ubrr(uint32_t fOscHz, uint32_t baud, uint16_t *ubrr, int *doubleSpeed);

/* The rate the USART shifts with `ubrr` in that mode, f / (S * (UBRR + 1)), rounded to the nearest hertz.
 * The driver reports it next to the requested rate, so a board's witnesses show the error. */
uint32_t AvrBaud_RateHz(uint32_t fOscHz, uint16_t ubrr, int doubleSpeed);

#endif /* AVR_BAUD_H */
