/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* The wired host link: the COM_* contract (hal/com.h) over the board's hal/uart_drv.h
 * driver, which the board names in its DS_LINK_SOURCES (docs/boards.md §2). Pure
 * forwarding — no state and no logic; the driver owns the pipe, the record drain and the
 * vector, and this file names no silicon so that any board can drop its own driver in.
 *
 * This link is soldered to the host: there is no attach edge to drive and no control
 * channel to notify on, so COM_Attach does nothing and COM_NotifyPoll returns 0 - the
 * same way COM_Attach is a no-op on a board without a D+ (hal/com.h). Its link
 * diagnostics live with the driver, not in a header of their own. */
#include "com.h"
#include "board.h"
#include "uart_drv.h"

int COM_Init(void)
{
    return Uart_Init(Board_Get()->linkBaud);
}

int COM_Present(void)
{
    return Uart_Present();
}

void COM_Attach(int on)
{
    (void)on;
}

void COM_SetRxHandler(void (*fn)(const uint8_t *, uint16_t))
{
    Uart_SetRxHandler(fn);
}

int COM_TxSend(const uint8_t *buf, uint16_t len)
{
    return Uart_TxSend(buf, len);
}

uint16_t COM_TxPoll(void)
{
    return Uart_TxPoll();
}

void COM_Witnesses(void)
{
    Uart_Witnesses();
}

uint16_t COM_NotifyPoll(void)
{
    return 0u;
}
