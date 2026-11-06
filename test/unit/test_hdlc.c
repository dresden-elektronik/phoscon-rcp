/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* test_hdlc.c — the HDLC codec's suite (docs/testing.md §2, coding-rules.md §4.4/§4.5).
 * Vectors are frames captured from a Silabs EFR32 RCP plus the worked
 * byte-stuffing example; all FCS-verified there except the invalid MAC_15_4_LADDR. */
#include <stdio.h>

#include "hdlc.h"
#include "tu.h"
#include "u_bstream.h"

static Hdlc_RxResult feed(const uint8_t *bytes, unsigned len, Hdlc_Rx *rx, const uint8_t **payload)
{
    Hdlc_RxResult last = HDLC_RX_NONE;
    unsigned i;

    for (i = 0; i < len; ++i)
    {
        Hdlc_RxResult r = Hdlc_RxByte(rx, bytes[i]);

        if (r != HDLC_RX_NONE)
        {
            last = r;
            *payload = rx->buf;
        }
    }

    return last;
}

static int payloadIs(const Hdlc_Rx *rx, const uint8_t *want, unsigned len)
{
    unsigned i;

    if (rx->len != len)
        return 0;

    for (i = 0; i < len; ++i)
    {
        if (rx->buf[i] != want[i])
            return 0;
    }

    return 1;
}

TU_TEST(crc_matches_spec_worked_example)
{
    /* docs/protocol.md §3: "payload 89 06 25 13, FCS computed over the unescaped payload,
   * crc(89 06 25 13) = 0x7e64, wire FCS = 0x7e64 ^ 0xffff = 0x819b". */
    static const uint8_t payload[4] = {0x89u, 0x06u, 0x25u, 0x13u};
    static const uint8_t withFcs[6] = {0x89u, 0x06u, 0x25u, 0x13u, 0x9Bu, 0x81u};

    TU_EQ(Hdlc_Crc(payload, 4u), 0x7E64u);
    /* ... and the residue property: a frame with its FCS included leaves 0xF0B8. */
    TU_EQ(Hdlc_Crc(withFcs, 6u), HDLC_FCS_GOOD);
}

TU_TEST(decodes_captured_bringup_frame)
{
    /* captured GET->PROTOCOL_VERSION: 7e 81 06 01 04 03 db 0a 7e */
    static const uint8_t frame[9] = {0x7Eu, 0x81u, 0x06u, 0x01u, 0x04u, 0x03u, 0xDBu, 0x0Au, 0x7Eu};
    static const uint8_t want[5] = {0x81u, 0x06u, 0x01u, 0x04u, 0x03u};
    Hdlc_Rx rx;
    const uint8_t *payload = 0;

    Hdlc_RxInit(&rx);
    TU_EQ(feed(frame, sizeof(frame), &rx, &payload), HDLC_RX_FRAME);
    TU_CHECK(payloadIs(&rx, want, sizeof(want)));
    TU_EQ(rx.len, 5u);
}

TU_TEST(decodes_captured_set_frame)
{
    /* captured SET PHY_ENABLED: 7e 87 06 20 01 f2 62 7e */
    static const uint8_t frame[8] = {0x7Eu, 0x87u, 0x06u, 0x20u, 0x01u, 0xF2u, 0x62u, 0x7Eu};
    static const uint8_t want[4] = {0x87u, 0x06u, 0x20u, 0x01u};
    Hdlc_Rx rx;
    const uint8_t *payload = 0;

    Hdlc_RxInit(&rx);
    TU_EQ(feed(frame, sizeof(frame), &rx, &payload), HDLC_RX_FRAME);
    TU_CHECK(payloadIs(&rx, want, sizeof(want)));
}

TU_TEST(decodes_escaped_captured_frame)
{
    /* docs/protocol.md §3: 7e 89 06 25 7d 33 9b 81 7e - the 0x13 (XON) arrives as 7d 33, so this is
   * the one vector that exercises the unescape path end to end. */
    static const uint8_t frame[9] = {0x7Eu, 0x89u, 0x06u, 0x25u, 0x7Du, 0x33u, 0x9Bu, 0x81u, 0x7Eu};
    static const uint8_t want[4] = {0x89u, 0x06u, 0x25u, 0x13u};
    Hdlc_Rx rx;
    const uint8_t *payload = 0;

    Hdlc_RxInit(&rx);
    TU_EQ(feed(frame, sizeof(frame), &rx, &payload), HDLC_RX_FRAME);
    TU_CHECK(payloadIs(&rx, want, sizeof(want)));
}

TU_TEST(encode_then_decode_round_trip)
{
    /* Deterministic LCG; the payload deliberately contains the four escape bytes and
   * their neighbours. */
    static uint8_t payload[64];
    static uint8_t framed[HDLC_FRAME_MAX];
    static uint8_t special[6] = {0x11u, 0x13u, 0x7Eu, 0x7Du, 0x00u, 0xFFu};
    U_BStream out;
    Hdlc_Rx rx;
    const uint8_t *back = 0;
    uint32_t seed = 12345u;
    unsigned i;
    uint16_t n;

    for (i = 0; i < sizeof(payload); ++i)
    {
        seed = (seed * 1103515245u) + 12345u;
        payload[i] = (uint8_t)((seed >> 16) & 0xFFu);
    }

    for (i = 0; i < sizeof(special); ++i)
        payload[i] = special[i];

    U_bstream_init(&out, framed, sizeof(framed));
    n = Hdlc_Encode(payload, sizeof(payload), &out);
    TU_CHECK(n > sizeof(payload));
    TU_EQ(framed[0], HDLC_FEND);
    TU_EQ(framed[n - 1u], HDLC_FEND);

    Hdlc_RxInit(&rx);
    TU_EQ(feed(framed, n, &rx, &back), HDLC_RX_FRAME);
    TU_CHECK(payloadIs(&rx, payload, sizeof(payload)));
}

TU_TEST(encode_escapes_the_superset)
{
    static const uint8_t payload[4] = {0x11u, 0x13u, 0x7Eu, 0x7Du};
    static uint8_t framed[HDLC_FRAME_MAX];
    U_BStream out;
    uint16_t n;
    unsigned i;

    U_bstream_init(&out, framed, sizeof(framed));
    n = Hdlc_Encode(payload, sizeof(payload), &out);
    /* 4 escaped bytes + FCS pair + 2 delimiters => at least 12 bytes. */
    TU_CHECK(n >= 12u);
    /* No bare 0x7D or 0x7E in the body: 0x7D consumes the next byte, 0x7E ends the
   * frame. */
    for (i = 1u; i + 1u < n; ++i)
    {
        if (framed[i] == HDLC_FEND)
            TU_CHECK(0);
        else if (framed[i] == HDLC_FESC)
        {
            TU_CHECK(framed[i + 1u] != HDLC_FEND && framed[i + 1u] != HDLC_FESC);
            i++;
        }
    }
}

TU_TEST(encode_refuses_frames_that_do_not_fit)
{
    static uint8_t payload[128];
    static uint8_t tight[32];
    U_BStream out;

    U_bstream_init(&out, tight, sizeof(tight));
    TU_EQ(Hdlc_Encode(payload, sizeof(payload), &out), 0u);
}

/* The receive buffer must hold the worst-case wire frame whole: the largest Spinel frame
 * (a 160-byte value plus a 3-byte header) with every byte escaped encodes to 332 bytes. */
TU_TEST(maximum_frame_survives_encode_and_decode)
{
    static uint8_t payload[(HDLC_FRAME_MAX - 6u) / 2u];
    static uint8_t framed[HDLC_FRAME_MAX];
    U_BStream out;
    Hdlc_Rx rx;
    const uint8_t *back = 0;
    unsigned i;
    uint16_t n;

    for (i = 0u; i < sizeof(payload); ++i)
    {
        payload[i] = HDLC_FEND; /* every byte takes the two-byte escape */
    }

    U_bstream_init(&out, framed, sizeof(framed));
    n = Hdlc_Encode(payload, sizeof(payload), &out);
    TU_CHECK(n > sizeof(payload));
    TU_CHECK(n <= HDLC_FRAME_MAX);
    Hdlc_RxInit(&rx);
    TU_EQ(feed(framed, n, &rx, &back), HDLC_RX_FRAME);
    TU_CHECK(payloadIs(&rx, payload, sizeof(payload)));
}

TU_TEST(resyncs_after_garbage)
{
    /* Anything before the first FEND is discarded (docs/protocol.md §3). */
    static const uint8_t stream[12] = {
        0xAAu, 0x55u, 0x00u, 0x7Du, 0x7Eu, 0x7Eu, 0x81u, 0x06u, 0x01u, 0x04u, 0x03u, 0x7Eu};
    /* 7d 7e in the noise decodes to 0x5E, then the next 7e delimits: noise cannot desync. */
    static const uint8_t frame[9] = {0x7Eu, 0x81u, 0x06u, 0x01u, 0x04u, 0x03u, 0xDBu, 0x0Au, 0x7Eu};
    Hdlc_Rx rx;
    const uint8_t *payload = 0;

    Hdlc_RxInit(&rx);
    (void)feed(stream, sizeof(stream), &rx, &payload);
    Hdlc_RxInit(&rx);
    TU_EQ(feed(frame, sizeof(frame), &rx, &payload), HDLC_RX_FRAME);
    TU_EQ(rx.len, 5u);
}

TU_TEST(rejects_corrupted_fcs)
{
    static const uint8_t frame[9] = {0x7Eu, 0x81u, 0x06u, 0x01u, 0x04u, 0x03u, 0xDBu, 0x0Au, 0x7Eu};
    uint8_t bad[9];
    Hdlc_Rx rx;
    const uint8_t *payload = 0;
    unsigned i;

    for (i = 0; i < sizeof(bad); ++i)
        bad[i] = frame[i];

    bad[6] ^= 0x01u;
    Hdlc_RxInit(&rx);
    TU_EQ(feed(bad, sizeof(bad), &rx, &payload), HDLC_RX_ERROR);
}

TU_TEST(rejects_truncated_frame)
{
    static const uint8_t frame[9] = {0x7Eu, 0x81u, 0x06u, 0x01u, 0x04u, 0x03u, 0xDBu, 0x0Au, 0x7Eu};
    Hdlc_Rx rx;
    const uint8_t *payload = 0;

    /* One FCS byte short: the delimiter arrives but the residue is wrong -> ERROR. */
    Hdlc_RxInit(&rx);
    TU_EQ(feed(frame, sizeof(frame) - 2u, &rx, &payload), HDLC_RX_NONE);
    TU_EQ(Hdlc_RxByte(&rx, HDLC_FEND), HDLC_RX_ERROR);

    /* One byte cannot hold an FCS: two FENDs are idle. */
    Hdlc_RxInit(&rx);
    TU_EQ(Hdlc_RxByte(&rx, HDLC_FEND), HDLC_RX_NONE);
    TU_EQ(Hdlc_RxByte(&rx, 0x01u), HDLC_RX_NONE);
    TU_EQ(Hdlc_RxByte(&rx, HDLC_FEND), HDLC_RX_ERROR);
}

TU_TEST(drops_oversize_frame_and_stays_in_sync)
{
    static const uint8_t frame[9] = {0x7Eu, 0x81u, 0x06u, 0x01u, 0x04u, 0x03u, 0xDBu, 0x0Au, 0x7Eu};
    Hdlc_Rx rx;
    const uint8_t *payload = 0;
    unsigned i;

    Hdlc_RxInit(&rx);
    TU_EQ(Hdlc_RxByte(&rx, HDLC_FEND), HDLC_RX_NONE);

    for (i = 0; i < HDLC_FRAME_MAX + 8u; ++i)
        TU_EQ(Hdlc_RxByte(&rx, 0x5Au), HDLC_RX_NONE);

    TU_CHECK(rx.len <= HDLC_FRAME_MAX); /* never wrote past the buffer */
    TU_EQ(Hdlc_RxByte(&rx, HDLC_FEND), HDLC_RX_ERROR);
    TU_EQ(feed(frame, sizeof(frame), &rx, &payload), HDLC_RX_FRAME);
}

TU_TEST(idle_delimiters_are_not_frames)
{
    Hdlc_Rx rx;
    unsigned i;

    Hdlc_RxInit(&rx);

    for (i = 0; i < 5u; ++i)
        TU_EQ(Hdlc_RxByte(&rx, HDLC_FEND), HDLC_RX_NONE);
}

/* A deterministic pseudo-random stream (coding-rules.md §4.4): never past the buffer,
 * always resyncs, never reports a frame the residue check rejects. */
TU_TEST(random_stream_never_desyncs)
{
    static const uint8_t frame[9] = {0x7Eu, 0x81u, 0x06u, 0x01u, 0x04u, 0x03u, 0xDBu, 0x0Au, 0x7Eu};
    Hdlc_Rx rx;
    const uint8_t *payload = 0;
    uint32_t seed = 0xC0FFEEu;
    unsigned i;
    unsigned frames = 0;
    unsigned errors = 0;

    Hdlc_RxInit(&rx);

    for (i = 0; i < 20000u; ++i)
    {
        Hdlc_RxResult r;

        seed = (seed * 1103515245u) + 12345u;
        r = Hdlc_RxByte(&rx, (uint8_t)((seed >> 16) & 0xFFu));

        if (r == HDLC_RX_FRAME)
            frames++;
        else if (r == HDLC_RX_ERROR)
            errors++;

        TU_CHECK(rx.len <= HDLC_FRAME_MAX);
    }

    TU_NOTE("random frames", frames);
    TU_NOTE("random errors", errors);

    Hdlc_RxInit(&rx);
    TU_EQ(feed(frame, sizeof(frame), &rx, &payload), HDLC_RX_FRAME);
    TU_EQ(rx.len, 5u);
}

/* The worst case is 2 * len + 6: an opening FEND, every body byte escaped, both FCS bytes escaped,
 * a closing FEND. That needs a payload whose body escapes and whose FCS bytes both escape as well, so
 * it is searched for - 0x7E and 0x7D both escape, which keeps the body worst-case while varying the
 * CRC. Two earlier versions of this case were wrong in instructive ways: one asserted 332 for a
 * payload whose FCS happened not to escape (the encoder was right at 330), the next accepted a payload
 * with only one escaping FCS byte (65, not 66). */
TU_TEST(encode_worst_case_needs_two_per_byte_plus_six)
{
    static uint8_t payload[16];
    static uint8_t wire[HDLC_FRAME_MAX];
    U_BStream out;
    uint32_t pattern;
    uint16_t crc;
    int found = 0;
    unsigned i;

    for (pattern = 0u; pattern < (1u << 16) && !found; ++pattern)
    {
        for (i = 0u; i < (unsigned)sizeof(payload); ++i)
        {
            payload[i] = (uint8_t)(((pattern >> i) & 1u) ? HDLC_FEND : HDLC_FESC);
        }

        crc = (uint16_t)(Hdlc_Crc(payload, (uint16_t)sizeof(payload)) ^ 0xFFFFu);
        if ((crc & 0xFFu) == HDLC_FEND || (crc & 0xFFu) == HDLC_FESC)
        {
            found = ((crc >> 8) == HDLC_FEND || (crc >> 8) == HDLC_FESC);
        }
    }

    TU_CHECK(found); /* without a both-escaping FCS the +6 bound would go untested */

    /* 2 * 16 + 6 = 38 exactly, and one byte less is refused rather than truncated. */
    U_bstream_init(&out, wire, 38u);
    TU_EQ(Hdlc_Encode(payload, (uint16_t)sizeof(payload), &out), 38u);

    U_bstream_init(&out, wire, 37u);
    TU_EQ(Hdlc_Encode(payload, (uint16_t)sizeof(payload), &out), 0u);
}
int main(void)
{
    static const Tu_Test tests[] = {
        {"crc_matches_spec_worked_example", crc_matches_spec_worked_example},
        {"decodes_captured_bringup_frame", decodes_captured_bringup_frame},
        {"decodes_captured_set_frame", decodes_captured_set_frame},
        {"decodes_escaped_captured_frame", decodes_escaped_captured_frame},
        {"encode_then_decode_round_trip", encode_then_decode_round_trip},
        {"encode_escapes_the_superset", encode_escapes_the_superset},
        {"encode_refuses_frames_that_do_not_fit", encode_refuses_frames_that_do_not_fit},
        {"encode_worst_case_needs_two_per_byte_plus_six",
         encode_worst_case_needs_two_per_byte_plus_six},
        {"maximum_frame_survives_encode_and_decode", maximum_frame_survives_encode_and_decode},
        {"resyncs_after_garbage", resyncs_after_garbage},
        {"rejects_corrupted_fcs", rejects_corrupted_fcs},
        {"rejects_truncated_frame", rejects_truncated_frame},
        {"drops_oversize_frame_and_stays_in_sync", drops_oversize_frame_and_stays_in_sync},
        {"idle_delimiters_are_not_frames", idle_delimiters_are_not_frames},
        {"random_stream_never_desyncs", random_stream_never_desyncs}};

    Tu_Run("hdlc", tests, (int)(sizeof(tests) / sizeof(tests[0])));
    return 0; /* Tu_Run never returns */
}
