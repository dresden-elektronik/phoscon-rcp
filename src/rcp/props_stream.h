/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* props_stream.h — the STREAM_RAW data path (docs/protocol.md §7). */
#ifndef RCP_PROPS_STREAM_H
#define RCP_PROPS_STREAM_H

#include <stdint.h>

#include "radio.h"
#include "u_bstream.h"

/* SPINEL_PROP_STREAM__BEGIN + 0 (spinel.h:3399,3415). */
#define PROPS_ID_STREAM_DEBUG 0x0070u
/* 8 record bytes as 16 hex digits plus a newline, one record per line. */
#define PROPS_STREAM_DEBUG_LINE 17u

/* SPINEL_PROP_STREAM__BEGIN (0x70) + 1 (openthread/src/lib/spinel/spinel.h:3399,3486). */
#define PROPS_ID_STREAM_RAW 0x0071u

/* MD_FLAG bits (docs/protocol.md §7.2, upstream PackRadioFrame): bit0 = sent rather than received,
 * bit2 = FCS failed, bit3 = duplicate, bit4 = acknowledged with Frame Pending set
 * (SPINEL_MD_FLAG_ACKED_FP, spinel.h:896). */
#define PROPS_STREAM_MD_TX 0x0001u
#define PROPS_STREAM_MD_BAD_FCS 0x0004u
#define PROPS_STREAM_MD_DUPE 0x0008u
#define PROPS_STREAM_MD_ACKED_FP 0x0010u
#define PROPS_STREAM_MD_ACKED_SEC 0x0020u

#define PROPS_STREAM_MD_NOISE_UNUSED (-128)

#define PROPS_STREAM_MD_PHY_LEN 10u /* channel + LQI + 8-byte µs timestamp */

/* docs/protocol.md §7.2's fixed bytes around the PSDU: frameDataLen (2) + RSSI/noise/
 * flags (4) + MD_PHY (2 + 10) + MD_VEND (2 + 1) + MD_MAC (2 + 5) = 28 bytes, so a full
 * RADIO_PSDU_MAX PSDU is 155 value bytes; 160 leaves headroom. This is the outbound
 * ceiling that session.h RCP_VALUE_MAX takes as the value path's size (docs/architecture.md §5). */
#define PROPS_STREAM_RX_MAX 160u

/* docs/protocol.md §7.1's trailer after the PSDU: channel, csma backoffs, retries, enableCSMACA,
 * isHeaderUpdated, isRetransmission, isSecurityProcessed, txDelay (4), txDelayBase (4),
 * rxChannelAfterTx = at most 16 bytes; only the channel is required. */
#define PROPS_STREAM_TX_TRAILER 16u
#define PROPS_STREAM_TX_RETRIES_DEFAULT 3u /* for a trailer that ends before maxRetries */

typedef struct
{
    uint8_t psdu[RADIO_PSDU_MAX];
    uint8_t len;             /* PSDU length, its two FCS bytes included */
    uint8_t channel;         /* 0 = keep the current channel */
    uint8_t maxCsmaBackoffs; /* 0 = the driver's default */
    uint8_t maxRetries;
    int csmaCa;               /* 0 = transmit immediately, ignore backoff */
    int headerUpdated;        /* the host already wrote the key index and frame counter */
    int securityProcessed;    /* the host already secured the frame */
    uint32_t txDelay;         /* µs, 0 = ASAP */
    uint32_t txDelayBase;     /* the low half of the radio clock txDelay counts from */
    uint8_t rxChannelAfterTx; /* 0 = keep */
} PropsStream_TxRequest;
#define PROPS_STREAM_MD_MAC_LEN 5u /* ACK key id + 4-byte frame counter */

uint16_t PropsStream_BuildRawRx(U_BStream *dst, const Radio_Frame *frame);

int PropsStream_ParseRawTx(const uint8_t *value, uint16_t len, PropsStream_TxRequest *out);

uint16_t PropsStream_BuildRawTx(U_BStream *dst, const Radio_Frame *frame);

#endif
