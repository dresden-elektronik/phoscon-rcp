/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* samr21_uart.h — the SAMR21 side of the wired host link (docs/hardware_samr21.md §9): what a
 * SERCOM5 USART owes the hal/uart_drv.h contract, and what only this silicon cares about.
 *
 * The entry points are declared in hal/uart_drv.h, which is what com/uart.c includes so the
 * adapter names no silicon (docs/boards.md §2). What lives here is the part-specific sizing:
 * SERCOM5 as an async 8N1 USART on PA24 (PAD2, TxD) and PA25 (PAD3, RxD) — the pad pair a
 * ConBee II spends on USB DM/DP — one armed chunk out, one bounded run in per vector entry,
 * and a vector that only moves octets and calls the installed handler. */
#ifndef SAMR21_UART_H
#define SAMR21_UART_H

#include <stdint.h>

#include "uart_drv.h"

/* One write of this link. Equal to COM_TX_MAX (hal/com.h), which samr21_uart.c asserts
 * rather than assumes, because the adapter sizes its rings around COM_TX_MAX. */
#define UART_TX_MAX 64u

/* Octets taken per vector entry. The receiver holds two octets and a third in its shift
 * register (datasheet 24.7), so this bounds one entry, not a queue. */
#define UART_RX_RUN_MAX 32u

#endif
