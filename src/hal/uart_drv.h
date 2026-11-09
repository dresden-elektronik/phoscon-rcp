/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* uart_drv.h — the driver contract a wired host link implements (docs/boards.md §2).
 *
 * `com/uart.c` is the Spinel-on-UART adapter: it frames, encodes and dispatches, and it
 * names no silicon. Below it sits one driver per SoC family — hal/samr21/samr21_uart.c
 * today, hal/mg21/mg21_uart.c for a ConBee III — and the board names which one it links
 * in DS_LINK_SOURCES. A board whose link differs in peripheral instance, pad routing,
 * frame format or buffering (an async USART with no FIFO, say) replaces the driver and
 * reuses the adapter unchanged.
 *
 * What a driver owes, in contract terms:
 *   - Uart_Init brings the peripheral up for the board's wired parameters and arms its
 *     receive interrupt; the framing above it is the adapter's concern. It returns 0 when
 *     the line rate was refused or a bounded enable wait ran out, and names which in a
 *     DBG_COMP_UART record.
 *   - Uart_Present answers whether the link exists at all, which the Spinel dialog reports
 *     through interface status. A hard-wired link answers 1 forever; a link that can be
 *     absent answers from its own attach state.
 *   - Uart_SetRxHandler installs the sink for received runs. It runs in the link's own
 *     interrupt context, so the sink copies and returns, and the driver hands it whole
 *     units, never a partial one (docs/architecture.md §5).
 *   - Uart_TxSend accepts one adapter write — at most COM_TX_MAX octets — and returns 0
 *     only when a previous chunk is still on the wire or the length is over one chunk. The
 *     caller's buffer is not retained: the driver copies into its own holding. Main-loop
 *     only.
 *   - Uart_TxPoll arms the next chunk from the debug-record queue and returns the octets
 *     armed, 0 when the queue is empty or a chunk is outstanding. Main-loop only.
 *   - Uart_Witnesses publishes the driver's bring-up read-backs, including its vector count
 *     and its overrun and framing-error counters, which the watchdog compares against what
 *     the adapter counted (docs/testing.md §2).
 *   - A driver whose board changes the line rate at run time (DS_LINK_BITRATE) adds two:
 *     Uart_SetBaud moves the link once the line is idle (1), refuses while a chunk is still
 *     on the wire (0) or for a rate it cannot program (-1), and Uart_RxFrameErrors reports
 *     its framing-error count. Main-loop only.
 *
 * Everything a silicon keeps to itself — its vector body, its pad routing, its buffer
 * sizes — stays out of this header, in the driver's own <silicon>_uart.h.
 */

#ifndef HAL_UART_DRV_H
#define HAL_UART_DRV_H

#include <stdint.h>

/* A received run: octets and length, valid for the duration of the call. */
typedef void (*Uart_DataInFn)(const uint8_t *bytes, uint16_t len);

int Uart_Init(uint32_t baud);
int Uart_Present(void);
void Uart_SetRxHandler(Uart_DataInFn handler);
int Uart_TxSend(const uint8_t *bytes, uint16_t len);
uint16_t Uart_TxPoll(void);
void Uart_Witnesses(void);
int Uart_SetBaud(uint32_t baud);
uint16_t Uart_RxFrameErrors(void);

#endif /* HAL_UART_DRV_H */
