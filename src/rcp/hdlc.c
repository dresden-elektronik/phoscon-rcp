/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* hdlc.c — HDLC framing for the Spinel transport (src/rcp/hdlc.h). */
#include "hdlc.h"

/* CRC-16/X.25, REFLECTED: poly 0x8408, shift right, init 0xFFFF, complement on the
 * wire. The spec's prose says "no reflect"; its arithmetic and captures say
 * otherwise (docs/protocol.md §3). */
static uint16_t hdlcCrcByte(uint16_t crc, uint8_t byte)
{
    uint16_t i;

    crc ^= (uint16_t)byte;

    for (i = 0; i < 8u; ++i)
        crc = (crc & 0x0001u) ? (uint16_t)((crc >> 1) ^ 0x8408u) : (uint16_t)(crc >> 1);

    return crc;
}

uint16_t Hdlc_Crc(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFFu;

    while (len-- > 0u)
        crc = hdlcCrcByte(crc, *data++);

    return crc;
}

void Hdlc_RxInit(Hdlc_Rx *rx)
{
    if (rx == 0)
        return;

    rx->len = 0u;
    rx->crc = 0xFFFFu;
    rx->escaped = 0;
    rx->overflow = 0;
}

Hdlc_RxResult Hdlc_RxByte(Hdlc_Rx *rx, uint8_t byte)
{
    Hdlc_RxResult result = HDLC_RX_NONE;
    uint16_t n;

    if (rx == 0)
    {
        return HDLC_RX_ERROR;
    }

    if (byte == HDLC_FEND)
    {
        /* Consecutive FENDs are keep-alives, not errors (docs/protocol.md §3). */
        if (rx->overflow)
            result = HDLC_RX_ERROR;
        else if (rx->len >= HDLC_FCS_LEN)
        {
            if (rx->crc == HDLC_FCS_GOOD)
            {
                n = (uint16_t)(rx->len - HDLC_FCS_LEN);
                Hdlc_RxInit(rx);
                rx->len = n;
                return HDLC_RX_FRAME;
            }

            result = HDLC_RX_ERROR;
        }
        else if (rx->len != 0u)
            result = HDLC_RX_ERROR;

        Hdlc_RxInit(rx);
        return result;
    }

    if (byte == HDLC_FESC)
    {
        rx->escaped = 1;
        return HDLC_RX_NONE;
    }

    if (rx->escaped)
    {
        byte = (uint8_t)(byte ^ HDLC_ESC_XOR);
        rx->escaped = 0;
    }

    if (rx->overflow)
    {
        return HDLC_RX_NONE;
    }

    if (rx->len >= HDLC_FRAME_MAX)
    {
        rx->overflow = 1;
        return HDLC_RX_NONE;
    }

    rx->buf[rx->len] = byte;
    rx->len = (uint16_t)(rx->len + 1u);
    rx->crc = hdlcCrcByte(rx->crc, byte);
    return HDLC_RX_NONE;
}

static uint16_t hdlcPutEscaped(U_BStream *dst, uint8_t byte)
{
    /* The superset {0x11, 0x13, 0x7E, 0x7D}: escaping XON/XOFF keeps a UART's flow
     * control from eating a frame (docs/protocol.md §3). */
    if (byte == HDLC_FEND || byte == HDLC_FESC || byte == 0x11u || byte == 0x13u)
    {
        U_bstream_put_u8(dst, HDLC_FESC);
        U_bstream_put_u8(dst, (uint8_t)(byte ^ HDLC_ESC_XOR));
        return 2u;
    }

    U_bstream_put_u8(dst, byte);
    return 1u;
}

uint16_t Hdlc_Encode(const uint8_t *payload, uint16_t len, U_BStream *dst)
{
    uint8_t fcs[HDLC_FCS_LEN];
    uint16_t crc;
    uint16_t written = 0u;
    uint16_t i;

    if (dst == 0 || (payload == 0 && len != 0u))
        return 0u;

    /* Worst case: two FENDs, every body byte escaped and both FCS bytes escaped. */
    if (((uint32_t)len * 2u) + 6u > (uint32_t)U_bstream_remaining(dst))
        return 0u;

    crc = (uint16_t)(Hdlc_Crc(payload, len) ^ 0xFFFFu); /* wire FCS, LSB first */
    fcs[0] = (uint8_t)(crc & 0xFFu);
    fcs[1] = (uint8_t)(crc >> 8);

    U_bstream_put_u8(dst, HDLC_FEND);
    written = 1u;

    for (i = 0; i < len; ++i)
        written = (uint16_t)(written + hdlcPutEscaped(dst, payload[i]));

    written = (uint16_t)(written + hdlcPutEscaped(dst, fcs[0]));
    written = (uint16_t)(written + hdlcPutEscaped(dst, fcs[1]));
    U_bstream_put_u8(dst, HDLC_FEND);
    return (uint16_t)(written + 1u);
}
