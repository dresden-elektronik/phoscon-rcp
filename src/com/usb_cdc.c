/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* ConBee II host link: the COM_* contract (hal/com.h) over the adopted USB
 * device API (usb/usbc.h). Pure forwarding - no state and no logic; the USB
 * device owns the pipes, the record queue and the flasher-reset trap
 * (docs/architecture.md §2). */
#include "com.h"
#include "usb_cdc.h"
#include "usbc.h"

int COM_Init(void)
{
    Usbc_Init();
    return 1; /* the pipes exist; whether the host is on them is COM_Present's answer */
}

int COM_Present(void)
{
    return Usbc_Configured();
}

void COM_Attach(int on)
{
    Usbc_Attach(on);
}

void COM_SetRxHandler(void (*fn)(const uint8_t *, uint16_t))
{
    Usbc_SetDataOutHandler(fn);
}

int COM_TxSend(const uint8_t *buf, uint16_t len)
{
    return Usbc_CdcInSend(buf, len);
}

uint16_t COM_TxPoll(void)
{
    return Usbc_CdcInPoll();
}

void COM_Witnesses(void)
{
    Usbc_Witnesses();
}

uint32_t COM_SofFrames(void)
{
    return Usbc_SofFrames();
}

uint32_t COM_Frame(void)
{
    return Usbc_Frame();
}

uint16_t COM_NotifyPoll(void)
{
    return Usbc_CdcNotifyPoll();
}
