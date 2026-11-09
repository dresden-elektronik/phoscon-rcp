/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* test_rcp_app.c — the app glue (app/rcp_app.c) under hostile input, on the fakes of test/fakes/.
 *
 * The property: nothing the host link carries, short of the flasher's reset frame, may cost the board
 * its watchdog. After any input - noise, frames cut or run together, more than the ring holds, every
 * request the session parses, a link that refuses for a while - every pass stays healthy (the pass
 * feeds), every pass leaves interrupts unlocked, and a version request is still answered. */
#include <stdio.h>

#include "app_main.h"
#include "dbg.h"
#include "fakes.h"
#include "hdlc.h"
#include "props.h"
#include "props_stream.h"
#include "rcp_app.h"
#include "spinel.h"
#include "tu.h"
#include "u_bstream.h"

static uint32_t rngState;

static uint32_t rng(void)
{
    rngState = rngState * 1664525u + 1013904223u;
    return rngState >> 8;
}

static uint32_t unhealthyPasses;
static uint32_t lockedPasses;
static uint32_t linkCursor;

static void pass(uint32_t stepMs)
{
    fakeNowMs += stepMs;
    App_Poll(fakeNowMs);

    if (!App_Healthy(fakeNowMs))
        unhealthyPasses++;

    if (fakeIrqDepth != 0 || fakeIrqUnbalanced)
        lockedPasses++;
}

static void boot(uint32_t seed)
{
    uint16_t i;

    rngState = seed;
    unhealthyPasses = 0u;
    lockedPasses = 0u;
    linkCursor = 0u;
    fakeNowMs = 1000u;
    Fake_LinkReset();
    Fake_RadioReset();
    App_Boot();

    for (i = 0u; i < 4u; ++i)
        pass(1u);
}

static uint16_t packUint(uint8_t *dst, uint32_t v)
{
    uint16_t n = 0u;

    do
    {
        uint8_t b = (uint8_t)(v & 0x7Fu);

        v >>= 7;
        dst[n++] = (uint8_t)(v != 0u ? (b | 0x80u) : b);
    } while (v != 0u);

    return n;
}

/* One Spinel frame, HDLC-encoded, as a host writes it. */
static uint16_t wireFrame(uint8_t *dst, uint16_t cap, uint8_t tid, uint8_t cmd, uint32_t prop,
                          const uint8_t *value, uint16_t valueLen)
{
    uint8_t raw[256];
    uint16_t n = 0u;
    uint16_t i;
    U_BStream bs;

    raw[n++] = (uint8_t)(0x80u | (tid & 0x0Fu));
    raw[n++] = cmd;
    n = (uint16_t)(n + packUint(&raw[n], prop));

    for (i = 0u; i < valueLen && n < sizeof(raw); ++i)
        raw[n++] = value[i];

    U_bstream_init(&bs, dst, cap);
    return Hdlc_Encode(raw, n, &bs);
}

static void push(const uint8_t *buf, uint32_t len)
{
    while (len > 0u)
    {
        uint16_t chunk = (uint16_t)(len > 64u ? 64u : len);

        App_DataOut(buf, chunk);
        buf += chunk;
        len -= chunk;
    }
}

static void request(uint8_t tid, uint8_t cmd, uint32_t prop, const uint8_t *value, uint16_t len)
{
    uint8_t wire[HDLC_FRAME_MAX];

    push(wire, wireFrame(wire, sizeof(wire), tid, cmd, prop, value, len));
}

/* 1 when the link has carried VALUE_IS NCP_VERSION for this TID since the last look. */
static int versionAnswered(uint8_t tid)
{
    static Hdlc_Rx rx;
    int seen = 0;

    for (; linkCursor < fakeLinkOutLen; ++linkCursor)
    {
        if (Hdlc_RxByte(&rx, fakeLinkOut[linkCursor]) == HDLC_RX_FRAME && rx.len >= 3u &&
            rx.buf[0] == (uint8_t)(0x80u | tid) && rx.buf[1] == SPINEL_CMD_PROP_VALUE_IS &&
            rx.buf[2] == (uint8_t)PROPS_ID_NCP_VERSION)
            seen = 1;
    }

    if (fakeLinkOutLen > FAKE_LINK_OUT_MAX / 2u)
    {
        /* Keep room in the capture: what was read is gone. */
        Fake_LinkReset();
        linkCursor = 0u;
    }

    return seen;
}

/* A flag first, so whatever came before is a finished frame, then the question; the answer must come
 * within a few passes. */
static int stillAnswers(uint8_t tid)
{
    static const uint8_t flag = 0x7Eu;
    uint16_t i;

    (void)versionAnswered(0u);
    push(&flag, 1u);
    request(tid, SPINEL_CMD_PROP_VALUE_GET, PROPS_ID_NCP_VERSION, 0, 0u);

    for (i = 0u; i < 64u; ++i)
    {
        pass(1u);

        if (versionAnswered(tid))
            return 1;
    }

    return 0;
}

static void enableTransmit(void)
{
    static const uint8_t on = 1u;

    request(1u, SPINEL_CMD_PROP_VALUE_SET, PROPS_ID_PHY_ENABLED, &on, 1u);
    request(2u, SPINEL_CMD_PROP_VALUE_SET, PROPS_ID_MAC_RAW_STREAM, &on, 1u);
    pass(1u);
    pass(1u);
}

TU_TEST(the_link_answers_a_version_request)
{
    boot(1u);
    TU_EQ(stillAnswers(3u), 1);
    TU_EQ(unhealthyPasses, 0u);
    TU_EQ(lockedPasses, 0u);
}

/* Noise in chunks of any size, sometimes with no pass between them. */
TU_TEST(noise_never_silences_the_link)
{
    uint8_t chunk[512];
    uint32_t round;
    uint16_t i;

    boot(2u);

    for (round = 0u; round < 4000u; ++round)
    {
        uint16_t n = (uint16_t)(1u + rng() % sizeof(chunk));

        for (i = 0u; i < n; ++i)
            chunk[i] = (uint8_t)rng();

        push(chunk, n);

        if (rng() % 5u != 0u)
            pass(1u + rng() % 20u);
    }

    TU_EQ(stillAnswers(4u), 1);
    TU_EQ(unhealthyPasses, 0u);
    TU_EQ(lockedPasses, 0u);
}

/* Many times the receive ring without a pass: the oldest is dropped and counted, nothing waits. */
TU_TEST(a_flood_past_the_ring_is_dropped_not_stuck)
{
    uint32_t i;
    uint32_t droppedBefore;

    boot(3u);
    droppedBefore = RcpApp_RxDropped();

    for (i = 0u; i < 2000u; ++i)
        request((uint8_t)(1u + i % 15u), SPINEL_CMD_PROP_VALUE_GET, PROPS_ID_NCP_VERSION, 0, 0u);

    TU_CHECK(RcpApp_RxDropped() > droppedBefore);
    pass(1u);
    TU_EQ(stillAnswers(5u), 1);
    TU_EQ(unhealthyPasses, 0u);
    TU_EQ(lockedPasses, 0u);
}

/* Valid frames cut at any octet and run together, the way a link that loses bytes delivers them. */
TU_TEST(frames_cut_and_run_together_never_silence_the_link)
{
    uint8_t wire[HDLC_FRAME_MAX];
    uint8_t value[140];
    uint32_t round;
    uint16_t i;

    boot(4u);
    enableTransmit();

    for (round = 0u; round < 4000u; ++round)
    {
        uint16_t vlen = (uint16_t)(rng() % sizeof(value));
        uint16_t n;
        uint16_t cut;

        for (i = 0u; i < vlen; ++i)
            value[i] = (uint8_t)rng();

        n = wireFrame(wire, sizeof(wire), (uint8_t)rng(), (uint8_t)(rng() % 8u),
                      (rng() & 1u) ? PROPS_ID_STREAM_RAW : PROPS_ID_NCP_VERSION, value, vlen);
        cut = (uint16_t)(rng() % (uint32_t)(n + 1u));
        push(&wire[(rng() & 1u) ? 0u : cut], (rng() & 1u) ? cut : (uint32_t)(n - cut));

        if (rng() % 3u != 0u)
            pass(1u + rng() % 10u);
    }

    TU_EQ(stillAnswers(6u), 1);
    TU_EQ(unhealthyPasses, 0u);
    TU_EQ(lockedPasses, 0u);
}

/* Every property the core knows, and some it does not, with any command and any value; a third of
 * them transmit requests with any trailer, timed ones included, which the radio holds until their
 * moment. */
TU_TEST(any_request_leaves_the_link_answering)
{
    static const uint32_t props[] = {
        PROPS_ID_LAST_STATUS, PROPS_ID_PROTOCOL_VERSION, PROPS_ID_NCP_VERSION, PROPS_ID_INTERFACE_TYPE,
        PROPS_ID_CAPS, PROPS_ID_RCP_API_VERSION, PROPS_ID_RCP_MIN_HOST_API, PROPS_ID_RCP_LOG_CRASH_DUMP,
        PROPS_ID_RCP_MAC_KEY, PROPS_ID_RCP_MAC_FRAME_COUNTER, PROPS_ID_RCP_ENH_ACK_PROBING,
        PROPS_ID_RCP_TIMESTAMP, PROPS_ID_RCP_CSL_ACCURACY, PROPS_ID_RCP_CSL_UNCERTAINTY,
        PROPS_ID_PHY_ENABLED, PROPS_ID_HWADDR, PROPS_ID_PHY_CHAN, PROPS_ID_PHY_CHAN_SUPPORTED,
        PROPS_ID_PHY_CCA_THRESHOLD, PROPS_ID_PHY_TX_POWER, PROPS_ID_PHY_RSSI, PROPS_ID_PHY_RX_SENSITIVITY,
        PROPS_ID_PHY_CHAN_PREFERRED, PROPS_ID_RADIO_CAPS, PROPS_ID_MAC_LADDR, PROPS_ID_MAC_SADDR,
        PROPS_ID_MAC_PANID, PROPS_ID_MAC_RAW_STREAM, PROPS_ID_MAC_PROMISCUOUS, PROPS_ID_MAC_RX_ON_WHEN_IDLE,
        PROPS_ID_MAC_SRC_MATCH_ENABLED, PROPS_ID_MAC_SRC_MATCH_SHORT, PROPS_ID_MAC_SRC_MATCH_EXT,
        PROPS_ID_MAC_SCAN_STATE, PROPS_ID_MAC_SCAN_MASK, PROPS_ID_MAC_SCAN_PERIOD, PROPS_ID_MAC_SCAN_RESULT};
    uint8_t value[2u + 127u + PROPS_STREAM_TX_TRAILER];
    uint32_t round;
    uint16_t i;

    boot(5u);
    enableTransmit();

    for (round = 0u; round < 20000u; ++round)
    {
        /* Mostly GET, SET, INSERT and REMOVE; now and then RESET or a command nobody defined. */
        uint8_t cmd = (uint8_t)((rng() % 16u == 0u) ? rng() : 2u + rng() % 4u);
        uint32_t prop = (rng() % 8u == 0u) ? rng() % 0x4000u : props[rng() % (sizeof(props) / sizeof(props[0]))];
        uint16_t vlen = (uint16_t)(rng() % 24u);

        if (rng() % 3u == 0u)
        {
            /* A transmit request: a PSDU of any length that parses, then any trailer. */
            uint8_t psduLen = (uint8_t)(3u + rng() % 125u);

            cmd = SPINEL_CMD_PROP_VALUE_SET;
            prop = PROPS_ID_STREAM_RAW;
            value[0] = psduLen;
            value[1] = 0u;
            vlen = (uint16_t)(2u + psduLen + PROPS_STREAM_TX_TRAILER);

            for (i = 2u; i < vlen; ++i)
                value[i] = (uint8_t)rng();

            /* Mostly real channels and frames without the security bit, which no key is loaded for,
             * so most requests reach the radio. */
            if (rng() % 4u != 0u)
            {
                value[2u + psduLen] = (uint8_t)(11u + rng() % 16u);
                value[2u] = (uint8_t)(value[2u] & ~0x08u);
            }

            /* The moment: mostly none, otherwise up to 50 ms either side of the radio's clock. A frame
             * far out holds the radio for its whole lead; a_frame_due_long_after_its_request_is_not_stuck
             * has that case on its own. */
            {
                uint8_t *timing = &value[2u + psduLen + 7u];
                uint32_t baseUs = (uint32_t)fakeNowMs * 1000u - 50000u + rng() % 100000u;
                uint32_t delayUs = (rng() % 4u == 0u) ? 1u + rng() % 50000u : 0u;

                for (i = 0u; i < 4u; ++i)
                {
                    timing[i] = (uint8_t)(delayUs >> (8u * i));
                    timing[4u + i] = (uint8_t)(baseUs >> (8u * i));
                }
            }
        }
        else
        {
            for (i = 0u; i < vlen; ++i)
                value[i] = (uint8_t)rng();
        }

        request((uint8_t)rng(), cmd, prop, value, vlen);
        pass(1u + rng() % 40u);

        /* The fuzz turns the radio off as often as anything else does; turn it back on now and then. */
        if (round % 16u == 15u)
            enableTransmit();

        if (round % 1000u == 999u)
            TU_EQ(stillAnswers((uint8_t)(1u + round % 15u)), 1);
    }

    TU_NOTE("transmissions the radio took", fakeRadioTxCount);
    TU_CHECK(fakeRadioTxCount > 100u);
    TU_EQ(stillAnswers(7u), 1);
    TU_EQ(unhealthyPasses, 0u);
    TU_EQ(lockedPasses, 0u);
}

/* A timed frame whose base lies far ahead of the radio's clock: the radio holds it for the whole lead,
 * every transmit request meanwhile is answered BUSY, and no pass in that time may starve the watchdog. */
TU_TEST(a_frame_due_long_after_its_request_is_not_stuck)
{
    uint8_t value[2u + 5u + PROPS_STREAM_TX_TRAILER];
    const uint32_t leadUs = 600000000uL; /* 10 min */
    uint32_t dueUs;
    uint32_t sent;
    uint32_t i;

    boot(7u);
    enableTransmit();

    for (i = 0u; i < sizeof(value); ++i)
        value[i] = 0u;

    value[0] = 5u;
    value[2] = 0x01u;                  /* a data frame, no Ack requested */
    value[2u + 5u] = 15u;              /* channel */
    value[2u + 5u + 7u] = 1u;          /* txDelay: 1 us */
    dueUs = fakeNowMs * 1000u + leadUs - 1u;

    for (i = 0u; i < 4u; ++i)
        value[2u + 5u + 11u + i] = (uint8_t)(dueUs >> (8u * i)); /* txDelayBase */

    sent = fakeRadioTxCount;
    request(9u, SPINEL_CMD_PROP_VALUE_SET, PROPS_ID_STREAM_RAW, value, (uint16_t)sizeof(value));

    for (i = 0u; i < 6100u; ++i)
        pass(100u); /* 10 min 10 s */

    TU_EQ(fakeRadioTxCount, sent + 1u);
    TU_EQ(fakeRadioLeftOpen, 0u);
    TU_EQ(stillAnswers(10u), 1);
    TU_EQ(unhealthyPasses, 0u);
    TU_EQ(lockedPasses, 0u);
}

/* One transmit request: a beacon request (no Ack asked) on txChannel, then receive on rxAfter. */
static void scanStep(uint8_t tid, uint8_t txChannel, uint8_t rxAfter)
{
    static const uint8_t beaconRequest[] = {0x03u, 0x08u, 0x00u, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0x07u, 0x00u, 0x00u};
    uint8_t value[2u + sizeof(beaconRequest) + PROPS_STREAM_TX_TRAILER];
    uint16_t off = (uint16_t)(2u + sizeof(beaconRequest));
    uint16_t i;

    for (i = 0u; i < sizeof(value); ++i)
        value[i] = 0u;

    value[0] = (uint8_t)sizeof(beaconRequest);

    for (i = 0u; i < sizeof(beaconRequest); ++i)
        value[2u + i] = beaconRequest[i];

    value[2u] = beaconRequest[0];
    value[off] = txChannel;
    value[off + 3u] = 1u;          /* CSMA-CA */
    value[off + 15u] = rxAfter;
    request(tid, SPINEL_CMD_PROP_VALUE_SET, PROPS_ID_STREAM_RAW, value, (uint16_t)sizeof(value));

    for (i = 0u; i < 20u; ++i)
        pass(1u);
}

/* An OpenThread host scans as its RCP protocol has it: the frame names its own channel, the receiver
 * goes to rxChannelAfterTxDone, and PHY_CHAN is written only when the host's idea of the channel
 * differs (radio_spinel.cpp: mChannel = the frame's channel after Transmit). The beacon request must go
 * out on the scan channel and the radio must then listen there, not on PHY_CHAN. */
TU_TEST(a_frame_goes_out_on_its_own_channel_and_the_receiver_stays_where_it_was_sent)
{
    static const uint8_t pan = 15u;

    boot(8u);
    enableTransmit();
    request(3u, SPINEL_CMD_PROP_VALUE_SET, PROPS_ID_PHY_CHAN, &pan, 1u);
    pass(1u);
    pass(1u);
    TU_EQ(fakeRadioChannel, 15u);

    scanStep(4u, 11u, 11u);
    TU_EQ(fakeRadioTxChannel, 11u);
    TU_EQ(fakeRadioChannel, 11u);

    scanStep(5u, 12u, 12u);
    TU_EQ(fakeRadioTxChannel, 12u);
    TU_EQ(fakeRadioChannel, 12u);

    /* The scan is over: the host's channel is 12, not 15, so it writes PHY_CHAN 15 - the value the
     * property already holds - and the radio must go back. */
    request(6u, SPINEL_CMD_PROP_VALUE_SET, PROPS_ID_PHY_CHAN, &pan, 1u);
    pass(1u);
    pass(1u);
    TU_EQ(fakeRadioChannel, 15u);

    /* A frame that keeps the channel (txChannel 0) goes out where the radio is. */
    scanStep(7u, 0u, 0u);
    TU_EQ(fakeRadioTxChannel, 15u);
    TU_EQ(fakeRadioChannel, 15u);
    TU_EQ(unhealthyPasses, 0u);
}

/* A RESET empties the source-match table, and the radio must get the empty table: what it holds
 * decides the Frame Pending bit of every Ack to a poll. Whatever the table went through before -
 * here 256 changes, each handed down, so the radio's copy sits at a count that wraps to the reset's. */
TU_TEST(after_a_reset_the_radio_holds_the_empty_source_match_table)
{
    static const uint8_t reset[] = {0x02u};
    static const uint8_t child[2] = {0x34u, 0x12u};
    uint8_t on;
    uint16_t i;

    boot(9u);
    enableTransmit();
    request(1u, SPINEL_CMD_PROP_VALUE_SET, PROPS_ID_MAC_SRC_MATCH_SHORT, child, sizeof(child));
    pass(1u);

    for (i = 0u; i < 255u; ++i)
    {
        on = (uint8_t)(i & 1u);
        request((uint8_t)(1u + i % 15u), SPINEL_CMD_PROP_VALUE_SET, PROPS_ID_MAC_SRC_MATCH_ENABLED, &on, 1u);
        pass(1u);
    }

    TU_EQ(fakeRadioSrcMatchShorts, 1u);
    TU_EQ(fakeRadioSrcMatchEnabled, 0u);

    request(2u, SPINEL_CMD_RESET, 0u, reset, sizeof(reset));
    pass(1u);
    pass(1u);

    /* The power-on table: off and empty. */
    TU_EQ(fakeRadioSrcMatchShorts, 0u);
    TU_EQ(fakeRadioSrcMatchEnabled, 0u);
}

static uint32_t counterRecords;

static void countCounters(uint16_t component, uint16_t line, uint16_t reason)
{
    (void)line;

    if (component == DBG_COMP_APP && reason == DBG_APP_RSN_FRAMES_RX)
        counterRecords++;
}

/* The tick wraps after 49.7 days, and a deadline must hold across it (docs/architecture.md §7): with the
 * record stream on, the counters go out once every 5 s, not on every pass of the seconds before the
 * wrap. 30 s of 100 ms passes from 3 s before it. */
TU_TEST(the_counters_keep_their_period_across_the_tick_wrap)
{
    static const uint8_t on = 1u;
    uint16_t i;

    boot(10u);
    request(3u, SPINEL_CMD_PROP_VALUE_SET, PROPS_ID_RCP_LOG_CRASH_DUMP, &on, 1u);
    pass(1u);

    /* Toward the wrap as a running tick gets there: no step longer than the 24.8 days a modular deadline
     * can see back, each with a pass that takes it. */
    while (fakeNowMs < 0xFFFFFFFFuL - 3000u - 0x40000000uL)
        pass(0x40000000uL);

    /* The counters go out 3 s before the wrap, so their next deadline is past it. */
    counterRecords = 0u;
    Debug_SetSink(countCounters);
    fakeNowMs = 0xFFFFFFFFuL - 3000u - 1u;
    pass(1u);
    TU_EQ(counterRecords, 1u);

    for (i = 0u; i < 300u; ++i)
        pass(100u);

    Debug_SetSink(0);
    TU_NOTE("counter records in 30 s across the wrap", counterRecords);
    TU_CHECK(counterRecords >= 6u);
    TU_CHECK(counterRecords <= 8u);
}

/* A link that refuses every write for a while: replies that do not fit are dropped, nothing waits on
 * the link, and it answers again once the link takes writes. */
TU_TEST(a_link_that_refuses_for_a_while_answers_again)
{
    uint32_t i;

    boot(6u);
    fakeLinkRefuse = 1;

    for (i = 0u; i < 3000u; ++i)
    {
        request((uint8_t)(1u + i % 15u), SPINEL_CMD_PROP_VALUE_GET, PROPS_ID_NCP_VERSION, 0, 0u);
        pass(1u);
    }

    TU_CHECK(RcpApp_TxDropped() > 0u);
    fakeLinkRefuse = 0;

    for (i = 0u; i < 64u; ++i)
        pass(1u);

    TU_EQ(stillAnswers(8u), 1);
    TU_EQ(unhealthyPasses, 0u);
    TU_EQ(lockedPasses, 0u);
}

int main(void)
{
    static const Tu_Test tests[] = {
        {"the_link_answers_a_version_request", the_link_answers_a_version_request},
        {"noise_never_silences_the_link", noise_never_silences_the_link},
        {"a_flood_past_the_ring_is_dropped_not_stuck", a_flood_past_the_ring_is_dropped_not_stuck},
        {"frames_cut_and_run_together_never_silence_the_link", frames_cut_and_run_together_never_silence_the_link},
        {"any_request_leaves_the_link_answering", any_request_leaves_the_link_answering},
        {"a_frame_due_long_after_its_request_is_not_stuck", a_frame_due_long_after_its_request_is_not_stuck},
        {"a_frame_goes_out_on_its_own_channel_and_the_receiver_stays_where_it_was_sent",
         a_frame_goes_out_on_its_own_channel_and_the_receiver_stays_where_it_was_sent},
        {"after_a_reset_the_radio_holds_the_empty_source_match_table",
         after_a_reset_the_radio_holds_the_empty_source_match_table},
        {"the_counters_keep_their_period_across_the_tick_wrap", the_counters_keep_their_period_across_the_tick_wrap},
        {"a_link_that_refuses_for_a_while_answers_again", a_link_that_refuses_for_a_while_answers_again}};

    Tu_Run("rcp_app", tests, (int)(sizeof(tests) / sizeof(tests[0])));
    return 0; /* Tu_Run never returns */
}
