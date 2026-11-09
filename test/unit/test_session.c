/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* test_session.c — the RCP session's suite (docs/testing.md §2):
 * dialogs captured from a Silabs EFR32 RCP replayed host frame in,
 * RCP frame out, answers compared byte for byte. */
#include <stdio.h>

#include "hdlc.h"
#include "session.h"
#include "tu.h"
#include "u_bstream.h"

#define OUT 128

static Rcp_Session session;

static void freshSession(void)
{
    Props_Identity id;

    id.ncpVersion = "DSPINEL/1.0; AT86RF233";
    id.rxSensitivityDbm = -96;
    id.txPowerMaxDbm = 127;
    id.txPowerMaxCh26Dbm = 127;
    Rcp_Init(&session, &id);
}

static uint16_t feed(const uint8_t *frame, uint16_t len, uint8_t *reply)
{
    U_BStream bs;

    U_bstream_init(&bs, reply, OUT);
    return Rcp_Feed(&session, frame, len, &bs);
}

static int same(const uint8_t *a, const uint8_t *b, uint16_t len)
{
    uint16_t i;

    for (i = 0; i < len; ++i)
    {
        if (a[i] != b[i])
            return 0;
    }

    return 1;
}

TU_TEST(replays_the_captured_bringup_dialog)
{
    uint8_t reply[OUT];
    uint16_t n;

    freshSession();
    {
        /* Host: GET PROTOCOL_VERSION, TID 1 (81 02 01). Captured answer: 81 06 01 04 03. */
        static const uint8_t get[3] = {0x81u, 0x02u, 0x01u};
        static const uint8_t want[5] = {0x81u, 0x06u, 0x01u, 0x04u, 0x03u};
        n = feed(get, 3u, reply);
        TU_EQ(n, 5u);
        TU_CHECK(same(reply, want, 5u));
    }
    {
        /* Host: GET INTERFACE_TYPE, TID 3 (83 02 03). Captured answer: 83 06 03 03. */
        static const uint8_t get[3] = {0x83u, 0x02u, 0x03u};
        static const uint8_t want[4] = {0x83u, 0x06u, 0x03u, 0x03u};
        n = feed(get, 3u, reply);
        TU_EQ(n, 4u);
        TU_CHECK(same(reply, want, 4u));
    }
    {
        /* Host GET RCP_API_VERSION (84 02 b0 01): pins the two-byte packed-id framing both
   * ways; our value differs from the capture's 10. */
        static const uint8_t get[4] = {0x84u, 0x02u, 0xB0u, 0x01u};
        n = feed(get, 4u, reply);
        TU_EQ(n, 5u);
        TU_EQ(reply[0], 0x84u);
        TU_EQ(reply[1], 0x06u);
        TU_EQ(reply[2], 0xB0u); /* the property id rounds back as two bytes */
        TU_EQ(reply[3], 0x01u);
        TU_NOTE("our RCP_API_VERSION", reply[4]);
    }
    {
        /* Host GET RCP_MIN_HOST_API_VERSION (85 02 b1 01); captured answer 85 06 b1 01 04 -
   * ours is 4 too, so this one matches exactly. */
        static const uint8_t get[4] = {0x85u, 0x02u, 0xB1u, 0x01u};
        static const uint8_t want[5] = {0x85u, 0x06u, 0xB1u, 0x01u, 0x04u};
        n = feed(get, 4u, reply);
        TU_EQ(n, 5u);
        TU_CHECK(same(reply, want, 5u));
    }
    {
        /* Host GET NCP_VERSION (82 02 02): ours names this firmware, so the test pins the 'U'
   * shape - zero-terminated and framed like the capture. */
        static const uint8_t get[3] = {0x82u, 0x02u, 0x02u};
        n = feed(get, 3u, reply);
        TU_CHECK(n > 6u);
        TU_EQ(reply[0], 0x82u);
        TU_EQ(reply[1], 0x06u);
        TU_EQ(reply[2], 0x02u);
        TU_EQ(reply[n - 1u], 0x00u); /* the terminator is inside the frame */
        TU_EQ(reply[3], (uint8_t)'D');
    }
}

TU_TEST(replays_the_captured_form_network_set_sequence)
{
    uint8_t reply[OUT];
    uint16_t n;

    freshSession();
    /* Each row: host SET (cmd 3), captured RCP answer (cmd 6); echoing the same bytes is
   * wire-compatible. */
    {
        static const uint8_t set[4] = {0x87u, 0x03u, 0x20u, 0x01u};
        static const uint8_t want[4] = {0x87u, 0x06u, 0x20u, 0x01u}; /* PHY_ENABLED */
        n = feed(set, 4u, reply);
        TU_EQ(n, 4u);
        TU_CHECK(same(reply, want, 4u));
    }
    {
        static const uint8_t set[4] = {0x88u, 0x03u, 0x21u, 0x14u};
        static const uint8_t want[4] = {0x88u, 0x06u, 0x21u, 0x14u}; /* PHY_CHAN 20 */
        n = feed(set, 4u, reply);
        TU_EQ(n, 4u);
        TU_CHECK(same(reply, want, 4u));
    }
    {
        static const uint8_t set[4] = {0x89u, 0x03u, 0x25u, 0x13u};
        static const uint8_t want[4] = {0x89u, 0x06u, 0x25u, 0x13u}; /* TX_POWER 19 */
        n = feed(set, 4u, reply);
        TU_EQ(n, 4u);
        TU_CHECK(same(reply, want, 4u));
    }
    {
        static const uint8_t set[5] = {0x8Bu, 0x03u, 0x35u, 0x00u, 0x00u};
        static const uint8_t want[5] = {0x8Bu, 0x06u, 0x35u, 0x00u, 0x00u}; /* SADDR 0 */
        n = feed(set, 5u, reply);
        TU_EQ(n, 5u);
        TU_CHECK(same(reply, want, 5u));
    }
    {
        static const uint8_t set[5] = {0x8Cu, 0x03u, 0x36u, 0xD9u, 0x85u};
        static const uint8_t want[5] = {0x8Cu, 0x06u, 0x36u, 0xD9u, 0x85u}; /* PANID */
        n = feed(set, 5u, reply);
        TU_EQ(n, 5u);
        TU_CHECK(same(reply, want, 5u));
    }
    {
        static const uint8_t set[4] = {0x8Eu, 0x03u, 0x37u, 0x01u};
        static const uint8_t want[4] = {0x8Eu, 0x06u, 0x37u, 0x01u}; /* RAW_STREAM */
        n = feed(set, 4u, reply);
        TU_EQ(n, 4u);
        TU_CHECK(same(reply, want, 4u));
    }
    /* The state a captured "form network" sequence needs. */
    TU_EQ(session.props.channel, 20u);
    TU_EQ((int8_t)session.props.txPowerDbm, 19);
    TU_EQ(session.props.shortAddr, 0u);
    TU_EQ(session.props.panId, 0x85D9u);
    TU_EQ(session.props.rawStreamEnabled, 1u);
    TU_EQ(session.props.phyEnabled, 1u);
}

TU_TEST(unknown_command_is_invalid_command_with_the_same_tid)
{
    static const uint8_t frame[4] = {0x8Fu, 0x12u, 0x00u, 0x00u}; /* PEEK (18), TID 15 */
    static const uint8_t want[4] = {0x8Fu, 0x06u, 0x00u, 0x05u};
    uint8_t reply[OUT];
    uint16_t n;

    freshSession();
    n = feed(frame, 4u, reply);
    TU_EQ(n, 4u);
    TU_CHECK(same(reply, want, 4u));
    TU_EQ(session.unknownCmd, 1u);
}

TU_TEST(unknown_property_is_prop_not_found_with_the_same_tid)
{
    static const uint8_t get[3] = {0x82u, 0x02u, 0x7Fu};         /* GET 0x7F, TID 2 */
    static const uint8_t want[4] = {0x82u, 0x06u, 0x00u, 0x0Du}; /* PROP_NOT_FOUND */
    uint8_t reply[OUT];
    uint16_t n;

    freshSession();
    n = feed(get, 3u, reply);
    TU_EQ(n, 4u);
    TU_CHECK(same(reply, want, 4u));
    TU_EQ(session.unknownProp, 1u);

    /* A SET of it is the same status, and still answered. */
    {
        static const uint8_t set[4] = {0x82u, 0x03u, 0x7Fu, 0x01u};
        n = feed(set, 4u, reply);
        TU_EQ(n, 4u);
        TU_CHECK(same(reply, want, 4u));
        TU_EQ(session.unknownProp, 2u);
    }
}

TU_TEST(refused_set_is_invalid_arg_and_the_value_is_unchanged)
{
    static const uint8_t bad[4] = {0x88u, 0x03u, 0x21u, 0x09u};  /* channel 9 */
    static const uint8_t want[4] = {0x88u, 0x06u, 0x00u, 0x03u}; /* INVALID_ARG */
    uint8_t reply[OUT];
    uint16_t n;

    freshSession();
    n = feed(bad, 4u, reply);
    TU_EQ(n, 4u);
    TU_CHECK(same(reply, want, 4u));
    TU_EQ(session.props.channel, 0u); /* refused, so not stored */
    TU_EQ(session.rejected, 1u);
}

TU_TEST(reset_is_uncorrelated_and_reinitialises)
{
    static const uint8_t reset[3] = {0x80u, 0x01u, 0x02u};
    static const uint8_t want[4] = {0x80u, 0x06u, 0x00u, 0x72u}; /* 114, TID 0 */
    uint8_t reply[OUT];
    uint16_t n;

    freshSession();
    /* Move the state away from power-on first, so the reset has something to undo. */
    {
        static const uint8_t set[4] = {0x88u, 0x03u, 0x21u, 0x14u};
        (void)feed(set, 4u, reply);
    }
    TU_EQ(session.props.channel, 20u);

    n = feed(reset, 3u, reply);
    TU_EQ(n, 4u);
    TU_CHECK(same(reply, want, 4u));
    TU_EQ(session.props.channel, 0u); /* docs/protocol.md §6 */
    TU_EQ(session.props.phyEnabled, 0u);
    /* the identity survives the reset */
    TU_CHECK(session.props.ncpVersion != 0);
    TU_EQ(session.props.ncpVersion[0], (uint8_t)'D');
}

TU_TEST(malformed_frames_are_counted_and_unanswered)
{
    static const uint8_t notSpinel[3] = {0x00u, 0x02u, 0x01u};
    static const uint8_t truncated[2] = {0x81u, 0x02u};
    static const uint8_t noop[2] = {0x8Fu, 0x00u};
    uint8_t reply[OUT];

    freshSession();
    TU_EQ(feed(notSpinel, 3u, reply), 0u);
    TU_EQ(feed(truncated, 2u, reply), 0u);
    TU_EQ(session.malformed, 2u);
    TU_EQ(session.dropped, 0u); /* unanswered by design, not a drop */

    /* A NOOP is a no-op: no reply, and not an error either. */
    TU_EQ(feed(noop, 2u, reply), 0u);
    TU_EQ(session.unknownCmd, 0u);
    TU_EQ(session.dropped, 0u);
}

TU_TEST(a_reply_that_does_not_fit_is_counted_not_truncated)
{
    static const uint8_t get[3] = {0x81u, 0x02u, 0x02u}; /* NCP_VERSION, long */
    uint8_t tiny[2];
    U_BStream bs;

    freshSession();
    /* Two bytes cannot hold even the four-byte NOMEM fallback: say nothing, count it. */
    U_bstream_init(&bs, tiny, sizeof(tiny));
    TU_EQ(Rcp_Feed(&session, get, 3u, &bs), 0u);
    TU_CHECK(session.dropped >= 1u);

    /* With room for the fallback it answers NOMEM rather than silence. */
    freshSession();
    {
        uint8_t small[4];
        U_BStream bs2;

        U_bstream_init(&bs2, small, sizeof(small));
        TU_EQ(Rcp_Feed(&session, get, 3u, &bs2), 4u);
        TU_EQ(small[0], 0x81u); /* the command's TID, still correlated */
        TU_EQ(small[3], SPINEL_STATUS_NOMEM);
    }
}

TU_TEST(dialog_over_hdlc_frames_matches_the_wire)
{
    /* The whole stack in one test - hdlc + spinel + props + session - compared with the
   * captured frame including its FCS. */
    static const uint8_t hostSet[4] = {0x87u, 0x03u, 0x20u, 0x01u};
    static const uint8_t wantWire[8] = {0x7Eu, 0x87u, 0x06u, 0x20u, 0x01u, 0xF2u, 0x62u, 0x7Eu};
    uint8_t framed[HDLC_FRAME_MAX];
    uint8_t reply[OUT];
    uint8_t wire[OUT];
    U_BStream enc;
    Hdlc_Rx rx;
    const uint8_t *payload = 0;
    uint16_t n;
    uint16_t i;
    Hdlc_RxResult r = HDLC_RX_NONE;

    freshSession();

    /* host -> RCP through HDLC by hand (the transport's job in P3). */
    U_bstream_init(&enc, framed, sizeof(framed));
    {
        U_BStream rout;
        U_bstream_init(&rout, reply, sizeof(reply));
        TU_EQ(Hdlc_Encode(hostSet, 4u, &enc), 8u);
        Hdlc_RxInit(&rx);
        r = HDLC_RX_NONE;

        for (i = 0; i < 8u; ++i)
        {
            r = Hdlc_RxByte(&rx, framed[i]);

            if (r == HDLC_RX_FRAME)
                payload = rx.buf;
        }

        TU_EQ(r, HDLC_RX_FRAME);
        n = Rcp_Feed(&session, payload, rx.len, &rout);
        TU_EQ(n, 4u);
        /* HDLC-frame the reply and compare with the captured RCP frame byte for byte. */
        U_bstream_init(&enc, wire, sizeof(wire));
        TU_EQ(Hdlc_Encode(reply, n, &enc), 8u);
        TU_CHECK(same(wire, wantWire, 8u));
    }
}

/* docs/protocol.md §7.1: the TX half - the SET STREAM_RAW reply is deferred to TX done,
 * a second request is BUSY, and an unstartable one says so now rather than later. */

/* A docs/protocol.md §7.1 payload: PSDU length, the PSDU, then the 16-byte trailer. */
static uint16_t txPayload(
    uint8_t *dst, const uint8_t *psdu, uint8_t psduLen, uint8_t channel, int csma, uint8_t retries)
{
    uint16_t i;
    uint16_t off = (uint16_t)(2u + psduLen);

    dst[0] = psduLen;
    dst[1] = 0u;

    for (i = 0u; i < psduLen; ++i)
        dst[2u + i] = psdu[i];

    dst[off] = channel; /* channel */
    dst[off + 1u] = 0u; /* maxCsmaBackoffs */
    dst[off + 2u] = retries;
    dst[off + 3u] = (uint8_t)(csma ? 1 : 0);
    /* isHeaderUpdated / isRetransmission / isSecurityProcessed: the MAC's business. */
    dst[off + 4u] = 0u;
    dst[off + 5u] = 0u;
    dst[off + 6u] = 0u;
    /* txDelay (4) and txDelayBase (4) in µs. */
    for (i = 7u; i < 15u; ++i)
        dst[off + i] = 0u;

    dst[off + 15u] = 0u; /* rxChannelAfterTx: keep */
    return (uint16_t)(off + PROPS_STREAM_TX_TRAILER);
}

/* Sink stand-in: records what it was handed, so the suite can check the parse and the
 * request's TID passthrough that lets Rcp_TxDone find the request later. */
static int sinkCalls;
static int sinkRefuse;
static uint8_t sinkTid;
static PropsStream_TxRequest sinkReq;

static int fakeSink(void *ctx, const PropsStream_TxRequest *req, uint8_t tid)
{
    (void)ctx;
    sinkCalls++;

    if (sinkRefuse)
        return 0;

    sinkReq = *req;
    sinkTid = tid;
    return 1;
}

/* SET MAC_RAW_STREAM_ENABLED = true, which docs/protocol.md §7.1 requires before any TX. */
static void enableRawStream(void)
{
    uint8_t frame[4];
    uint8_t reply[OUT];

    frame[0] = 0x81u; /* command, TID 1 */
    frame[1] = 0x03u; /* CMD_PROP_VALUE_SET */
    frame[2] = 0x37u; /* PROP=MAC_RAW_STREAM_ENABLED */
    frame[3] = 0x01u;
    (void)feed(frame, 4u, reply);
}

static uint16_t setStreamRaw(uint8_t tid, const uint8_t *payload, uint16_t payLen, uint8_t *reply)
{
    uint8_t frame[192];

    frame[0] = (uint8_t)(0x80u | tid);
    frame[1] = 0x03u; /* CMD_PROP_VALUE_SET */
    frame[2] = 0x71u; /* PROP=STREAM_RAW */
    {
        uint16_t i;

        for (i = 0u; i < payLen; ++i)
            frame[3u + i] = payload[i];
    }
    return feed(frame, (uint16_t)(3u + payLen), reply);
}

/* SET RCP_MAC_KEY: mode, index, three 16-octet keys each with its length. */
static uint16_t setMacKey(uint8_t tid, uint8_t *reply)
{
    uint8_t frame[4u + 56u];
    uint8_t k;

    frame[0] = (uint8_t)(0x80u | tid);
    frame[1] = 0x03u; /* CMD_PROP_VALUE_SET */
    frame[2] = 0x80u; /* PROP=RCP_MAC_KEY 0x800, packed */
    frame[3] = 0x10u;
    frame[4] = 0x08u; /* key ID mode 1, in upstream's legacy form */
    frame[5] = 5u;    /* key index */

    for (k = 0u; k < 3u; ++k)
    {
        uint8_t i;

        frame[6u + k * 18u] = 16u;
        frame[7u + k * 18u] = 0u;

        for (i = 0u; i < 16u; ++i)
            frame[8u + k * 18u + i] = (k == 1u) ? i : 0x5Au;
    }

    return feed(frame, sizeof(frame), reply);
}

TU_TEST(a_key_is_answered_with_last_status_and_never_read_back)
{
    uint8_t reply[OUT];
    uint16_t n;

    freshSession();
    n = setMacKey(2u, reply);
    TU_EQ(n, 4u);
    TU_EQ(reply[0], 0x82u);
    TU_EQ(reply[1], 0x06u);
    TU_EQ(reply[2], 0x00u); /* LAST_STATUS */
    TU_EQ(reply[3], SPINEL_STATUS_OK);
    TU_EQ(session.props.macSec.hasKeys, 1u);
    TU_EQ(session.props.macSec.keyIndex, 5u);

    {
        static const uint8_t get[4] = {0x83u, 0x02u, 0x80u, 0x10u};

        n = feed(get, 4u, reply);
        TU_EQ(n, 4u);
        TU_EQ(reply[2], 0x00u);
        TU_EQ(reply[3], SPINEL_STATUS_PROP_NOT_FOUND);
    }
}

/* RadioSpinel sends a dummy payload with this GET, to make the two directions the same length. */
TU_TEST(the_radio_clock_answers_a_get_with_a_payload)
{
    static const uint8_t get[12] = {0x84u, 0x02u, 0x82u, 0x10u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    uint8_t reply[OUT];
    uint16_t n;

    freshSession();
    session.props.radioTimeUs = 0x0102030405060708uLL;
    n = feed(get, sizeof(get), reply);
    TU_EQ(n, 12u);
    TU_EQ(reply[1], 0x06u);
    TU_EQ(reply[2], 0x82u);
    TU_EQ(reply[3], 0x10u);
    TU_EQ(reply[4], 0x08u);
    TU_EQ(reply[11], 0x01u);
}

TU_TEST(a_reset_forgets_the_keys)
{
    uint8_t reply[OUT];
    static const uint8_t reset[3] = {0x80u, 0x01u, 0x02u};

    freshSession();
    (void)setMacKey(2u, reply);
    TU_EQ(session.props.macSec.hasKeys, 1u);
    (void)feed(reset, 3u, reply);
    TU_EQ(session.props.macSec.hasKeys, 0u);
}

/* A secured data frame with its header left to this firmware, as RadioSpinel sends it. */
static uint16_t securedTxPayload(uint8_t *payload)
{
    static const uint8_t psdu[] = {0x69u, 0xD8u, 0x11u, 0xCEu, 0xFAu, 0x34u, 0x12u, 0x11u, 0x00u, 0xFFu, 0xEEu,
                                   0xDDu, 0xCCu, 0xBBu, 0xAAu, 0x0Du, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xDEu,
                                   0xADu, 0xBEu, 0xEFu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};
    uint16_t i;
    uint16_t n = 0u;

    payload[n++] = (uint8_t)sizeof(psdu);
    payload[n++] = 0u;

    for (i = 0u; i < sizeof(psdu); ++i)
        payload[n++] = psdu[i];

    for (i = 0u; i < PROPS_STREAM_TX_TRAILER; ++i)
        payload[n++] = 0u;

    payload[2u + sizeof(psdu)] = 20u; /* channel */
    return n;
}

TU_TEST(a_secured_transmission_reaches_the_radio_encrypted_and_reports_its_header)
{
    uint8_t reply[OUT];
    uint8_t payload[80];
    uint16_t len;
    uint16_t n;
    U_BStream bs;

    freshSession();
    enableRawStream();
    Rcp_SetTxSink(&session, fakeSink, 0);
    sinkCalls = 0;
    sinkRefuse = 0;
    (void)setMacKey(2u, reply);
    session.props.macSec.frameCounter = 0x01020304uL;

    len = securedTxPayload(payload);
    TU_EQ(setStreamRaw(3u, payload, len, reply), 0u);
    TU_EQ(sinkCalls, 1);
    TU_EQ(sinkReq.psdu[16], 0x04u); /* the frame counter is written in */
    TU_EQ(sinkReq.psdu[20], 5u);    /* and the key index */
    TU_CHECK(sinkReq.psdu[21] != 0xDEu || sinkReq.psdu[22] != 0xADu); /* and the payload is not plain */

    /* The buffer the app hands over: a shorter one here is a completion lost on the bench. */
    U_bstream_init(&bs, reply, RCP_TX_DONE_MAX);
    n = Rcp_TxDone(&session, 3u, RADIO_TX_OK, 0, &bs);

    /* header, prop, status, Frame Pending, headerUpdated, the empty Ack frame, key index, counter */
    TU_EQ(n, 3u + 3u + 28u + 5u);
    TU_EQ(reply[0], 0x83u);
    TU_EQ(reply[2], 0x00u);
    TU_EQ(reply[3], SPINEL_STATUS_OK);
    TU_EQ(reply[4], 0u);
    TU_EQ(reply[5], 1u);
    TU_EQ(reply[6], 0u); /* the Ack's length */
    TU_EQ(reply[7], 0u);
    TU_EQ(reply[34], 5u);
    TU_EQ(reply[35], 0x04u);
    TU_EQ(reply[38], 0x01u);
    TU_EQ(session.props.macSec.frameCounter, 0x01020305uL);
}

TU_TEST(a_failed_secured_transmission_carries_no_ack_frame)
{
    uint8_t reply[OUT];
    uint8_t payload[80];
    uint16_t len;
    uint16_t n;
    U_BStream bs;

    freshSession();
    enableRawStream();
    Rcp_SetTxSink(&session, fakeSink, 0);
    sinkRefuse = 0;
    (void)setMacKey(2u, reply);

    len = securedTxPayload(payload);
    TU_EQ(setStreamRaw(3u, payload, len, reply), 0u);
    U_bstream_init(&bs, reply, OUT);
    n = Rcp_TxDone(&session, 3u, RADIO_TX_NO_ACK, 0, &bs);
    TU_EQ(n, 3u + 3u + 5u);
    TU_EQ(reply[3], SPINEL_STATUS_NO_ACK);
    TU_EQ(reply[5], 1u);
}

/* OpenThread's SubMac asserts on any completion but OK, NO_ACK, CCA_FAILURE or an abort, so a driver
 * timeout under a host with keys must be one of them (docs/protocol.md §7.1). */
TU_TEST(a_driver_timeout_does_not_bring_down_a_host_with_keys)
{
    uint8_t reply[OUT];
    uint8_t payload[80];
    uint16_t len;
    uint16_t n;
    U_BStream bs;

    freshSession();
    enableRawStream();
    Rcp_SetTxSink(&session, fakeSink, 0);
    sinkRefuse = 0;
    (void)setMacKey(2u, reply);

    len = securedTxPayload(payload);
    TU_EQ(setStreamRaw(3u, payload, len, reply), 0u);
    U_bstream_init(&bs, reply, OUT);
    n = Rcp_TxDone(&session, 3u, RADIO_TX_TIMEOUT, 0, &bs);
    TU_EQ(n, 3u + 3u + 5u);
    TU_EQ(reply[3], SPINEL_STATUS_CCA_FAILURE);
}

TU_TEST(a_secured_frame_with_no_key_never_reaches_the_radio)
{
    uint8_t reply[OUT];
    uint8_t payload[80];
    uint16_t len;
    uint16_t n;

    freshSession();
    enableRawStream();
    Rcp_SetTxSink(&session, fakeSink, 0);
    sinkCalls = 0;
    sinkRefuse = 0;

    len = securedTxPayload(payload);
    n = setStreamRaw(3u, payload, len, reply);
    TU_EQ(sinkCalls, 0);
    TU_CHECK(n >= 4u);
    TU_EQ(reply[3], SPINEL_STATUS_INVALID_STATE);
}

TU_TEST(set_stream_raw_defers_the_reply_and_reaches_the_sink)
{
    static const uint8_t psdu[3] = {0x01u, 0x00u, 0x42u};
    uint8_t payload[64];
    uint8_t reply[OUT];
    U_BStream bs;
    uint16_t n;

    freshSession();
    sinkCalls = 0;
    sinkRefuse = 0;
    Rcp_SetTxSink(&session, fakeSink, 0);
    enableRawStream();

    n = setStreamRaw(5u, payload, txPayload(payload, psdu, 3u, 26u, 0, 0u), reply);
    TU_EQ(n, 0u); /* no reply yet: it comes from the completion */
    TU_EQ(sinkCalls, 1);
    TU_EQ(sinkTid, 5u);
    TU_EQ(sinkReq.len, 3u);
    TU_CHECK(same(sinkReq.psdu, psdu, 3u));
    TU_EQ(sinkReq.channel, 26u);
    TU_EQ(sinkReq.csmaCa, 0);
    TU_EQ(sinkReq.maxRetries, 0u);

    /* docs/protocol.md §7.1: the completion is LAST_STATUS, correlated with the request's TID.
     * OpenThread's HandleTransmitDone unpacks status, framePending and headerUpdated in that
     * order, so the value is exactly those three fields. */
    U_bstream_init(&bs, reply, OUT);
    n = Rcp_TxDone(&session, 5u, RADIO_TX_OK, 0, &bs);
    TU_EQ(n, 6u);
    TU_EQ(reply[0], 0x85u); /* command, TID 5 */
    TU_EQ(reply[1], 0x06u); /* CMD_PROP_VALUE_IS */
    TU_EQ(reply[2], 0x00u); /* PROP=LAST_STATUS */
    TU_EQ(reply[3], 0x00u); /* status = OK */
    TU_EQ(reply[4], 0x00u); /* framePending = false */
    TU_EQ(reply[5], 0x00u); /* headerUpdated = false */
}

/* docs/protocol.md §7.1: a maximum PSDU is 2 + 127 + 16 = 145 value bytes; the session's
 * value path must carry it whole, not truncate it at the old 64-byte ceiling. */
TU_TEST(a_maximum_psdu_request_reaches_the_sink_whole)
{
    uint8_t psdu[RADIO_PSDU_MAX];
    uint8_t payload[2u + RADIO_PSDU_MAX + PROPS_STREAM_TX_TRAILER];
    uint8_t reply[OUT];
    uint16_t i;
    uint16_t n;

    freshSession();
    sinkCalls = 0;
    sinkRefuse = 0;
    Rcp_SetTxSink(&session, fakeSink, 0);
    enableRawStream();

    for (i = 0u; i < RADIO_PSDU_MAX; ++i)
        psdu[i] = (uint8_t)(0x40u + i);

    n = setStreamRaw(5u, payload, txPayload(payload, psdu, RADIO_PSDU_MAX, 26u, 0, 0u), reply);
    TU_EQ(n, 0u); /* accepted: the reply is deferred to the completion */
    TU_EQ(sinkCalls, 1);
    TU_EQ(sinkReq.len, RADIO_PSDU_MAX);
    TU_CHECK(same(sinkReq.psdu, psdu, RADIO_PSDU_MAX));
}

TU_TEST(a_second_frame_while_one_is_in_flight_is_busy)
{
    static const uint8_t psdu[3] = {0x01u, 0x00u, 0x42u};
    uint8_t payload[64];
    uint8_t reply[OUT];
    uint16_t n;

    freshSession();
    sinkCalls = 0;
    sinkRefuse = 0;
    Rcp_SetTxSink(&session, fakeSink, 0);
    enableRawStream();

    (void)setStreamRaw(5u, payload, txPayload(payload, psdu, 3u, 26u, 0, 0u), reply);
    TU_EQ(sinkCalls, 1);

    n = setStreamRaw(6u, payload, txPayload(payload, psdu, 3u, 26u, 0, 0u), reply);
    TU_EQ(n, 6u);
    TU_EQ(reply[0], 0x86u); /* the *second* request's TID ... */
    TU_EQ(reply[3], 0x0Cu); /* ... and BUSY */
    TU_EQ(reply[4], 0x00u); /* framePending */
    TU_EQ(reply[5], 0x00u); /* headerUpdated */
    TU_EQ(sinkCalls, 1);    /* the sink was not called again */

    /* After the completion the slot is free again. */
    {
        U_BStream bs;
        U_bstream_init(&bs, reply, OUT);
        TU_EQ(Rcp_TxDone(&session, 5u, RADIO_TX_OK, 0, &bs), 6u);
    }
    n = setStreamRaw(7u, payload, txPayload(payload, psdu, 3u, 26u, 0, 0u), reply);
    TU_EQ(n, 0u);
    TU_EQ(sinkCalls, 2);
    TU_EQ(sinkTid, 7u);
}

TU_TEST(tx_without_the_raw_stream_is_invalid_state)
{
    static const uint8_t psdu[3] = {0x01u, 0x00u, 0x42u};
    uint8_t payload[64];
    uint8_t reply[OUT];
    uint16_t n;

    freshSession();
    sinkCalls = 0;
    sinkRefuse = 0;
    Rcp_SetTxSink(&session, fakeSink, 0);

    n = setStreamRaw(5u, payload, txPayload(payload, psdu, 3u, 26u, 0, 0u), reply);
    TU_EQ(n, 6u);
    /* docs/protocol.md §7.1: the TX TID is reserved for STREAM_RAW, so a refusal carries the
     * same three-field LAST_STATUS the completion does, not a bare status. */
    TU_EQ(reply[3], 0x04u); /* docs/protocol.md §7.1: INVALID_STATE */
    TU_EQ(reply[4], 0x00u); /* framePending */
    TU_EQ(reply[5], 0x00u); /* headerUpdated */
    TU_EQ(sinkCalls, 0);
}

TU_TEST(a_refusing_sink_is_invalid_state_and_leaves_nothing_in_flight)
{
    static const uint8_t psdu[3] = {0x01u, 0x00u, 0x42u};
    uint8_t payload[64];
    uint8_t reply[OUT];
    uint16_t n;

    freshSession();
    sinkCalls = 0;
    sinkRefuse = 1;
    Rcp_SetTxSink(&session, fakeSink, 0);
    enableRawStream();

    n = setStreamRaw(5u, payload, txPayload(payload, psdu, 3u, 26u, 0, 0u), reply);
    TU_EQ(n, 6u);
    /* docs/protocol.md §7.1: a refusal is answered at once, and the answer carries the
     * refused request's TID - the host must never be left waiting for a completion. */
    TU_EQ(reply[0], 0x85u); /* the refused request's TID ... */
    TU_EQ(reply[1], 0x06u); /* ... CMD_PROP_VALUE_IS ... */
    TU_EQ(reply[2], 0x00u); /* ... PROP=LAST_STATUS ... */
    TU_EQ(reply[3], 0x04u); /* ... INVALID_STATE ... */
    TU_EQ(reply[4], 0x00u); /* ... framePending ... */
    TU_EQ(reply[5], 0x00u); /* ... headerUpdated */

    /* Nothing is in flight, so the next request is not BUSY. */
    sinkRefuse = 0;
    n = setStreamRaw(6u, payload, txPayload(payload, psdu, 3u, 26u, 0, 0u), reply);
    TU_EQ(n, 0u);
    TU_EQ(sinkTid, 6u);

    /* An accepted request still defers: its own completion carries its TID. */
    {
        U_BStream bs;

        U_bstream_init(&bs, reply, OUT);
        TU_EQ(Rcp_TxDone(&session, 6u, RADIO_TX_OK, 0, &bs), 6u);
        TU_EQ(reply[0], 0x86u);
        TU_EQ(reply[3], 0x00u);
    }
}

/* docs/protocol.md §7.1: a session with no sink cannot hand the frame to the radio, so
 * the request is answered now with the request's TID, never deferred. */
TU_TEST(tx_without_a_sink_is_invalid_state_with_the_tid)
{
    static const uint8_t psdu[3] = {0x01u, 0x00u, 0x42u};
    uint8_t payload[64];
    uint8_t reply[OUT];
    uint16_t n;

    freshSession();
    sinkCalls = 0;
    Rcp_SetTxSink(&session, 0, 0);
    enableRawStream();

    n = setStreamRaw(9u, payload, txPayload(payload, psdu, 3u, 26u, 0, 0u), reply);
    TU_EQ(n, 6u);
    TU_EQ(reply[0], 0x89u); /* the request's TID */
    TU_EQ(reply[1], 0x06u); /* CMD_PROP_VALUE_IS */
    TU_EQ(reply[2], 0x00u); /* PROP=LAST_STATUS */
    TU_EQ(reply[3], 0x04u); /* INVALID_STATE */
    TU_EQ(reply[4], 0x00u); /* framePending */
    TU_EQ(reply[5], 0x00u); /* headerUpdated */
    TU_EQ(sinkCalls, 0);
}

TU_TEST(a_malformed_payload_is_a_parse_error)
{
    static const uint8_t psdu[3] = {0x01u, 0x00u, 0x42u};
    uint8_t payload[64];
    uint8_t reply[OUT];
    uint16_t n;

    freshSession();
    sinkCalls = 0;
    sinkRefuse = 0;
    Rcp_SetTxSink(&session, fakeSink, 0);
    enableRawStream();

    /* A value that ends before the channel, the one trailer field a request must carry. */
    (void)txPayload(payload, psdu, 3u, 26u, 0, 0u);
    n = setStreamRaw(5u, payload, (uint16_t)(2u + 3u), reply);
    TU_EQ(n, 6u);
    TU_EQ(reply[3], 0x09u); /* PARSE_ERROR */
    TU_EQ(reply[4], 0x00u); /* framePending */
    TU_EQ(reply[5], 0x00u); /* headerUpdated */
    TU_EQ(sinkCalls, 0);

    /* A channel outside 11..26 is malformed too, and 0 means "keep the current one". */
    n = setStreamRaw(6u, payload, txPayload(payload, psdu, 3u, 27u, 0, 0u), reply);
    TU_EQ(reply[3], 0x09u);
    n = setStreamRaw(7u, payload, txPayload(payload, psdu, 3u, 0u, 0, 0u), reply);
    TU_EQ(n, 0u);
    TU_EQ(sinkReq.channel, 0u);

    /* And none of that left a transmission in flight. */
    TU_EQ(sinkCalls, 1);
}

TU_TEST(a_stale_completion_is_not_answered)
{
    static const uint8_t psdu[3] = {0x01u, 0x00u, 0x42u};
    uint8_t payload[64];
    uint8_t reply[OUT];
    U_BStream bs;
    uint16_t n;

    freshSession();
    sinkCalls = 0;
    sinkRefuse = 0;
    Rcp_SetTxSink(&session, fakeSink, 0);

    /* Nothing in flight at all. */
    U_bstream_init(&bs, reply, OUT);
    TU_EQ(Rcp_TxDone(&session, 5u, RADIO_TX_OK, 0, &bs), 0u);

    enableRawStream();
    (void)setStreamRaw(5u, payload, txPayload(payload, psdu, 3u, 26u, 0, 0u), reply);

    /* A completion for a TID this session never started is a driver bug: answering it would
   * put an unmatched TID in front of the host. */
    U_bstream_init(&bs, reply, OUT);
    n = Rcp_TxDone(&session, 6u, RADIO_TX_OK, 0, &bs);
    TU_EQ(n, 0u);

    /* The real one still completes. */
    U_bstream_init(&bs, reply, OUT);
    TU_EQ(Rcp_TxDone(&session, 5u, RADIO_TX_OK, 0, &bs), 6u);
    TU_EQ(reply[0], 0x85u);
}

/* Rcp_Healthy gates the watchdog feed (docs/architecture.md §7): only a transmission that outlives
 * RCP_TX_STUCK_MS plus its own delay makes the session unhealthy. */
TU_TEST(a_transmission_that_never_completes_makes_the_session_unhealthy)
{
    static const uint8_t psdu[3] = {0x01u, 0x00u, 0x42u};
    uint8_t payload[64];
    uint8_t reply[OUT];
    U_BStream bs;
    const uint32_t t0 = 0xFFFFFF00uL; /* the tick wraps while the frame is open */

    freshSession();
    sinkRefuse = 0;
    Rcp_SetTxSink(&session, fakeSink, 0);
    enableRawStream();

    /* Idle is healthy whatever the time. */
    TU_EQ(Rcp_Healthy(&session, 0u), 1);
    TU_EQ(Rcp_Healthy(&session, 0x80000000uL), 1);

    (void)setStreamRaw(5u, payload, txPayload(payload, psdu, 3u, 26u, 0, 0u), reply);

    /* The first look starts the clock; the limit itself is still healthy, one past it is not. */
    TU_EQ(Rcp_Healthy(&session, t0), 1);
    TU_EQ(Rcp_Healthy(&session, t0 + RCP_TX_STUCK_MS), 1);
    TU_EQ(Rcp_Healthy(&session, t0 + RCP_TX_STUCK_MS + 1u), 0);

    /* A late completion makes it healthy again, and the next frame gets a clock of its own. */
    U_bstream_init(&bs, reply, OUT);
    TU_EQ(Rcp_TxDone(&session, 5u, RADIO_TX_OK, 0, &bs), 6u);
    TU_EQ(Rcp_Healthy(&session, t0 + RCP_TX_STUCK_MS + 2u), 1);

    (void)setStreamRaw(6u, payload, txPayload(payload, psdu, 3u, 26u, 0, 0u), reply);
    TU_EQ(Rcp_Healthy(&session, t0 + 2u * RCP_TX_STUCK_MS), 1);
    TU_EQ(Rcp_Healthy(&session, t0 + 3u * RCP_TX_STUCK_MS), 1);
    TU_EQ(Rcp_Healthy(&session, t0 + 3u * RCP_TX_STUCK_MS + 1u), 0);
}

TU_TEST(a_timed_transmission_has_its_delay_on_top_of_the_limit)
{
    static const uint8_t psdu[3] = {0x01u, 0x00u, 0x42u};
    uint8_t payload[64];
    uint8_t reply[OUT];
    uint16_t len;
    const uint32_t delayUs = 3000000uL; /* 3 s */
    const uint16_t trailer = (uint16_t)(2u + sizeof(psdu));

    freshSession();
    sinkRefuse = 0;
    Rcp_SetTxSink(&session, fakeSink, 0);
    enableRawStream();

    len = txPayload(payload, psdu, 3u, 26u, 0, 0u);
    payload[trailer + 7u] = (uint8_t)delayUs;
    payload[trailer + 8u] = (uint8_t)(delayUs >> 8);
    payload[trailer + 9u] = (uint8_t)(delayUs >> 16);
    payload[trailer + 10u] = (uint8_t)(delayUs >> 24);
    (void)setStreamRaw(5u, payload, len, reply);
    TU_EQ(sinkReq.txDelay, delayUs);

    /* Never earlier than the delay plus the limit: a frame asked for 3 s ahead is not stuck at 3 s. */
    TU_EQ(Rcp_Healthy(&session, 0u), 1);
    TU_EQ(Rcp_Healthy(&session, 3000u + RCP_TX_STUCK_MS), 1);
    TU_EQ(Rcp_Healthy(&session, RCP_TX_STUCK_MS + (delayUs >> 9)), 1);
    TU_EQ(Rcp_Healthy(&session, RCP_TX_STUCK_MS + (delayUs >> 9) + 1u), 0);
}

static void putU32At(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)v;
    dst[1] = (uint8_t)(v >> 8);
    dst[2] = (uint8_t)(v >> 16);
    dst[3] = (uint8_t)(v >> 24);
}

/* A timed frame is due at base + delay on the radio's clock, and the host chooses both: the limit runs
 * from that moment, whatever share of the lead the base carries. Otherwise a host that names a base
 * ahead of the clock keeps a healthy frame in flight past the limit, and the watchdog resets the board
 * for input alone. */
TU_TEST(a_timed_transmission_is_not_stuck_before_its_moment)
{
    static const uint8_t psdu[3] = {0x01u, 0x00u, 0x42u};
    uint8_t payload[64];
    uint8_t reply[OUT];
    uint16_t len;
    const uint16_t trailer = (uint16_t)(2u + sizeof(psdu));
    const uint32_t nowUs = 0xFFFF0000uL;       /* the radio's clock wraps before the moment */
    const uint32_t leadUs = 60000000uL;        /* 60 s ahead, nearly all of it in the base */
    const uint32_t delayUs = 1000u;

    freshSession();
    sinkRefuse = 0;
    Rcp_SetTxSink(&session, fakeSink, 0);
    enableRawStream();
    session.props.radioTimeUs = 0x500000000uLL | nowUs;

    len = txPayload(payload, psdu, 3u, 26u, 0, 0u);
    putU32At(&payload[trailer + 7u], delayUs);
    putU32At(&payload[trailer + 11u], nowUs + leadUs - delayUs);
    (void)setStreamRaw(5u, payload, len, reply);
    TU_EQ(sinkReq.txDelay, delayUs);

    TU_EQ(Rcp_Healthy(&session, 0u), 1);
    TU_EQ(Rcp_Healthy(&session, leadUs / 1000u + RCP_TX_STUCK_MS), 1);
    TU_EQ(Rcp_Healthy(&session, RCP_TX_STUCK_MS + (leadUs >> 9)), 1);
    TU_EQ(Rcp_Healthy(&session, RCP_TX_STUCK_MS + (leadUs >> 9) + 1u), 0);

    /* A moment already gone completes at once (radio.h), so it gets the plain limit. */
    freshSession();
    Rcp_SetTxSink(&session, fakeSink, 0);
    enableRawStream();
    session.props.radioTimeUs = nowUs;
    len = txPayload(payload, psdu, 3u, 26u, 0, 0u);
    putU32At(&payload[trailer + 7u], delayUs);
    putU32At(&payload[trailer + 11u], nowUs - 5000000uL);
    (void)setStreamRaw(6u, payload, len, reply);
    TU_EQ(Rcp_Healthy(&session, 0u), 1);
    TU_EQ(Rcp_Healthy(&session, RCP_TX_STUCK_MS), 1);
    TU_EQ(Rcp_Healthy(&session, RCP_TX_STUCK_MS + 1u), 0);
}

/* A host RESET while a frame is still on its way. The radio is still sending it, so a new transmit
 * request is answered BUSY until it is done - the truth about the radio. But its completion belongs to
 * a session that no longer exists: answered, it would put a TID from before the RESET in front of a
 * host that has started over and may be waiting on that TID for something else. */
TU_TEST(a_reset_during_a_transmission_drops_its_completion)
{
    static const uint8_t psdu[3] = {0x01u, 0x00u, 0x42u};
    static const uint8_t reset[2] = {0x81u, 0x01u};
    uint8_t payload[64];
    uint8_t reply[OUT];
    U_BStream bs;
    uint16_t n;

    freshSession();
    sinkCalls = 0;
    sinkRefuse = 0;
    Rcp_SetTxSink(&session, fakeSink, 0);
    enableRawStream();
    (void)setStreamRaw(5u, payload, txPayload(payload, psdu, 3u, 26u, 0, 0u), reply);
    TU_EQ(sinkCalls, 1);

    TU_CHECK(feed(reset, sizeof(reset), reply) > 0u);

    enableRawStream();
    n = setStreamRaw(7u, payload, txPayload(payload, psdu, 3u, 26u, 0, 0u), reply);
    TU_EQ(sinkCalls, 1);
    TU_CHECK(n >= 4u);
    TU_EQ(reply[0], 0x87u);
    TU_EQ(reply[3], SPINEL_STATUS_BUSY);

    /* The radio finishes the frame from before the RESET: nothing goes to the host. */
    U_bstream_init(&bs, reply, OUT);
    TU_EQ(Rcp_TxDone(&session, 5u, RADIO_TX_OK, 0, &bs), 0u);
    TU_EQ(Rcp_Healthy(&session, 0u), 1);

    /* And the radio is the host's again. */
    (void)setStreamRaw(8u, payload, txPayload(payload, psdu, 3u, 26u, 0, 0u), reply);
    TU_EQ(sinkCalls, 2);
    U_bstream_init(&bs, reply, OUT);
    TU_CHECK(Rcp_TxDone(&session, 8u, RADIO_TX_OK, 0, &bs) > 0u);
}

/* The property handler's status is the answer, not NOMEM (docs/protocol.md §5). */
TU_TEST(a_get_that_fails_reports_the_handlers_status)
{
    Props_Identity id;
    uint8_t frame[8];
    uint8_t reply[OUT];
    uint16_t n;

    /* HWADDR is readable, and refuses when the board has no factory address. */
    id.ncpVersion = "DSPINEL/1.0";
    id.rxSensitivityDbm = -96;
    id.txPowerMaxDbm = 127;
    id.txPowerMaxCh26Dbm = 127;
    id.hwAddr = 0;
    Rcp_Init(&session, &id);

    frame[0] = 0x81u; /* command, TID 1 */
    frame[1] = 0x02u; /* CMD_PROP_VALUE_GET */
    frame[2] = 0x08u; /* PROP=HWADDR */
    n = feed(frame, 3u, reply);
    TU_EQ(n, 4u);
    TU_EQ(reply[0], 0x81u); /* the request's TID */
    TU_EQ(reply[1], 0x06u); /* CMD_PROP_VALUE_IS */
    TU_EQ(reply[2], 0x00u); /* PROP=LAST_STATUS */
    TU_EQ(reply[3], 0x04u); /* INVALID_STATE, not NOMEM (11) */
}

/* docs/protocol.md §7.1: the completion carries the radio outcome; NO_ACK comes from the
 * own engine, the only party listening during the acknowledgment window. The second value
 * byte is the ACK's Frame Pending bit, the third is headerUpdated (always 0): a sleepy
 * child's host reads the first to decide whether it must poll, so both values must survive. */
TU_TEST(a_completion_carries_the_radios_outcome_and_the_acks_frame_pending_bit)
{
    static const uint8_t psdu[3] = {0x01u, 0x00u, 0x42u};
    uint8_t payload[64];
    uint8_t reply[OUT];
    U_BStream bs;
    uint16_t n;

    struct
    {
        int radio;
        int framePending;
        uint8_t spinel;
    } cases[] = {{RADIO_TX_OK, 0, SPINEL_STATUS_OK},
                 {RADIO_TX_OK, 1, SPINEL_STATUS_OK},
                 {RADIO_TX_NO_ACK, 0, SPINEL_STATUS_NO_ACK},
                 {RADIO_TX_CCA_FAIL, 0, SPINEL_STATUS_CCA_FAILURE},
                 {RADIO_TX_TIMEOUT, 0, SPINEL_STATUS_FAILURE}};
    uint16_t i;

    for (i = 0u; i < (uint16_t)(sizeof(cases) / sizeof(cases[0])); ++i)
    {
        freshSession();
        sinkCalls = 0;
        sinkRefuse = 0;
        Rcp_SetTxSink(&session, fakeSink, 0);
        enableRawStream();
        (void)setStreamRaw(5u, payload, txPayload(payload, psdu, 3u, 26u, 0, 0u), reply);

        U_bstream_init(&bs, reply, OUT);
        n = Rcp_TxDone(&session, 5u, cases[i].radio, cases[i].framePending, &bs);
        TU_EQ(n, 6u);
        TU_EQ(reply[3], cases[i].spinel);
        TU_EQ(reply[4], (uint8_t)cases[i].framePending); /* the ACK's Frame Pending bit */
        TU_EQ(reply[5], 0x00u);                          /* headerUpdated, always 0 */
    }
}

/* docs/protocol.md §7.1: only the TX-done path and the STREAM_RAW refusals carry the
 * three-field LAST_STATUS; every other reply keeps the bare status, so a GET answer is
 * unchanged by the TX fix. */
/* The scan family on the wire: what a SET answers, what a refusal answers, and which ids are not
 * properties at all (docs/protocol.md §10.1). The frames are pinned byte for byte because a host
 * decides what to do next from these shapes alone. */
TU_TEST(the_scan_family_is_answered_on_the_wire)
{
    uint8_t reply[OUT];
    uint16_t n;

    freshSession();

    /* Host: SET PHY_ENABLED = 1, then the channel list, then the dwell, then the start. Each answer is
     * the property re-read (docs/protocol.md §5), so a host learns that a scan began without asking
     * a second question. */
    {
        static const uint8_t setPhy[4] = {0x81u, 0x03u, 0x20u, 0x01u};
        static const uint8_t wantPhy[4] = {0x81u, 0x06u, 0x20u, 0x01u};

        n = feed(setPhy, 4u, reply);
        TU_EQ(n, 4u);
        TU_CHECK(same(reply, wantPhy, 4u));
    }
    {
        static const uint8_t setMask[5] = {0x82u, 0x03u, 0x31u, 0x01u, 15u};
        static const uint8_t wantMask[5] = {0x82u, 0x06u, 0x31u, 0x01u, 15u};

        n = feed(setMask, 5u, reply);
        TU_EQ(n, 5u);
        TU_CHECK(same(reply, wantMask, 5u));
    }
    {
        static const uint8_t setPeriod[5] = {0x83u, 0x03u, 0x32u, 100u, 0x00u};
        static const uint8_t wantPeriod[5] = {0x83u, 0x06u, 0x32u, 100u, 0x00u};

        n = feed(setPeriod, 5u, reply);
        TU_EQ(n, 5u);
        TU_CHECK(same(reply, wantPeriod, 5u));
    }
    {
        /* SET MAC_SCAN_STATE = 2: accepted, and the echo carries 2, because a raw RCP's scan state is
         * energy while a dwell is running. */
        static const uint8_t start[4] = {0x84u, 0x03u, 0x30u, 0x02u};
        static const uint8_t wantStart[4] = {0x84u, 0x06u, 0x30u, 0x02u};

        n = feed(start, 4u, reply);
        TU_EQ(n, 4u);
        TU_CHECK(same(reply, wantStart, 4u));
    }
    {
        /* A second start while the first owns the machine is a state error, in the bare status shape. */
        static const uint8_t again[4] = {0x85u, 0x03u, 0x30u, 0x02u};
        static const uint8_t wantAgain[4] = {0x85u, 0x06u, 0x00u, 0x04u}; /* LAST_STATUS INVALID_STATE */

        n = feed(again, 4u, reply);
        TU_EQ(n, 4u);
        TU_CHECK(same(reply, wantAgain, 4u));
    }
    {
        /* A cancel is accepted and answers as a re-read: still 2, because a dwell ends by its own
         * dwell, not by a second write. */
        static const uint8_t cancel[4] = {0x86u, 0x03u, 0x30u, 0x00u};
        static const uint8_t wantCancel[4] = {0x86u, 0x06u, 0x30u, 0x02u};

        n = feed(cancel, 4u, reply);
        TU_EQ(n, 4u);
        TU_CHECK(same(reply, wantCancel, 4u));
    }
    {
        /* Beacon and discover are not implemented by a raw RCP, and the answer is UNIMPLEMENTED - the
         * answer a stock OpenThread coprocessor gives the same request. */
        static const uint8_t beacon[4] = {0x87u, 0x03u, 0x30u, 0x01u};
        static const uint8_t wantUnimpl[4] = {0x87u, 0x06u, 0x00u, 0x02u};

        n = feed(beacon, 4u, reply);
        TU_EQ(n, 4u);
        TU_CHECK(same(reply, wantUnimpl, 4u));
    }
    {
        /* MAC_SCAN_RESULT is an event, not a property: a GET asks for something this table does not
         * have, which is PROP_NOT_FOUND - what upstream's own conformance gate asserts for an RCP. */
        static const uint8_t getResult[3] = {0x88u, 0x02u, 0x39u};
        static const uint8_t wantNotFound[4] = {0x88u, 0x06u, 0x00u, 0x0Du};

        n = feed(getResult, 3u, reply);
        TU_EQ(n, 4u);
        TU_CHECK(same(reply, wantNotFound, 4u));
    }
    {
        /* A fourteen-channel list is a legal value, and the answer is the list re-read: count byte and
         * channels, in the order they were written (docs/protocol.md §10.1). */
        static const uint8_t band[18] = {0x89u, 0x03u, 0x31u, 14u, 12u, 13u, 14u, 15u, 16u,
                                         17u, 18u, 19u, 20u, 21u, 22u, 23u, 24u, 25u};
        static const uint8_t bandEcho[18] = {0x89u, 0x06u, 0x31u, 14u, 12u, 13u, 14u, 15u, 16u,
                                             17u, 18u, 19u, 20u, 21u, 22u, 23u, 24u, 25u};
        static const uint8_t startAgain[4] = {0x8Au, 0x03u, 0x30u, 0x02u};
        static const uint8_t wantState[4] = {0x8Au, 0x06u, 0x00u, 0x04u}; /* INVALID_STATE */

        n = feed(band, 18u, reply);
        TU_EQ(n, 18u);
        TU_CHECK(same(reply, bandEcho, 18u));

        n = feed(startAgain, 4u, reply);
        TU_EQ(n, 4u);
        /* A second start while the first dwell owns the machine is refused on the state, before the
         * mask is looked at: two dwells at once is a host bug, not a queue. The mask that cannot hold a
         * dwell is refused by the property suite, which is where that rule lives. */
        TU_CHECK(same(reply, wantState, 4u));
    }
    {
        /* A reset drops the dwell's bookkeeping with everything else the session owns: after CMD_RESET
         * the state reads idle again (docs/protocol.md §6). What the transceiver is left doing is the
         * app's duty, and the rig is where that is proved. */
        static const uint8_t reset[3] = {0x81u, 0x01u, 0x02u};
        static const uint8_t getAfter[3] = {0x82u, 0x02u, 0x30u};
        static const uint8_t wantIdle[4] = {0x82u, 0x06u, 0x30u, 0x00u};

        n = feed(reset, 3u, reply);
        TU_CHECK(n >= 4u);
        n = feed(getAfter, 3u, reply);
        TU_EQ(n, 4u);
        TU_CHECK(same(reply, wantIdle, 4u));
    }
    {
        /* A band the silicon does not have is refused as a dwell, and the mask that produced it stays
         * stored: a host is told which of its two writes was the problem. */
        static const uint8_t setPhy[4] = {0x83u, 0x03u, 0x20u, 0x01u};
        static const uint8_t setMask[5] = {0x84u, 0x03u, 0x31u, 0x01u, 5u};
        static const uint8_t wantMask[5] = {0x84u, 0x06u, 0x31u, 0x01u, 5u};
        static const uint8_t start[4] = {0x85u, 0x03u, 0x30u, 0x02u};
        static const uint8_t wantArg[4] = {0x85u, 0x06u, 0x00u, 0x03u}; /* INVALID_ARGUMENT (3) */

        feed(setPhy, 4u, reply);
        n = feed(setMask, 5u, reply);
        TU_EQ(n, 5u);
        TU_CHECK(same(reply, wantMask, 5u));
        n = feed(start, 4u, reply);
        TU_EQ(n, 4u);
        TU_CHECK(same(reply, wantArg, 4u));
    }
}

TU_TEST(a_non_tx_reply_keeps_the_bare_status_shape)
{
    static const uint8_t getMissing[3] = {0x81u, 0x02u, 0x7Fu};         /* GET 0x7F, TID 1 */
    static const uint8_t wantMissing[4] = {0x81u, 0x06u, 0x00u, 0x0Du}; /* PROP_NOT_FOUND */
    static const uint8_t getVersion[3] = {0x82u, 0x02u, 0x01u};         /* GET PROTOCOL_VERSION */
    uint8_t reply[OUT];
    uint16_t n;

    freshSession();

    /* A failed GET is a four-byte LAST_STATUS: status alone, no framePending/headerUpdated. */
    n = feed(getMissing, 3u, reply);
    TU_EQ(n, 4u);
    TU_CHECK(same(reply, wantMissing, 4u));

    /* A successful GET echoes the property value, not a TX status. */
    n = feed(getVersion, 3u, reply);
    TU_EQ(n, 5u);
    TU_EQ(reply[0], 0x82u);
    TU_EQ(reply[1], 0x06u);
    TU_EQ(reply[2], 0x01u); /* PROP=PROTOCOL_VERSION */
    TU_EQ(reply[3], 0x04u); /* value 4 */
}

TU_TEST(insert_and_remove_are_answered_on_the_wire)
{
    uint8_t reply[OUT];
    uint16_t n;

    freshSession();
    {
        /* SET 0x1303 (ENABLED) = 1: 'b' echoed back, TID 4. Property 0x1303 is packed as
         * 0x83 0x26. */
        static const uint8_t set[5] = {0x84u, 0x03u, 0x83u, 0x26u, 0x01u};
        static const uint8_t want[5] = {0x84u, 0x06u, 0x83u, 0x26u, 0x01u};

        n = feed(set, sizeof(set), reply);
        TU_EQ(n, 5u);
        TU_CHECK(same(reply, want, 5u));
    }
    {
        /* INSERT 0x1304 (short list) with one address 0x0002: answered VALUE_INSERTED
         * carrying the request's value, not a LIST_STATUS and not the stored list. */
        static const uint8_t ins[6] = {0x85u, 0x04u, 0x84u, 0x26u, 0x02u, 0x00u};
        static const uint8_t want[6] = {0x85u, 0x07u, 0x84u, 0x26u, 0x02u, 0x00u};

        n = feed(ins, sizeof(ins), reply);
        TU_EQ(n, 6u);
        TU_CHECK(same(reply, want, 6u));
    }
    {
        /* GET 0x1304 answers the stored list: one entry, little endian. */
        static const uint8_t get[4] = {0x86u, 0x02u, 0x84u, 0x26u};
        static const uint8_t want[6] = {0x86u, 0x06u, 0x84u, 0x26u, 0x02u, 0x00u};

        n = feed(get, sizeof(get), reply);
        TU_EQ(n, 6u);
        TU_CHECK(same(reply, want, 6u));
    }
    {
        /* REMOVE the same entry: VALUE_REMOVED, same value echoed. */
        static const uint8_t rem[6] = {0x87u, 0x05u, 0x84u, 0x26u, 0x02u, 0x00u};
        static const uint8_t want[6] = {0x87u, 0x08u, 0x84u, 0x26u, 0x02u, 0x00u};

        n = feed(rem, sizeof(rem), reply);
        TU_EQ(n, 6u);
        TU_CHECK(same(reply, want, 6u));
    }
    {
        /* REMOVE it again: ITEM_NOT_FOUND (20), as a LAST_STATUS with the same TID. */
        static const uint8_t rem[6] = {0x88u, 0x05u, 0x84u, 0x26u, 0x02u, 0x00u};
        static const uint8_t want[4] = {0x88u, 0x06u, 0x00u, 0x14u};

        n = feed(rem, sizeof(rem), reply);
        TU_EQ(n, 4u);
        TU_CHECK(same(reply, want, 4u));
    }
    {
        /* INSERT on a property that is not a table: PROP_NOT_FOUND, as upstream answers. */
        static const uint8_t ins[5] = {0x89u, 0x04u, 0x36u, 0x02u, 0x00u};
        static const uint8_t want[4] = {0x89u, 0x06u, 0x00u, 0x0Du};

        n = feed(ins, sizeof(ins), reply);
        TU_EQ(n, 4u);
        TU_CHECK(same(reply, want, 4u));
    }
    TU_EQ(session.unknownProp, 1u); /* only the last one */
    TU_EQ(session.unknownCmd, 0u);  /* INSERT and REMOVE are no longer unknown commands */
}

TU_TEST(a_full_source_match_table_answers_nomem_and_keeps_the_old_list)
{
    uint8_t reply[OUT];
    uint16_t n;
    /* Header, SET, the packed id of 0x1304 (0x84 0x26), then 33 short addresses. */
    uint8_t set[4u + 33u * 2u];

    freshSession();

    set[0] = 0x81u;
    set[1] = 0x03u;
    set[2] = 0x84u;
    set[3] = 0x26u;

    for (uint16_t k = 0; k < 33u * 2u; ++k)
        set[4u + k] = (uint8_t)k;

    /* One entry too many: LAST_STATUS NOMEM (11), and the empty table stays empty. */
    {
        static const uint8_t want[4] = {0x81u, 0x06u, 0x00u, 0x0Bu};

        n = feed(set, (uint16_t)sizeof(set), reply);
        TU_EQ(n, 4u);
        TU_CHECK(same(reply, want, 4u));
    }
    {
        static const uint8_t get[4] = {0x82u, 0x02u, 0x84u, 0x26u};
        static const uint8_t want[4] = {0x82u, 0x06u, 0x84u, 0x26u}; /* empty list */

        n = feed(get, sizeof(get), reply);
        TU_EQ(n, 4u);
        TU_CHECK(same(reply, want, 4u));
    }
}

int main(void)
{
    static const Tu_Test tests[] = {
        {"replays_the_captured_bringup_dialog", replays_the_captured_bringup_dialog},
        {"replays_the_captured_form_network_set_sequence",
         replays_the_captured_form_network_set_sequence},
        {"unknown_command_is_invalid_command_with_the_same_tid",
         unknown_command_is_invalid_command_with_the_same_tid},
        {"unknown_property_is_prop_not_found_with_the_same_tid",
         unknown_property_is_prop_not_found_with_the_same_tid},
        {"refused_set_is_invalid_arg_and_the_value_is_unchanged",
         refused_set_is_invalid_arg_and_the_value_is_unchanged},
        {"reset_is_uncorrelated_and_reinitialises", reset_is_uncorrelated_and_reinitialises},
        {"malformed_frames_are_counted_and_unanswered",
         malformed_frames_are_counted_and_unanswered},
        {"a_reply_that_does_not_fit_is_counted_not_truncated",
         a_reply_that_does_not_fit_is_counted_not_truncated},
        {"dialog_over_hdlc_frames_matches_the_wire", dialog_over_hdlc_frames_matches_the_wire},
        {"set_stream_raw_defers_the_reply_and_reaches_the_sink",
         set_stream_raw_defers_the_reply_and_reaches_the_sink},
        {"a_maximum_psdu_request_reaches_the_sink_whole",
         a_maximum_psdu_request_reaches_the_sink_whole},
        {"a_second_frame_while_one_is_in_flight_is_busy",
         a_second_frame_while_one_is_in_flight_is_busy},
        {"tx_without_the_raw_stream_is_invalid_state", tx_without_the_raw_stream_is_invalid_state},
        {"a_refusing_sink_is_invalid_state_and_leaves_nothing_in_flight",
         a_refusing_sink_is_invalid_state_and_leaves_nothing_in_flight},
        {"tx_without_a_sink_is_invalid_state_with_the_tid",
         tx_without_a_sink_is_invalid_state_with_the_tid},
        {"a_malformed_payload_is_a_parse_error", a_malformed_payload_is_a_parse_error},
        {"a_stale_completion_is_not_answered", a_stale_completion_is_not_answered},
        {"a_get_that_fails_reports_the_handlers_status",
         a_get_that_fails_reports_the_handlers_status},
        {"a_completion_carries_the_radios_outcome_and_the_acks_frame_pending_bit",
         a_completion_carries_the_radios_outcome_and_the_acks_frame_pending_bit},
        {"a_key_is_answered_with_last_status_and_never_read_back", a_key_is_answered_with_last_status_and_never_read_back},
        {"the_radio_clock_answers_a_get_with_a_payload", the_radio_clock_answers_a_get_with_a_payload},
        {"a_transmission_that_never_completes_makes_the_session_unhealthy",
         a_transmission_that_never_completes_makes_the_session_unhealthy},
        {"a_timed_transmission_has_its_delay_on_top_of_the_limit",
         a_timed_transmission_has_its_delay_on_top_of_the_limit},
        {"a_timed_transmission_is_not_stuck_before_its_moment", a_timed_transmission_is_not_stuck_before_its_moment},
        {"a_reset_during_a_transmission_drops_its_completion", a_reset_during_a_transmission_drops_its_completion},
        {"a_reset_forgets_the_keys", a_reset_forgets_the_keys},
        {"a_secured_transmission_reaches_the_radio_encrypted_and_reports_its_header",
         a_secured_transmission_reaches_the_radio_encrypted_and_reports_its_header},
        {"a_failed_secured_transmission_carries_no_ack_frame", a_failed_secured_transmission_carries_no_ack_frame},
        {"a_driver_timeout_does_not_bring_down_a_host_with_keys", a_driver_timeout_does_not_bring_down_a_host_with_keys},
        {"a_secured_frame_with_no_key_never_reaches_the_radio", a_secured_frame_with_no_key_never_reaches_the_radio},
        {"a_non_tx_reply_keeps_the_bare_status_shape",
         a_non_tx_reply_keeps_the_bare_status_shape},
        {"the_scan_family_is_answered_on_the_wire", the_scan_family_is_answered_on_the_wire},
        {"insert_and_remove_are_answered_on_the_wire", insert_and_remove_are_answered_on_the_wire},
        {"a_full_source_match_table_answers_nomem_and_keeps_the_old_list",
         a_full_source_match_table_answers_nomem_and_keeps_the_old_list}};

    Tu_Run("session", tests, (int)(sizeof(tests) / sizeof(tests[0])));
    return 0; /* Tu_Run never returns */
}
