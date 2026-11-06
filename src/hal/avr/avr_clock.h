/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* avr_clock.h — what the ATmega256RFR2 clock module offers its own board glue (docs/hardware_avr.md
 * §11.12). Not in hal/hal.h: these are this silicon's facts.
 *
 * The system clock is the transceiver's 16 MHz crystal (fuse CKSEL = 1110, §11.10). The bootloader
 * divides it by two and leaves it so; this image clears the prescaler and runs at 16 MHz, and reads
 * the prescaler back, because every divisor on the board - Timer/Counter0's compare value, the
 * USART's UBRR - is computed from the clock the read-back says, not from the one the board is
 * supposed to have. */
#ifndef AVR_CLOCK_H
#define AVR_CLOCK_H

#include <stdint.h>

/* The crystal behind the system clock: the transceiver's, by the fuses. */
#define AVR_CLOCK_XTAL_HZ 16000000u

/* What Clock_Init returns. */
#define AVR_CLOCK_DOWN 0 /* no millisecond: the image cannot keep time and must not open a link */
#define AVR_CLOCK_FULL 1 /* the prescaler cleared: 16 MHz */
#define AVR_CLOCK_DIVIDED 2 /* the prescaler did not clear: the read-back's rate, which the tick and
                             * the link are then computed for */

int Clock_Init(void);

/* The system clock the read-back of CLKPR gives. */
uint32_t Clock_CpuHz(void);

/* Hand the millisecond from the polled compare flag (bring-up, interrupts masked) to the
 * Timer/Counter0 vector. Called once, as HAL_Init unlocks interrupts. */
void Clock_TickCountedByIrq(void);

/* The bring-up read-backs, as records (DBG_COMP_WITNESS). */
void Clock_WitnessReport(void);

#endif /* AVR_CLOCK_H */
