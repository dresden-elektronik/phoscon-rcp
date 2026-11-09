/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* test_mac_sec.c — securing a frame for an OpenThread host (src/rcp/mac_sec.h). The expected frames
 * were computed with an independent AES-CCM implementation, not with this firmware's own. */
#include <stdint.h>

#include "mac_sec.h"
#include "tu.h"

static const uint8_t ext8[MAC_SEC_EXT_LEN] = {0xAAu, 0xBBu, 0xCCu, 0xDDu, 0xEEu, 0xFFu, 0x00u, 0x11u};

#define COUNTER 0x00010203uL
#define KEY_INDEX 5u
#define FRAME_MAX 64u

/* Version 2006 data, short destination, extended source, PAN ID compressed. */
static const uint8_t data2006Clear[] = {0x69, 0xD8, 0x11, 0xCE, 0xFA, 0x34, 0x12, 0x11, 0x00, 0xFF, 0xEE, 0xDD,
                                        0xCC, 0xBB, 0xAA, 0x0D, 0x03, 0x02, 0x01, 0x00, 0x05, 0xDE, 0xAD, 0xBE,
                                        0xEF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t data2006Want[] = {0x69, 0xD8, 0x11, 0xCE, 0xFA, 0x34, 0x12, 0x11, 0x00, 0xFF, 0xEE, 0xDD,
                                       0xCC, 0xBB, 0xAA, 0x0D, 0x03, 0x02, 0x01, 0x00, 0x05, 0x50, 0x85, 0x1F,
                                       0xFA, 0x34, 0x7A, 0x22, 0xCC, 0x00, 0x00};

/* Version 2006 Data Request: the command identifier is authenticated header, left in the clear. */
static const uint8_t poll2006Clear[] = {0x6B, 0xD8, 0x12, 0xCE, 0xFA, 0x34, 0x12, 0x11, 0x00, 0xFF, 0xEE,
                                        0xDD, 0xCC, 0xBB, 0xAA, 0x0D, 0x03, 0x02, 0x01, 0x00, 0x05, 0x04,
                                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t poll2006Want[] = {0x6B, 0xD8, 0x12, 0xCE, 0xFA, 0x34, 0x12, 0x11, 0x00, 0xFF, 0xEE,
                                       0xDD, 0xCC, 0xBB, 0xAA, 0x0D, 0x03, 0x02, 0x01, 0x00, 0x05, 0x04,
                                       0xBE, 0xA3, 0xB5, 0x9A, 0x00, 0x00};

/* Version 2015 data: both addresses short, compressed, so one PAN ID; a CSL IE and Termination 2. */
static const uint8_t data2015IeClear[] = {0x69, 0xAA, 0x13, 0xCE, 0xFA, 0x34, 0x12, 0x78, 0x56, 0x0D, 0x03, 0x02,
                                          0x01, 0x00, 0x05, 0x04, 0x0D, 0x01, 0x02, 0x03, 0x04, 0x80, 0x3F, 0x01,
                                          0x02, 0x03, 0x04, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t data2015IeWant[] = {0x69, 0xAA, 0x13, 0xCE, 0xFA, 0x34, 0x12, 0x78, 0x56, 0x0D, 0x03, 0x02,
                                         0x01, 0x00, 0x05, 0x04, 0x0D, 0x01, 0x02, 0x03, 0x04, 0x80, 0x3F, 0x8F,
                                         0x2A, 0xA2, 0x11, 0xAB, 0x58, 0xD2, 0xB8, 0xC9, 0x00, 0x00};

/* Version 2015 Data Request: the command identifier is encrypted. */
static const uint8_t poll2015Clear[] = {0x6B, 0xA8, 0x14, 0xCE, 0xFA, 0x34, 0x12, 0x78, 0x56, 0x0D, 0x03,
                                        0x02, 0x01, 0x00, 0x05, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t poll2015Want[] = {0x6B, 0xA8, 0x14, 0xCE, 0xFA, 0x34, 0x12, 0x78, 0x56, 0x0D, 0x03,
                                       0x02, 0x01, 0x00, 0x05, 0x8A, 0xB2, 0x8B, 0x21, 0x34, 0x00, 0x00};

static MacSec_State state;
static uint8_t frame[FRAME_MAX];

static void setKeysAt(uint8_t keyIndex)
{
    uint8_t value[56];
    uint8_t k;

    value[0] = 1u << 3;
    value[1] = keyIndex;

    for (k = 0u; k < 3u; ++k)
    {
        uint8_t i;

        value[2u + k * 18u] = 16u;
        value[3u + k * 18u] = 0u;

        for (i = 0u; i < 16u; ++i)
            value[4u + k * 18u + i] = (k == 1u) ? i : (uint8_t)(k == 0u ? 0x11u : 0x22u);
    }

    TU_EQ(MacSec_SetKeys(&state, value, sizeof(value)), 1);
}

static void counterIs(uint32_t counter)
{
    uint8_t value[4];

    value[0] = (uint8_t)(counter & 0xFFu);
    value[1] = (uint8_t)((counter >> 8) & 0xFFu);
    value[2] = (uint8_t)((counter >> 16) & 0xFFu);
    value[3] = (uint8_t)((counter >> 24) & 0xFFu);
    TU_EQ(MacSec_SetFrameCounter(&state, value, 4u), 1);
}

static void load(const uint8_t *src, uint8_t len)
{
    uint8_t i;

    for (i = 0u; i < len; ++i)
        frame[i] = src[i];
}

static int matches(const uint8_t *want, uint8_t len)
{
    uint8_t i;

    for (i = 0u; i < len; ++i)
    {
        if (frame[i] != want[i])
            return 0;
    }

    return 1;
}

static void freshState(void)
{
    MacSec_Init(&state);
    setKeysAt(KEY_INDEX);
    counterIs(COUNTER);
}

static void securedAs(const uint8_t *clear, const uint8_t *want, uint8_t len)
{
    MacSec_TxResult r;

    freshState();
    load(clear, len);
    TU_EQ(MacSec_SecureTx(&state, frame, len, ext8, 1, 0, &r), 1);
    TU_CHECK(matches(want, len));
    TU_EQ(r.secured, 1);
    TU_EQ(r.headerUpdated, 1);
    TU_EQ(r.keyIndex, KEY_INDEX);
    TU_EQ(r.frameCounter, COUNTER);
}

TU_TEST(a_data_frame_encrypts_its_payload_and_authenticates_its_header)
{
    securedAs(data2006Clear, data2006Want, sizeof(data2006Clear));
}

TU_TEST(a_version_2006_poll_keeps_its_command_identifier_in_the_clear)
{
    securedAs(poll2006Clear, poll2006Want, sizeof(poll2006Clear));
}

TU_TEST(a_version_2015_frame_authenticates_its_header_ies)
{
    securedAs(data2015IeClear, data2015IeWant, sizeof(data2015IeClear));
}

TU_TEST(a_version_2015_poll_encrypts_its_command_identifier)
{
    securedAs(poll2015Clear, poll2015Want, sizeof(poll2015Clear));
}

TU_TEST(the_layout_finds_each_shape)
{
    MacSec_Layout l;

    TU_EQ(MacSec_ParseLayout(data2006Clear, sizeof(data2006Clear), &l), 1);
    TU_EQ(l.auxOffset, 15u);
    TU_EQ(l.adataLen, 21u);
    TU_EQ(l.payloadLen, 4u);
    TU_EQ(l.micLen, 4u);
    TU_EQ(l.level, 5u);
    TU_EQ(l.keyIdMode, 1u);

    TU_EQ(MacSec_ParseLayout(poll2006Clear, sizeof(poll2006Clear), &l), 1);
    TU_EQ(l.adataLen, 22u);
    TU_EQ(l.payloadLen, 0u);

    TU_EQ(MacSec_ParseLayout(data2015IeClear, sizeof(data2015IeClear), &l), 1);
    TU_EQ(l.auxOffset, 9u);
    TU_EQ(l.adataLen, 23u);
    TU_EQ(l.payloadLen, 5u);

    TU_EQ(MacSec_ParseLayout(poll2015Clear, sizeof(poll2015Clear), &l), 1);
    TU_EQ(l.adataLen, 15u);
    TU_EQ(l.payloadLen, 1u);
}

/* IEEE 802.15.4-2015 Table 7-2: where the Security Control octet lands tells which PAN IDs exist. */
TU_TEST(version_2015_pan_id_presence_follows_the_table)
{
    static const struct
    {
        uint16_t fcf;
        uint8_t auxOffset;
    } cases[] = {
        {0x2009u, 3u},  /* no addresses, not compressed: no PAN ID */
        {0x2049u, 5u},  /* no addresses, compressed: destination PAN ID */
        {0x2809u, 7u},  /* short destination only, not compressed: destination PAN ID */
        {0x2849u, 5u},  /* short destination only, compressed: none */
        {0xA009u, 7u},  /* short source only, not compressed: source PAN ID */
        {0xA049u, 5u},  /* short source only, compressed: none */
        {0xA809u, 11u}, /* short both, not compressed: both */
        {0xA849u, 9u},  /* short both, compressed: destination only */
        {0xEC09u, 21u}, /* extended both, not compressed: destination only */
        {0xEC49u, 19u}, /* extended both, compressed: none */
        {0xE809u, 17u}, /* short destination, extended source: both */
        {0xE849u, 15u}, /* the same, compressed: destination only */
    };
    uint8_t buf[FRAME_MAX];
    uint8_t c;
    uint8_t i;

    for (i = 0u; i < FRAME_MAX; ++i)
        buf[i] = 0u;

    for (c = 0u; c < (uint8_t)(sizeof(cases) / sizeof(cases[0])); ++c)
    {
        MacSec_Layout l;

        buf[0] = (uint8_t)(cases[c].fcf & 0xFFu);
        buf[1] = (uint8_t)(cases[c].fcf >> 8);
        buf[cases[c].auxOffset] = 0x0Du;
        TU_EQ(MacSec_ParseLayout(buf, FRAME_MAX, &l), 1);
        TU_EQ(l.auxOffset, cases[c].auxOffset);
        TU_EQ(l.level, 5u);
        buf[cases[c].auxOffset] = 0u;
    }
}

TU_TEST(the_frame_counter_is_written_in_and_moves_on)
{
    MacSec_TxResult r;
    uint8_t k;

    freshState();
    counterIs(0x11223344uL);
    load(data2006Clear, sizeof(data2006Clear));

    for (k = 0u; k < 5u; ++k)
        frame[16u + k] = 0u; /* counter and key index: the firmware's to fill */

    TU_EQ(MacSec_SecureTx(&state, frame, sizeof(data2006Clear), ext8, 0, 0, &r), 1);
    TU_EQ(frame[16], 0x44u);
    TU_EQ(frame[19], 0x11u);
    TU_EQ(frame[20], KEY_INDEX);
    TU_EQ(r.headerUpdated, 1);
    TU_EQ(r.frameCounter, 0x11223344uL);
    TU_EQ(r.keyIndex, KEY_INDEX);
    TU_EQ(state.frameCounter, 0x11223345uL);
}

/* A retransmission: the same counter, the same bytes on the air, and the counter does not move. */
TU_TEST(an_updated_header_is_kept_and_the_counter_does_not_move)
{
    MacSec_TxResult r;

    freshState();
    counterIs(0x99u);
    load(data2006Clear, sizeof(data2006Clear));
    TU_EQ(MacSec_SecureTx(&state, frame, sizeof(data2006Clear), ext8, 1, 0, &r), 1);
    TU_CHECK(matches(data2006Want, sizeof(data2006Want)));
    TU_EQ(state.frameCounter, 0x99u);
    TU_EQ(r.frameCounter, COUNTER);
}

TU_TEST(nothing_goes_out_marked_secured_but_unencrypted)
{
    MacSec_TxResult r;

    /* No key from the host yet. */
    MacSec_Init(&state);
    load(data2006Clear, sizeof(data2006Clear));
    TU_EQ(MacSec_SecureTx(&state, frame, sizeof(data2006Clear), ext8, 1, 0, &r), 0);

    /* Key ID mode 2, which the host must have secured itself. */
    freshState();
    load(data2006Clear, sizeof(data2006Clear));
    frame[15] = 0x15u;
    TU_EQ(MacSec_SecureTx(&state, frame, sizeof(data2006Clear), ext8, 1, 0, &r), 0);

    /* A level that does not encrypt the way a Thread frame must. */
    load(data2006Clear, sizeof(data2006Clear));
    frame[15] = 0x09u;
    TU_EQ(MacSec_SecureTx(&state, frame, sizeof(data2006Clear), ext8, 1, 0, &r), 0);

    /* A frame too short to hold its own auxiliary header and MIC. */
    load(data2006Clear, sizeof(data2006Clear));
    TU_EQ(MacSec_SecureTx(&state, frame, 18u, ext8, 1, 0, &r), 0);

    /* A counter with nowhere left to go is not reused. */
    freshState();
    counterIs(0xFFFFFFFFuL);
    load(data2006Clear, sizeof(data2006Clear));
    TU_EQ(MacSec_SecureTx(&state, frame, sizeof(data2006Clear), ext8, 0, 0, &r), 0);
    TU_EQ(state.frameCounter, 0xFFFFFFFFuL);
}

TU_TEST(a_frame_the_host_secured_or_left_plain_passes_untouched)
{
    MacSec_TxResult r;

    MacSec_Init(&state); /* a Zigbee host: no key, ever */
    load(data2006Clear, sizeof(data2006Clear));
    frame[0] = (uint8_t)(frame[0] & ~0x08u);
    TU_EQ(MacSec_SecureTx(&state, frame, sizeof(data2006Clear), ext8, 0, 0, &r), 1);
    TU_EQ(r.secured, 0);
    TU_EQ(r.headerUpdated, 0);
    TU_EQ(frame[21], 0xDEu);

    load(data2006Clear, sizeof(data2006Clear));
    frame[15] = 0x15u; /* key ID mode 2 */
    TU_EQ(MacSec_SecureTx(&state, frame, sizeof(data2006Clear), ext8, 1, 1, &r), 1);
    TU_EQ(r.secured, 1);
    TU_EQ(r.headerUpdated, 1);
    TU_EQ(frame[21], 0xDEu);
}

/* The neighbouring key indexes pick the neighbouring keys, and 128 wraps to 1. */
TU_TEST(the_frames_key_index_selects_the_key)
{
    static const struct
    {
        uint8_t current;
        uint8_t inFrame;
    } cases[] = {{5u, 4u}, {5u, 6u}, {1u, 128u}, {128u, 1u}};
    uint8_t c;

    for (c = 0u; c < (uint8_t)(sizeof(cases) / sizeof(cases[0])); ++c)
    {
        MacSec_TxResult r;

        MacSec_Init(&state);
        setKeysAt(cases[c].current);
        load(data2006Clear, sizeof(data2006Clear));
        frame[20] = cases[c].inFrame;
        TU_EQ(MacSec_SecureTx(&state, frame, sizeof(data2006Clear), ext8, 1, 0, &r), 1);
        TU_EQ(r.keyIndex, cases[c].inFrame);
        TU_CHECK(frame[21] != data2006Want[21] || frame[22] != data2006Want[22]);
    }

    /* An index that is none of the three falls back to the current key, as upstream's does. */
    MacSec_TxResult r;

    MacSec_Init(&state);
    setKeysAt(5u);
    load(data2006Clear, sizeof(data2006Clear));
    frame[20] = 5u;
    TU_EQ(MacSec_SecureTx(&state, frame, sizeof(data2006Clear), ext8, 1, 0, &r), 1);
    TU_CHECK(matches(data2006Want, sizeof(data2006Want)));
}

TU_TEST(the_counter_property_honours_set_if_larger)
{
    uint8_t value[5] = {0x10u, 0u, 0u, 0u, 1u};

    MacSec_Init(&state);
    counterIs(0x20u);
    TU_EQ(MacSec_SetFrameCounter(&state, value, 5u), 1);
    TU_EQ(state.frameCounter, 0x20u); /* smaller: stays */
    value[0] = 0x30u;
    TU_EQ(MacSec_SetFrameCounter(&state, value, 5u), 1);
    TU_EQ(state.frameCounter, 0x30u);
    value[0] = 0x05u;
    value[4] = 0u;
    TU_EQ(MacSec_SetFrameCounter(&state, value, 5u), 1);
    TU_EQ(state.frameCounter, 0x05u); /* plain set: anything goes */
    TU_EQ(MacSec_SetFrameCounter(&state, value, 3u), 0);
}

TU_TEST(a_malformed_key_value_is_refused_and_changes_nothing)
{
    uint8_t value[56];
    uint8_t i;

    MacSec_Init(&state);

    for (i = 0u; i < sizeof(value); ++i)
        value[i] = 0u;

    TU_EQ(MacSec_SetKeys(&state, value, 55u), 0);
    TU_EQ(MacSec_SetKeys(&state, value, 56u), 0); /* the length fields say zero */
    TU_EQ(state.hasKeys, 0u);
}

TU_TEST(init_forgets_the_keys)
{
    freshState();
    TU_EQ(state.hasKeys, 1u);
    MacSec_Init(&state);
    TU_EQ(state.hasKeys, 0u);
    TU_EQ(state.frameCounter, 0u);
    TU_EQ(state.curr.key[1], 0u);
    TU_EQ(state.curr.roundKey[20], 0u);
}

/* The lock the glue installs, recorded: how often it is taken, and the state as the unlock finds it. */
static int lockDepth;
static int lockCalls;
static MacSec_State seenAtUnlock;

static void recordLock(void)
{
    lockDepth++;
    lockCalls++;
}

static void recordUnlock(void)
{
    seenAtUnlock = state;
    lockDepth--;
}

/* A RESET runs MacSec_Init while the radio interrupt may be building an Enhanced Ack from the same
 * state. Whatever that Ack reads must change inside the lock, so it sees the keys or no keys, never a
 * state half cleared: hasKeys, the key index, both counters, and the probe table, which the interrupt
 * reads without looking at hasKeys. The keys themselves may go afterwards, with nothing left to gate. */
TU_TEST(init_clears_what_an_ack_reads_under_the_lock)
{
    static const uint8_t probe[2u + MAC_SEC_EXT_LEN + 1u] = {0x34u, 0x12u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u,
                                                             MAC_SEC_PROBE_LQI};
    uint8_t i;

    freshState();
    TU_EQ(MacSec_SetProbe(&state, probe, sizeof(probe)), 1);
    TU_EQ(state.hasKeys, 1u);

    {
        uint8_t *p = (uint8_t *)&seenAtUnlock;
        uint16_t k;

        for (k = 0u; k < sizeof(seenAtUnlock); ++k)
            p[k] = 0xFFu; /* so a lock never released cannot pass for a clean one */
    }

    lockCalls = 0;
    MacSec_SetLock(recordLock, recordUnlock);
    MacSec_Init(&state);
    MacSec_SetLock(0, 0);

    TU_EQ(lockCalls, 1);
    TU_EQ(lockDepth, 0);
    TU_EQ(seenAtUnlock.hasKeys, 0u);
    TU_EQ(seenAtUnlock.keyIndex, 0u);
    TU_EQ(seenAtUnlock.frameCounter, 0u);
    TU_EQ(seenAtUnlock.prevFrameCounter, 0u);

    for (i = 0u; i < MAC_SEC_PROBE_MAX; ++i)
        TU_EQ(seenAtUnlock.probes[i].metrics, 0u);

    /* And once it returns, nothing of the keys is left. */
    TU_EQ(state.curr.key[1], 0u);
    TU_EQ(state.next.roundKey[20], 0u);
    TU_EQ(state.probes[0].shortAddr, 0u);
}

/* What each frame's Ack must be: version 2, secured, the frame's source as its destination. The expected
 * bytes come from an independent AES-CCM, as above. */
static uint8_t ack[MAC_SEC_ACK_MAX];

/* Secured version 2 data as far as its auxiliary header: what RAIL has by its early event. */
static const uint8_t arriving[] = {0x69, 0xAA, 0x13, 0xCE, 0xFA, 0x34, 0x12, 0x78, 0x56,
                                   0x0D, 0x09, 0x08, 0x07, 0x06, 0x05};
static const uint8_t ackShort[] = {0x0A, 0x28, 0x13, 0xCE, 0xFA, 0x78, 0x56, 0x0D, 0x03, 0x02,
                                   0x01, 0x00, 0x05, 0x96, 0x55, 0xD4, 0x14, 0x00, 0x00};
static const uint8_t ackShortFp[] = {0x1A, 0x28, 0x13, 0xCE, 0xFA, 0x78, 0x56, 0x0D, 0x03, 0x02,
                                     0x01, 0x00, 0x05, 0xE5, 0xF8, 0x6E, 0xAF, 0x00, 0x00};
/* A short destination and an extended source, PAN ID compressed: one PAN ID. */
static const uint8_t arrivingExt[] = {0x69, 0xE8, 0x14, 0xCE, 0xFA, 0x34, 0x12, 0x11, 0x00, 0xFF, 0xEE,
                                      0xDD, 0xCC, 0xBB, 0xAA, 0x0D, 0x09, 0x08, 0x07, 0x06, 0x05};
static const uint8_t ackExt[] = {0x0A, 0x2C, 0x14, 0xCE, 0xFA, 0x11, 0x00, 0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA,
                                 0x0D, 0x03, 0x02, 0x01, 0x00, 0x05, 0x1A, 0xD6, 0xBA, 0x19, 0x00, 0x00};
static const uint8_t ackPrev[] = {0x0A, 0x28, 0x13, 0xCE, 0xFA, 0x78, 0x56, 0x0D, 0x00, 0x01,
                                  0x00, 0x00, 0x04, 0x11, 0x78, 0x97, 0x36, 0x00, 0x00};
static const uint8_t ackNext[] = {0x0A, 0x28, 0x13, 0xCE, 0xFA, 0x78, 0x56, 0x0D, 0x00, 0x00,
                                  0x00, 0x00, 0x06, 0xB9, 0xC1, 0x47, 0x8E, 0x00, 0x00};

static int ackIs(const uint8_t *want, uint8_t len)
{
    uint8_t i;

    for (i = 0u; i < len; ++i)
    {
        if (ack[i] != want[i])
            return 0;
    }

    return 1;
}

TU_TEST(a_secured_frame_gets_a_secured_enhanced_ack)
{
    MacSec_AckResult r;

    freshState();
    TU_EQ(MacSec_BuildEnhAck(&state, arriving, sizeof(arriving), ext8, 0, 0, 0u, ack, &r), sizeof(ackShort));
    TU_CHECK(ackIs(ackShort, sizeof(ackShort)));
    TU_EQ(r.secured, 1);
    TU_EQ(r.keyIndex, KEY_INDEX);
    TU_EQ(r.frameCounter, COUNTER);
    TU_EQ(state.frameCounter, COUNTER + 1u);
}

TU_TEST(the_ack_in_two_steps_is_the_same_ack)
{
    MacSec_AckResult r;
    MacSec_AckMic later;
    uint8_t n;
    uint8_t i;

    freshState();
    n = MacSec_BuildEnhAckHeader(&state, arriving, sizeof(arriving), ext8, 0, 0, 0u, ack, &r, &later);
    TU_EQ(n, sizeof(ackShort));
    TU_EQ(later.adataLen, sizeof(ackShort) - 6u);
    TU_EQ(r.frameCounter, COUNTER);

    /* What goes on air first is everything but the MIC, and no block has been spent on it yet. */
    for (i = 0u; i < later.adataLen; ++i)
        TU_EQ(ack[i], ackShort[i]);

    for (i = later.adataLen; i < n; ++i)
        TU_EQ(ack[i], 0u);

    TU_EQ(MacSec_EnhAckMic(&later, ack, &ack[later.adataLen]), 1);
    TU_CHECK(ackIs(ackShort, sizeof(ackShort)));
}

TU_TEST(an_unsecured_frames_ack_has_no_mic_to_wait_for)
{
    static const uint8_t plain[] = {0x61u, 0xA8u, 0x07u, 0xEFu, 0xBEu, 0x01u, 0x00u, 0x02u, 0x00u, 0xAAu, 0x00u, 0x00u};
    MacSec_AckResult r;
    MacSec_AckMic later;

    freshState();
    TU_CHECK(MacSec_BuildEnhAckHeader(&state, plain, sizeof(plain), ext8, 0, 0, 0u, ack, &r, &later) != 0u);
    TU_EQ(later.adataLen, 0u);
    TU_EQ(r.secured, 0);
}

TU_TEST(the_ack_pending_bit_follows_the_table)
{
    SrcMatch_Table table;
    MacSec_AckRx rx;
    static const uint8_t ext[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};

    SrcMatch_Init(&table);
    rx.version = 2u;
    rx.srcMode = 2u;
    rx.srcShort = 0x1234u;
    rx.srcExt = 0;

    /* A table that is off cannot say: a poll hears pending, a data frame nothing. */
    table.enabled = 0;
    rx.type = 3u;
    TU_EQ(MacSec_AckPending(&rx, &table), 1);
    rx.type = 1u;
    TU_EQ(MacSec_AckPending(&rx, &table), 0);

    table.enabled = 1;
    TU_EQ(MacSec_AckPending(&rx, &table), 0);
    TU_EQ(SrcMatch_InsertShort(&table, 0x1234u), SRCMATCH_OK);
    TU_EQ(MacSec_AckPending(&rx, &table), 1);

    rx.srcMode = 3u;
    rx.srcExt = ext;
    TU_EQ(MacSec_AckPending(&rx, &table), 0);
    TU_EQ(SrcMatch_InsertExt(&table, ext), SRCMATCH_OK);
    TU_EQ(MacSec_AckPending(&rx, &table), 1);

    rx.srcMode = 0u;
    TU_EQ(MacSec_AckPending(&rx, &table), 0);
}

TU_TEST(frame_pending_is_in_the_acks_frame_control)
{
    MacSec_AckResult r;

    freshState();
    TU_EQ(MacSec_BuildEnhAck(&state, arriving, sizeof(arriving), ext8, 1, 0, 0u, ack, &r), sizeof(ackShortFp));
    TU_CHECK(ackIs(ackShortFp, sizeof(ackShortFp)));
}

TU_TEST(an_extended_source_is_the_acks_destination)
{
    MacSec_AckResult r;

    freshState();
    TU_EQ(MacSec_BuildEnhAck(&state, arrivingExt, sizeof(arrivingExt), ext8, 0, 0, 0u, ack, &r), sizeof(ackExt));
    TU_CHECK(ackIs(ackExt, sizeof(ackExt)));
}

TU_TEST(the_previous_key_counts_on_from_where_it_stopped)
{
    MacSec_AckResult r;
    uint8_t rx[sizeof(arriving)];
    uint8_t i;

    for (i = 0u; i < sizeof(rx); ++i)
        rx[i] = arriving[i];

    rx[14] = 4u;
    freshState();
    state.prevFrameCounter = 0x100u;
    TU_EQ(MacSec_BuildEnhAck(&state, rx, sizeof(rx), ext8, 0, 0, 0u, ack, &r), sizeof(ackPrev));
    TU_CHECK(ackIs(ackPrev, sizeof(ackPrev)));
    TU_EQ(r.frameCounter, 0x100u);
    TU_EQ(state.prevFrameCounter, 0x101u);
    TU_EQ(state.frameCounter, COUNTER); /* the current key's is untouched */
}

TU_TEST(the_next_key_starts_from_zero_and_moves_no_counter)
{
    MacSec_AckResult r;
    uint8_t rx[sizeof(arriving)];
    uint8_t i;

    for (i = 0u; i < sizeof(rx); ++i)
        rx[i] = arriving[i];

    rx[14] = 6u;
    freshState();
    TU_EQ(MacSec_BuildEnhAck(&state, rx, sizeof(rx), ext8, 0, 0, 0u, ack, &r), sizeof(ackNext));
    TU_CHECK(ackIs(ackNext, sizeof(ackNext)));
    TU_EQ(r.frameCounter, 0u);
    TU_EQ(state.frameCounter, COUNTER);
}

TU_TEST(setting_keys_starts_the_previous_keys_counter)
{
    uint8_t value[56] = {0x08u, 6u};
    uint8_t k;

    freshState();

    for (k = 0u; k < 3u; ++k)
        value[2u + k * 18u] = 16u;

    TU_EQ(MacSec_SetKeys(&state, value, sizeof(value)), 1);
    TU_EQ(state.prevFrameCounter, COUNTER);
    TU_EQ(state.keyIndex, 6u);
}

TU_TEST(a_frame_that_cannot_be_answered_gets_no_ack)
{
    MacSec_AckResult r;
    uint8_t rx[sizeof(arriving)];
    uint8_t i;

    freshState();

    for (i = 0u; i < sizeof(rx); ++i)
        rx[i] = arriving[i];

    /* The auxiliary header has not arrived. */
    TU_EQ(MacSec_BuildEnhAck(&state, rx, 12u, ext8, 0, 0, 0u, ack, &r), 0u);
    /* A key index that is none of the three. */
    rx[14] = 9u;
    TU_EQ(MacSec_BuildEnhAck(&state, rx, sizeof(rx), ext8, 0, 0, 0u, ack, &r), 0u);
    TU_EQ(state.frameCounter, COUNTER);
    rx[14] = 5u;
    /* Level 5, key ID mode 2: not one a Thread node acknowledges this way. */
    rx[9] = 0x15u;
    TU_EQ(MacSec_BuildEnhAck(&state, rx, sizeof(rx), ext8, 0, 0, 0u, ack, &r), 0u);
    rx[9] = 0x0Du;
    /* No Ack asked for. */
    rx[0] = (uint8_t)(rx[0] & ~0x20u);
    TU_EQ(MacSec_BuildEnhAck(&state, rx, sizeof(rx), ext8, 0, 0, 0u, ack, &r), 0u);
    rx[0] = arriving[0];
    /* Version 2006: an Immediate Ack's business. */
    rx[1] = (uint8_t)((rx[1] & ~0x30u) | 0x10u);
    TU_EQ(MacSec_BuildEnhAck(&state, rx, sizeof(rx), ext8, 0, 0, 0u, ack, &r), 0u);
    rx[1] = arriving[1];
    /* A secured frame with no key to answer it. */
    MacSec_Init(&state);
    TU_EQ(MacSec_BuildEnhAck(&state, rx, sizeof(rx), ext8, 0, 0, 0u, ack, &r), 0u);
    /* The longest header the radio back-end reads still fits. */
    TU_CHECK(MAC_SEC_ACK_MAX >= 2u + 1u + 2u + 8u + 6u + 4u + 2u);
}

TU_TEST(an_unsecured_frame_gets_an_unsecured_ack)
{
    MacSec_AckResult r;
    uint8_t rx[9];
    uint8_t i;

    MacSec_Init(&state);

    for (i = 0u; i < sizeof(rx); ++i)
        rx[i] = arriving[i];

    rx[0] = (uint8_t)(rx[0] & ~0x08u);
    TU_EQ(MacSec_BuildEnhAck(&state, rx, sizeof(rx), ext8, 0, 0, 0u, ack, &r), 9u);
    TU_EQ(r.secured, 0);
    TU_EQ(ack[0], 0x02u);
    TU_EQ(ack[1], 0x28u);
    TU_EQ(ack[2], 0x13u);
    TU_EQ(ack[5], 0x78u);
}

/* LQI 200 and RSSI -40, in a vendor IE inside the secured part. */
TU_TEST(a_probing_ie_goes_inside_the_secured_ack)
{
    static const uint8_t want[] = {0x0A, 0x2A, 0x13, 0xCE, 0xFA, 0x78, 0x56, 0x0D, 0x03, 0x02, 0x01, 0x00, 0x05, 0x06, 0x00, 0x9B, 0xB8, 0xEA, 0x00, 0xC8, 0xB0, 0x90, 0x9F, 0xD1, 0x43, 0x00, 0x00};
    static const uint8_t probe[11] = {0x78, 0x56, 1, 2, 3, 4, 5, 6, 7, 8, MAC_SEC_PROBE_LQI | MAC_SEC_PROBE_RSSI};
    uint8_t ie[MAC_SEC_PROBE_IE_MAX];
    MacSec_AckRx rx;
    MacSec_AckResult r;
    uint8_t n;

    freshState();
    TU_EQ(MacSec_ParseAckRx(arriving, sizeof(arriving), &rx), 1);
    TU_EQ(MacSec_ProbeIe(&state, &rx, 200u, -40, -100, ie), 0u); /* nobody asked yet */
    TU_EQ(MacSec_SetProbe(&state, probe, sizeof(probe)), 1);
    n = MacSec_ProbeIe(&state, &rx, 200u, -40, -100, ie);
    TU_EQ(n, 8u);
    TU_EQ(MacSec_BuildEnhAck(&state, arriving, sizeof(arriving), ext8, 0, ie, n, ack, &r), sizeof(want));
    TU_CHECK(ackIs(want, sizeof(want)));
    TU_CHECK(sizeof(want) <= MAC_SEC_ACK_MAX);
}

TU_TEST(a_probe_ends_with_no_metrics_and_the_table_is_bounded)
{
    uint8_t probe[11] = {0x78, 0x56, 1, 2, 3, 4, 5, 6, 7, 8, MAC_SEC_PROBE_LQI};
    uint8_t ie[MAC_SEC_PROBE_IE_MAX];
    MacSec_AckRx rx;
    uint8_t i;

    freshState();
    TU_EQ(MacSec_ParseAckRx(arriving, sizeof(arriving), &rx), 1);
    TU_EQ(MacSec_SetProbe(&state, probe, 10u), 0);
    TU_EQ(MacSec_SetProbe(&state, probe, sizeof(probe)), 1);
    TU_EQ(MacSec_ProbeIe(&state, &rx, 7u, -40, -100, ie), 7u);
    TU_EQ(ie[6], 7u);
    probe[10] = 0u;
    TU_EQ(MacSec_SetProbe(&state, probe, sizeof(probe)), 1);
    TU_EQ(MacSec_ProbeIe(&state, &rx, 7u, -40, -100, ie), 0u);
    probe[10] = MAC_SEC_PROBE_LQI;

    for (i = 0u; i < MAC_SEC_PROBE_MAX; ++i)
    {
        probe[2] = (uint8_t)(0x40u + i);
        TU_EQ(MacSec_SetProbe(&state, probe, sizeof(probe)), 1);
    }

    probe[2] = 0x7Fu;
    TU_EQ(MacSec_SetProbe(&state, probe, sizeof(probe)), 0);
}

TU_TEST(an_arriving_frame_names_its_source)
{
    MacSec_AckRx a;

    TU_EQ(MacSec_ParseAckRx(arriving, sizeof(arriving), &a), 1);
    TU_EQ(a.version, 2u);
    TU_EQ(a.type, 1u);
    TU_EQ(a.srcMode, 2u);
    TU_EQ(a.srcShort, 0x5678u);
    TU_EQ(MacSec_ParseAckRx(arrivingExt, sizeof(arrivingExt), &a), 1);
    TU_EQ(a.srcMode, 3u);
    TU_EQ(a.srcExt[0], 0x11u);
    TU_EQ(MacSec_ParseAckRx(arriving, 4u, &a), 0);
}

int main(void)
{
    static const Tu_Test tests[] = {
        {"a_data_frame_encrypts_its_payload_and_authenticates_its_header",
         a_data_frame_encrypts_its_payload_and_authenticates_its_header},
        {"a_version_2006_poll_keeps_its_command_identifier_in_the_clear",
         a_version_2006_poll_keeps_its_command_identifier_in_the_clear},
        {"a_version_2015_frame_authenticates_its_header_ies", a_version_2015_frame_authenticates_its_header_ies},
        {"a_version_2015_poll_encrypts_its_command_identifier", a_version_2015_poll_encrypts_its_command_identifier},
        {"the_layout_finds_each_shape", the_layout_finds_each_shape},
        {"version_2015_pan_id_presence_follows_the_table", version_2015_pan_id_presence_follows_the_table},
        {"the_frame_counter_is_written_in_and_moves_on", the_frame_counter_is_written_in_and_moves_on},
        {"an_updated_header_is_kept_and_the_counter_does_not_move", an_updated_header_is_kept_and_the_counter_does_not_move},
        {"nothing_goes_out_marked_secured_but_unencrypted", nothing_goes_out_marked_secured_but_unencrypted},
        {"a_frame_the_host_secured_or_left_plain_passes_untouched", a_frame_the_host_secured_or_left_plain_passes_untouched},
        {"the_frames_key_index_selects_the_key", the_frames_key_index_selects_the_key},
        {"the_counter_property_honours_set_if_larger", the_counter_property_honours_set_if_larger},
        {"a_malformed_key_value_is_refused_and_changes_nothing", a_malformed_key_value_is_refused_and_changes_nothing},
        {"init_forgets_the_keys", init_forgets_the_keys},
        {"init_clears_what_an_ack_reads_under_the_lock", init_clears_what_an_ack_reads_under_the_lock},
        {"a_secured_frame_gets_a_secured_enhanced_ack", a_secured_frame_gets_a_secured_enhanced_ack},
        {"the_ack_in_two_steps_is_the_same_ack", the_ack_in_two_steps_is_the_same_ack},
        {"an_unsecured_frames_ack_has_no_mic_to_wait_for", an_unsecured_frames_ack_has_no_mic_to_wait_for},
        {"the_ack_pending_bit_follows_the_table", the_ack_pending_bit_follows_the_table},
        {"frame_pending_is_in_the_acks_frame_control", frame_pending_is_in_the_acks_frame_control},
        {"an_extended_source_is_the_acks_destination", an_extended_source_is_the_acks_destination},
        {"the_previous_key_counts_on_from_where_it_stopped", the_previous_key_counts_on_from_where_it_stopped},
        {"the_next_key_starts_from_zero_and_moves_no_counter", the_next_key_starts_from_zero_and_moves_no_counter},
        {"setting_keys_starts_the_previous_keys_counter", setting_keys_starts_the_previous_keys_counter},
        {"a_frame_that_cannot_be_answered_gets_no_ack", a_frame_that_cannot_be_answered_gets_no_ack},
        {"an_unsecured_frame_gets_an_unsecured_ack", an_unsecured_frame_gets_an_unsecured_ack},
        {"an_arriving_frame_names_its_source", an_arriving_frame_names_its_source},
        {"a_probing_ie_goes_inside_the_secured_ack", a_probing_ie_goes_inside_the_secured_ack},
        {"a_probe_ends_with_no_metrics_and_the_table_is_bounded", a_probe_ends_with_no_metrics_and_the_table_is_bounded},
    };

    Tu_Run("mac_sec", tests, (int)(sizeof(tests) / sizeof(tests[0])));
    return 0;
}
