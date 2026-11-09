/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* main_rcp.c - the RCP image's entry hooks (src/app/app_main.h). */
#include "app_main.h"

#include "rcp_app.h"

void App_Boot(void)
{
    RcpApp_Init();
}

void App_DataOut(const uint8_t *buf, uint16_t len)
{
    /* The flasher's reset frame carries no 0x7E, so the HDLC decoder discards it as text. */
    RcpApp_PushFromIrq(buf, len);
}

void App_Poll(uint32_t ms)
{
    (void)ms;
    RcpApp_Poll();
}

int App_Healthy(uint32_t ms)
{
    return RcpApp_Healthy(ms);
}

int App_PollAgain(void)
{
    return RcpApp_PollAgain();
}
