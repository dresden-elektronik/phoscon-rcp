/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* fake_com.c — the host link of hal/com.h for a host suite: every accepted write is appended to one
 * buffer the suite decodes (fakes.h). */
#include "fakes.h"

#include "com.h"

uint8_t fakeLinkOut[FAKE_LINK_OUT_MAX];
uint32_t fakeLinkOutLen;
int fakeLinkRefuse;

void Fake_LinkReset(void)
{
    fakeLinkOutLen = 0u;
    fakeLinkRefuse = 0;
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
