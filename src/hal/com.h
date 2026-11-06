/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* com.h — the host-link contract (docs/boards.md §4, docs/architecture.md §2).
 * The byte pipe between the host and the RCP. One implementation per link: the
 * ConBee II's native USB CDC in com/usb_cdc.c now, a UART link later; the
 * implementation is selected at link time, so --gc-sections drops the unused
 * one. Everything above this contract is link-agnostic. A link's own
 * diagnostics (the ConBee II's USBC witnesses and CDC ACM notifications) are
 * the implementation's, not the pipe's: they live in com/usb_cdc.h.
 *
 * The pipe guarantees, and they are the same rules the USB device API has:
 *   - Ordered bytes: what is handed to COM_TxSend leaves in the order written.
 *   - A single write is at most COM_TX_MAX bytes; a larger one is refused, not
 *     split. A transfer may span writes, so this is a transaction size, not a
 *     ceiling on a message.
 *   - COM_TxSend never blocks: it arms one write and reports busy (0) when the
 *     previous one has not been retired; the caller retries.
 *   - The receive handler runs in the link's own context (the USB vector, or the
 *     SERCOM vector), so it only queues or copies - never blocks, encodes, or
 *     touches flash.
 *
 * Which implementation a build carries is the board's decision: its board.cmake names
 * the link sources (docs/boards.md §4), so exactly one link is ever compiled in. */
#ifndef COM_H
#define COM_H

#include <stdint.h>

/* Largest single write the link accepts (today one full-speed CDC packet). */
#define COM_TX_MAX 64u

/* ---- The pipe ---- */
/* Bring the link's hardware up, once the clocks are (the board glue calls it in
 * hal/samr21/hal_samr21.c). 1 = the link can be armed. */
int COM_Init(void);

/* 1 once the link is up: for CDC, SET_CONFIGURATION landed and the data endpoints are
 * open; for a wired UART, the SERCOM is enabled. */
int COM_Present(void);

/* Clear/set the host's connect edge (today: the D+ pull-up, 29.8.2.1); a no-op on a link
 * that has none. */
void COM_Attach(int on);

/* Install the receive handler; it runs in the link's own context. Pass 0 to
 * clear. */
void COM_SetRxHandler(void (*fn)(const uint8_t *, uint16_t));

/* Arm one write of at most COM_TX_MAX bytes. 1 = accepted (bytes copied), 0 =
 * a previous write is unacknowledged or len is over one write. The buffer is
 * not retained. Main loop only. */
int COM_TxSend(const uint8_t *buf, uint16_t len);

/* Arm one write from the link's own queue (today the debug-record queue).
 * Returns the byte count armed, 0 if empty or a write is outstanding. Never
 * waits or stalls; main loop only. */
uint16_t COM_TxPoll(void);

/* The link's control channel and its register witnesses. Neither exists on a board whose link has no
 * control endpoint, and the shim answers 0 or does nothing there - the same way COM_Attach is a no-op
 * on a board without D+. */
uint16_t COM_NotifyPoll(void);
void COM_Witnesses(void);

#endif
