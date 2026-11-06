/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* avr_uart.h — the AVR side of the wired host link (docs/hardware_avr.md §11.1, §11.8).
 *
 * What the hal/uart_drv.h contract needs from this silicon: USART0 as an async 8N1 UART on PE1 (TXD0)
 * and PE0 (RXD0) at 38 400, the pair and the rate the bootloader speaks, so the bootloader, the vendor
 * firmware and an image built here cannot disagree about the wire. On the ConBee I the pair goes to an
 * FTDI bridge; on the RaspBee I to the Pi's header. */
#ifndef AVR_UART_H
#define AVR_UART_H

#include <stdint.h>

#include "uart_drv.h"

/* One write of this link: equal to COM_TX_MAX (hal/com.h), asserted in avr_uart.c. */
#define UART_TX_MAX 64u

/* Octets taken per vector entry: the receiver holds two and a third in the shift register. */
#define UART_RX_RUN_MAX 4u

/* Let the last octet leave and switch USART0 off, before a reset into the bootloader, which sets the
 * block up for itself. Called with interrupts masked. */
void Uart_Release(void);

#endif /* AVR_UART_H */
