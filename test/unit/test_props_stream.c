/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* test_props_stream.c — the STREAM_RAW RX layout's suite (docs/testing.md §2): every byte
 * of the notification is checked against docs/protocol.md §7.2's table, CRC flag included. */
#include "props_stream.h"
#include "tu.h"
#include "u_bstream.h"

#define BUF 200

static uint8_t buf[BUF];

static Radio_Frame frameWith(uint8_t len, int crcOk)
{
    Radio_Frame f;
    uint8_t i;

    for (i = 0; i < RADIO_PSDU_MAX; ++i)
        f.psdu[i] = (uint8_t)(0x40u + (i & 0x0Fu));

    f.len = len;
    f.rssi = -73;
    f.lqi = 200u;
    f.channel = 15u;
    f.timestamp = 0x0102030412345678uLL;
    f.crcOk = crcOk;
    f.ackedFp = 0;
    f.ackedSec = 0;
    f.ackKeyIndex = 0u;
    f.ackFrameCounter = 0u;
    return f;
}

static uint16_t build(const Radio_Frame *f)
{
    U_BStream bs;

    U_bstream_init(&bs, buf, sizeof(buf));
    return PropsStream_BuildRawRx(&bs, f);
}

TU_TEST(rx_notification_matches_the_spec_layout)
{
    Radio_Frame f = frameWith(5u, 1);
    uint16_t n = build(&f);

    /* 2 + len + 3 + (2 + 10) + (2 + 1) + (2 + 5) = len + 28 */
    TU_EQ(n, 33u);
    TU_EQ(buf[0], 5u); /* S frameDataLen */
    TU_EQ(buf[1], 0u);
    TU_EQ(buf[2], 0x40u); /* PSDU[0] */
    TU_EQ(buf[3], 0x41u);
    TU_EQ(buf[4], 0x42u);
    TU_EQ(buf[5], 0x43u);
    TU_EQ(buf[6], 0x44u);
    TU_EQ((int8_t)buf[7], -73);  /* c MD_POWER = RSSI dBm */
    TU_EQ((int8_t)buf[8], -128); /* c MD_NOISE = unused */
    TU_EQ(buf[9], 0u);           /* S MD_FLAG: received, FCS good */
    TU_EQ(buf[10], 0u);
    TU_EQ(buf[11], 10u); /* S MD_PHY len = 10 */
    TU_EQ(buf[12], 0u);
    TU_EQ(buf[13], 15u);   /* C channel */
    TU_EQ(buf[14], 200u);  /* C LQI */
    TU_EQ(buf[15], 0x78u); /* L64 timestamp, little endian */
    TU_EQ(buf[16], 0x56u);
    TU_EQ(buf[17], 0x34u);
    TU_EQ(buf[18], 0x12u);
    TU_EQ(buf[19], 0x04u); /* the high half: a board's radio clock passes 32 bits after 71 minutes */
    TU_EQ(buf[20], 0x03u);
    TU_EQ(buf[21], 0x02u);
    TU_EQ(buf[22], 0x01u);
    TU_EQ(buf[23], 1u); /* S MD_VEND len = 1 (packed 0 = 1 byte) */
    TU_EQ(buf[24], 0u);
    TU_EQ(buf[25], 0u); /* packed RX error code = 0 */
    TU_EQ(buf[26], 5u); /* S MD_MAC len = 5 */
    TU_EQ(buf[27], 0u);
    TU_EQ(buf[28], 0u); /* C ACK key id */
    TU_EQ(buf[29], 0u); /* L ACK frame counter */
    TU_EQ(buf[30], 0u);
    TU_EQ(buf[31], 0u);
    TU_EQ(buf[32], 0u);
}

TU_TEST(crc_invalid_frames_are_delivered_and_flagged)
{
    /* The sniffer keeps CRC-invalid frames; MD_FLAG's BAD_FCS bit is the only bad signal
   * (docs/protocol.md §7.2). */
    Radio_Frame f = frameWith(5u, 0);
    uint16_t n = build(&f);

    TU_EQ(n, 33u);
    TU_EQ(buf[9] & PROPS_STREAM_MD_BAD_FCS, PROPS_STREAM_MD_BAD_FCS);
    TU_EQ(buf[0], 5u); /* the frame itself is still there */
}

TU_TEST(a_zero_length_frame_still_carries_its_metadata)
{
    /* docs/protocol.md §7.2: frameDataLen is 0 for an RX error, and the metadata follows regardless. */
    Radio_Frame f = frameWith(0u, 0);
    uint16_t n = build(&f);

    TU_EQ(n, 28u);
    TU_EQ(buf[0], 0u);
    TU_EQ(buf[1], 0u);
    TU_EQ((int8_t)buf[2], -73); /* metadata starts right after the length */
    TU_EQ(buf[3], (uint8_t)-128);
}

TU_TEST(a_maximum_frame_fits)
{
    Radio_Frame f = frameWith(RADIO_PSDU_MAX, 1);
    uint16_t n = build(&f);

    TU_EQ(n, (uint16_t)(RADIO_PSDU_MAX + 28u));
    TU_CHECK(n < BUF);
    TU_EQ(buf[0], RADIO_PSDU_MAX);
    /* The byte after the PSDU is MD_POWER (the PSDU starts at offset 2). */
    TU_EQ(buf[2u + RADIO_PSDU_MAX], (uint8_t)-73);
}

TU_TEST(a_transmitted_frame_is_marked_in_md_flag)
{
    /* MD_FLAG bit0 separates "we sent this" from "we heard this" (docs/protocol.md §7.2). */
    Radio_Frame f = frameWith(5u, 1);
    U_BStream bs;
    uint16_t n;

    U_bstream_init(&bs, buf, sizeof(buf));
    n = PropsStream_BuildRawTx(&bs, &f);
    TU_EQ(n, 33u);
    TU_EQ(buf[9] & PROPS_STREAM_MD_TX, PROPS_STREAM_MD_TX);
    TU_EQ(buf[9] & PROPS_STREAM_MD_BAD_FCS, 0u);
}

/* docs/protocol.md §7.1: the TX request parser - a half-armed frame is worse than one
 * that is refused. */

static uint16_t txPayload(uint8_t *dst, uint8_t psduLen, uint8_t channel)
{
    uint16_t off = (uint16_t)(2u + psduLen);
    uint16_t i;

    dst[0] = psduLen;
    dst[1] = 0u;

    for (i = 0u; i < psduLen; ++i)
        dst[2u + i] = (uint8_t)(0xA0u + i);

    for (i = 0u; i < PROPS_STREAM_TX_TRAILER; ++i)
        dst[off + i] = 0u;

    dst[off] = channel;
    return (uint16_t)(off + PROPS_STREAM_TX_TRAILER);
}

TU_TEST(tx_request_parses_field_by_field)
{
    uint8_t payload[64];
    PropsStream_TxRequest req;
    uint16_t n = txPayload(payload, 3u, 20u);

    payload[2u + 3u + 1u] = 4u;    /* maxCsmaBackoffs */
    payload[2u + 3u + 2u] = 7u;    /* maxRetries */
    payload[2u + 3u + 3u] = 1u;    /* enableCSMACA */
    payload[2u + 3u + 7u] = 0x10u; /* txDelay = 0x10 µs */
    payload[2u + 3u + 15u] = 21u;  /* rxChannelAfterTx */

    TU_EQ(PropsStream_ParseRawTx(payload, n, &req), 1);
    TU_EQ(req.len, 3u);
    TU_EQ(req.psdu[0], 0xA0u);
    TU_EQ(req.psdu[2], 0xA2u);
    TU_EQ(req.channel, 20u);
    TU_EQ(req.maxCsmaBackoffs, 4u);
    TU_EQ(req.maxRetries, 7u);
    TU_EQ(req.csmaCa, 1);
    TU_EQ(req.txDelay, 0x10u);
    TU_EQ(req.txDelayBase, 0u);
    TU_EQ(req.rxChannelAfterTx, 21u);
}

TU_TEST(a_tx_request_may_end_after_any_field_past_the_channel)
{
    uint8_t payload[64];
    PropsStream_TxRequest req;
    uint16_t i;
    uint16_t n = txPayload(payload, 3u, 20u);
    const uint16_t off = 2u + 3u;

    for (i = 1u; i < PROPS_STREAM_TX_TRAILER; ++i)
        payload[off + i] = 1u;

    /* The channel alone: every other field takes its default. */
    TU_EQ(PropsStream_ParseRawTx(payload, (uint16_t)(off + 1u), &req), 1);
    TU_EQ(req.channel, 20u);
    TU_EQ(req.maxCsmaBackoffs, 0u);
    TU_EQ(req.maxRetries, PROPS_STREAM_TX_RETRIES_DEFAULT);
    TU_EQ(req.csmaCa, 1);
    TU_EQ(req.headerUpdated, 0);
    TU_EQ(req.securityProcessed, 0);

    /* Through isSecurityProcessed, as zigpy-ziggurat sends it. */
    payload[off + 2u] = 2u;
    payload[off + 3u] = 0u;
    TU_EQ(PropsStream_ParseRawTx(payload, (uint16_t)(off + 7u), &req), 1);
    TU_EQ(req.maxCsmaBackoffs, 1u);
    TU_EQ(req.maxRetries, 2u);
    TU_EQ(req.csmaCa, 0);
    TU_EQ(req.headerUpdated, 1);
    TU_EQ(req.securityProcessed, 1);
    TU_EQ(req.txDelay, 0u);
    TU_EQ(req.txDelayBase, 0u);
    TU_EQ(req.rxChannelAfterTx, 0u);

    /* A multi-byte field cut short counts as absent. */
    TU_EQ(PropsStream_ParseRawTx(payload, (uint16_t)(off + 10u), &req), 1);
    TU_EQ(req.txDelay, 0u);
    TU_EQ(PropsStream_ParseRawTx(payload, (uint16_t)(off + 11u), &req), 1);
    TU_EQ(req.txDelay, 0x01010101u);
    TU_EQ(req.txDelayBase, 0u);

    /* The last byte missing, and what a host may send beyond the trailer ignored. */
    TU_EQ(PropsStream_ParseRawTx(payload, (uint16_t)(n - 1u), &req), 1);
    TU_EQ(req.txDelayBase, 0x01010101u);
    TU_EQ(req.rxChannelAfterTx, 0u);
    payload[n] = 0x7Fu;
    TU_EQ(PropsStream_ParseRawTx(payload, (uint16_t)(n + 1u), &req), 1);
    TU_EQ(req.rxChannelAfterTx, 1u);
}

TU_TEST(malformed_tx_requests_are_refused_whole)
{
    uint8_t payload[64];
    PropsStream_TxRequest req;
    uint16_t n = txPayload(payload, 3u, 20u);

    TU_EQ(PropsStream_ParseRawTx(payload, 5u, &req), 0); /* ends before the channel */
    TU_EQ(PropsStream_ParseRawTx(payload, 1u, &req), 0); /* no complete length */
    TU_EQ(PropsStream_ParseRawTx(0, n, &req), 0);        /* no payload */

    /* A length field that promises more PSDU than the payload holds. */
    payload[0] = 60u;
    TU_EQ(PropsStream_ParseRawTx(payload, n, &req), 0);
    payload[0] = 3u;

    /* Zero-length PSDU is not a frame. */
    payload[0] = 0u;
    TU_EQ(PropsStream_ParseRawTx(payload, (uint16_t)(2u + PROPS_STREAM_TX_TRAILER), &req), 0);
    payload[0] = 3u;

    /* A channel outside 11..26 ... */
    payload[2u + 3u] = 27u;
    TU_EQ(PropsStream_ParseRawTx(payload, n, &req), 0);
    /* ... 0 means "keep the current channel", which is legal. */
    payload[2u + 3u] = 0u;
    TU_EQ(PropsStream_ParseRawTx(payload, n, &req), 1);
    /* A PSDU beyond what the radio can send. */
    payload[0] = 200u;
    payload[1] = 0u;
    TU_EQ(PropsStream_ParseRawTx(payload, n, &req), 0);
}

TU_TEST(a_notification_that_does_not_fit_is_refused_whole)
{
    Radio_Frame f = frameWith(RADIO_PSDU_MAX, 1);
    uint8_t small[32];
    U_BStream bs;

    U_bstream_init(&bs, small, sizeof(small));
    TU_EQ(PropsStream_BuildRawRx(&bs, &f), 0u);
    /* Nothing partial: the capacity check happens before the first byte is written. */
    TU_EQ(U_bstream_remaining(&bs), sizeof(small));
}

TU_TEST(an_acknowledged_with_pending_frame_is_marked_in_md_flag)
{
    /* MD_FLAG bit4 is the host's only view of the Ack this firmware composed
     * (SPINEL_MD_FLAG_ACKED_FP, docs/protocol.md §7.2). It must not disturb the other bits. */
    Radio_Frame f = frameWith(5u, 1);

    f.ackedFp = 1;
    TU_EQ(build(&f), 33u);
    TU_EQ(buf[9], 0x10u);
    TU_EQ(buf[10], 0u);

    /* With a bad FCS the two bits coexist rather than replacing one another. */
    f.crcOk = 0;
    TU_EQ(build(&f), 33u);
    TU_EQ(buf[9], 0x14u); /* BAD_FCS | ACKED_FP */

    /* And a plain good frame carries neither. */
    f.crcOk = 1;
    f.ackedFp = 0;
    TU_EQ(build(&f), 33u);
    TU_EQ(buf[9], 0u);

    /* A transmitted frame carries MD_TX and never ACKED_FP: the Ack of a frame this RCP sent
     * is the peer's, and its Frame Pending bit travels in the TX completion's LAST_STATUS
     * (docs/protocol.md §7.1), not in a notification. */
    f.ackedFp = 1;
    {
        U_BStream bs;

        U_bstream_init(&bs, buf, sizeof(buf));
        TU_EQ(PropsStream_BuildRawTx(&bs, &f), 33u);
        TU_EQ(buf[9], 0x01u); /* MD_TX only */
    }
}

int main(void)
{
    static const Tu_Test tests[] = {
        {"rx_notification_matches_the_spec_layout", rx_notification_matches_the_spec_layout},
        {"crc_invalid_frames_are_delivered_and_flagged",
         crc_invalid_frames_are_delivered_and_flagged},
        {"a_zero_length_frame_still_carries_its_metadata",
         a_zero_length_frame_still_carries_its_metadata},
        {"a_maximum_frame_fits", a_maximum_frame_fits},
        {"a_transmitted_frame_is_marked_in_md_flag", a_transmitted_frame_is_marked_in_md_flag},
        {"tx_request_parses_field_by_field", tx_request_parses_field_by_field},
        {"a_tx_request_may_end_after_any_field_past_the_channel", a_tx_request_may_end_after_any_field_past_the_channel},
        {"malformed_tx_requests_are_refused_whole", malformed_tx_requests_are_refused_whole},
        {"an_acknowledged_with_pending_frame_is_marked_in_md_flag",
         an_acknowledged_with_pending_frame_is_marked_in_md_flag},
        {"a_notification_that_does_not_fit_is_refused_whole",
         a_notification_that_does_not_fit_is_refused_whole}};

    Tu_Run("props_stream", tests, (int)(sizeof(tests) / sizeof(tests[0])));
    return 0; /* Tu_Run never returns */
}
