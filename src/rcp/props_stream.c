/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* props_stream.c — the STREAM_RAW RX notification (src/rcp/props_stream.h). */
#include "props_stream.h"

static void putU16(U_BStream *bs, uint16_t v)
{
    U_bstream_put_u8(bs, (uint8_t)(v & 0xFFu));
    U_bstream_put_u8(bs, (uint8_t)(v >> 8));
}

static void putU32(U_BStream *bs, uint32_t v)
{
    U_bstream_put_u8(bs, (uint8_t)(v & 0xFFu));
    U_bstream_put_u8(bs, (uint8_t)((v >> 8) & 0xFFu));
    U_bstream_put_u8(bs, (uint8_t)((v >> 16) & 0xFFu));
    U_bstream_put_u8(bs, (uint8_t)((v >> 24) & 0xFFu));
}

int PropsStream_ParseRawTx(const uint8_t *value, uint16_t len, PropsStream_TxRequest *out)
{
    uint16_t psduLen;
    uint16_t need;
    uint16_t rest;
    uint8_t channel;
    uint8_t i;
    uint16_t off;

    if (value == 0 || out == 0 || len < 2u)
        return 0;

    psduLen = (uint16_t)(value[0] | ((uint16_t)value[1] << 8));

    if (psduLen == 0u || psduLen > RADIO_PSDU_MAX)
        return 0;

    /* docs/protocol.md §7.1: the channel is the one trailer field a request must carry. */
    need = (uint16_t)(2u + psduLen + 1u);

    if (need > len)
        return 0;

    channel = value[2u + psduLen];

    if (channel != 0u && (channel < RADIO_CHAN_MIN || channel > RADIO_CHAN_MAX))
        return 0;

    for (i = 0u; i < (uint8_t)psduLen; ++i)
        out->psdu[i] = value[2u + i];

    out->len = (uint8_t)psduLen;
    out->channel = channel;
    off = (uint16_t)(2u + psduLen);
    rest = (uint16_t)(len - off);

    /* A host may end the trailer after any whole field; the rest take upstream's defaults. */
    out->maxCsmaBackoffs = (rest > 1u) ? value[off + 1u] : 0u;
    out->maxRetries = (rest > 2u) ? value[off + 2u] : PROPS_STREAM_TX_RETRIES_DEFAULT;
    out->csmaCa = (rest > 3u) ? ((value[off + 3u] != 0u) ? 1 : 0) : 1;
    out->headerUpdated = (rest > 4u && value[off + 4u] != 0u) ? 1 : 0;
    out->securityProcessed = (rest > 6u && value[off + 6u] != 0u) ? 1 : 0;
    out->txDelay = 0u;
    out->txDelayBase = 0u;
    out->rxChannelAfterTx = 0u;

    if (rest > 10u)
        out->txDelay = (uint32_t)value[off + 7u] | ((uint32_t)value[off + 8u] << 8) |
                       ((uint32_t)value[off + 9u] << 16) | ((uint32_t)value[off + 10u] << 24);

    if (rest > 14u)
        out->txDelayBase = (uint32_t)value[off + 11u] | ((uint32_t)value[off + 12u] << 8) |
                           ((uint32_t)value[off + 13u] << 16) | ((uint32_t)value[off + 14u] << 24);

    if (rest > 15u)
        out->rxChannelAfterTx = value[off + 15u];

    return 1;
}

static uint16_t propsStreamBuild(U_BStream *dst, const Radio_Frame *frame, uint16_t extraFlags)
{
    uint16_t flags;

    if (dst == 0 || frame == 0 || frame->len > RADIO_PSDU_MAX)
        return 0u;

    if ((uint32_t)frame->len + 32u > (uint32_t)U_bstream_remaining(dst))
        return 0u;

    flags = extraFlags;

    if (!frame->crcOk)
        flags |= PROPS_STREAM_MD_BAD_FCS;

    putU16(dst, (uint16_t)frame->len); /* frameDataLen */

    if (frame->len > 0u)
        U_bstream_put_bytes(dst, frame->psdu, frame->len);

    U_bstream_put_u8(dst, (uint8_t)frame->rssi);                  /* MD_POWER, dBm */
    U_bstream_put_u8(dst, (uint8_t)PROPS_STREAM_MD_NOISE_UNUSED); /* MD_NOISE */
    putU16(dst, flags);                                           /* MD_FLAG */

    putU16(dst, PROPS_STREAM_MD_PHY_LEN); /* MD_PHY */
    U_bstream_put_u8(dst, frame->channel);
    U_bstream_put_u8(dst, frame->lqi);
    putU32(dst, (uint32_t)(frame->timestamp & 0xFFFFFFFFuLL)); /* µs, little endian */
    putU32(dst, (uint32_t)(frame->timestamp >> 32));

    putU16(dst, 1u);           /* MD_VEND len: packed 0 = 1 byte */
    U_bstream_put_u8(dst, 0u); /* RX error code: 0 = OK */

    putU16(dst, PROPS_STREAM_MD_MAC_LEN); /* MD_MAC */
    U_bstream_put_u8(dst, frame->ackKeyIndex);
    putU32(dst, frame->ackFrameCounter);

    /* 2 + PSDU + 4 + (2 + MD_PHY) + (2 + 1) + (2 + MD_MAC) = len + 28. */
    return (uint16_t)(2u + frame->len + 4u + (2u + PROPS_STREAM_MD_PHY_LEN) + (2u + 1u) +
                      (2u + PROPS_STREAM_MD_MAC_LEN));
}

uint16_t PropsStream_BuildRawRx(U_BStream *dst, const Radio_Frame *frame)
{
    /* MD_FLAG bit4 tells the host what this firmware put into the Ack it composed for this
     * frame, which no host can observe otherwise (docs/protocol.md §7.2). */
    return propsStreamBuild(dst,
                            frame,
                            (uint16_t)((frame->ackedFp ? PROPS_STREAM_MD_ACKED_FP : 0u) |
                                       (frame->ackedSec ? PROPS_STREAM_MD_ACKED_SEC : 0u)));
}

uint16_t PropsStream_BuildRawTx(U_BStream *dst, const Radio_Frame *frame)
{
    /* docs/protocol.md §7.2: MD_FLAG bit0 marks a frame this RCP sent. */
    return propsStreamBuild(dst, frame, PROPS_STREAM_MD_TX);
}
