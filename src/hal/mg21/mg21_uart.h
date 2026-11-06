/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mg21_uart.h — the MG21 side of the wired host link (docs/hardware_mg21.md §10.3).

What the hal/uart_drv.h contract needs from this silicon, and what only this silicon cares about:
USART0 as an async 8N1 UART on PB1 (TxD) and PB0 (RxD) at 115 200, which is the pair the bootloader's
DeviceConfig CommConfig names, so the bootloader, the vendor firmware and an image built here cannot
disagree about the wire (docs/hardware_mg21.md §10.8). Two things a SERCOM reader has to relearn: the pads are
routed by the GPIO block rather than by the USART, and the receiver reports a bad frame per octet in
RXDATAX rather than as a status flag beside the data. */
#ifndef MG21_UART_H
#define MG21_UART_H

#include <stdint.h>

#include "uart_drv.h"

/* One write of this link: equal to COM_TX_MAX (hal/com.h), asserted rather than assumed, because the
 * adapter sizes its rings around it. */
#define UART_TX_MAX 64u

/* Octets taken per vector entry. The receiver holds a FIFO, not two shift registers, so the bound is
 * about the loop's stack use rather than the peripheral's depth. */
#define UART_RX_RUN_MAX 16u

/* Hand USART0 and its pads back as reset left them, before a reset into the bootloader, which
 * configures the same block and pins for itself: interrupts off, the block disabled, the route and
 * the PB0/PB1 modes cleared. Called with interrupts masked. */
void Uart_Release(void);

#endif /* MG21_UART_H */
