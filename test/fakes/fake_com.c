/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* fake_com.c — the host link of hal/com.h for a host suite: every accepted write is appended to one
 * buffer the suite decodes (fakes.h). */
#include "fakes.h"

#include "com.h"

uint8_t fakeLinkOut[FAKE_LINK_OUT_MAX];
uint32_t fakeLinkOutLen;
int fakeLinkRefuse;
uint32_t fakeLinkBitrate;
uint32_t fakeLinkSwitchAt;
int fakeLinkBusy;
int fakeLinkRateRefused;
uint16_t fakeLinkFrameErrors;

/* The capture only: the link's rate and its counters are the board's, and a suite that clears the
 * capture mid-run keeps them. */
void Fake_LinkReset(void)
{
    fakeLinkOutLen = 0u;
    fakeLinkRefuse = 0;
}

int COM_SetBitrate(uint32_t bps)
{
    if (fakeLinkRateRefused)
        return -1;

    if (fakeLinkBusy)
        return 0;

    fakeLinkBitrate = bps;
    fakeLinkSwitchAt = fakeLinkOutLen;
    return 1;
}

uint16_t COM_RxFrameErrors(void)
{
    return fakeLinkFrameErrors;
}

int COM_TxSend(const uint8_t *buf, uint16_t len)
{
    uint16_t i;

    if (fakeLinkRefuse || buf == 0 || len == 0u || len > COM_TX_MAX)
        return 0;

    for (i = 0u; i < len && fakeLinkOutLen < FAKE_LINK_OUT_MAX; ++i)
        fakeLinkOut[fakeLinkOutLen++] = buf[i];

    return 1;
}
