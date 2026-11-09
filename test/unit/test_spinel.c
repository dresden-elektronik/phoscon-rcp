/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* test_spinel.c — the Spinel framing layer's suite (docs/testing.md §2). Vectors are the
 * payloads captured from a Silabs EFR32 RCP, several compared byte for byte. */
#include "spinel.h"
#include "tu.h"
#include "u_bstream.h"

static int sameBytes(const uint8_t *a, const uint8_t *b, uint16_t len)
{
    uint16_t i;

    for (i = 0; i < len; ++i)
    {
        if (a[i] != b[i])
            return 0;
    }

    return 1;
}

TU_TEST(header_byte_is_flg_10_plus_tid)
{
    /* FLG is 0b10 in bits 7:6 (0x80..0xBF); the spec's "(0x40)" is 0b01 and decodes
   * nothing. */
    TU_EQ(Spinel_Header(0u), 0x80u);
    TU_EQ(Spinel_Header(1u), 0x81u);
    TU_EQ(Spinel_Header(15u), 0x8Fu);
    TU_EQ(Spinel_HeaderTid(0x80u), 0);
    TU_EQ(Spinel_HeaderTid(0x81u), 1);
    TU_EQ(Spinel_HeaderTid(0x8Fu), 15);
    /* Everything outside 0b10xxxxxx is not Spinel (docs/protocol.md §4). */
    TU_EQ(Spinel_HeaderTid(0x00u), -1);
    TU_EQ(Spinel_HeaderTid(0x3Fu), -1);
    TU_EQ(Spinel_HeaderTid(0x40u), -1); /* the spec's wrong "0x40" */
    TU_EQ(Spinel_HeaderTid(0x7Fu), -1);
    TU_EQ(Spinel_HeaderTid(0xC0u), -1);
    TU_EQ(Spinel_HeaderTid(0xFFu), -1);
}

TU_TEST(nli_or_iid_bits_are_tolerated)
{
    /* Bits 5:4 are NLI here / IID upstream; only the FLG is required. */
    static const uint8_t plain[5] = {0x81u, 0x06u, 0x01u, 0x04u, 0x03u};
    static const uint8_t iid1[5] = {0x91u, 0x06u, 0x01u, 0x04u, 0x03u};
    Spinel_Frame f;

    TU_EQ(Spinel_Decode(plain, 5u, &f), 1);
    TU_EQ(f.tid, 1u);
    TU_EQ(Spinel_Decode(iid1, 5u, &f), 1);
    TU_EQ(f.tid, 1u);
}

TU_TEST(decodes_captured_bringup_payloads)
{
    static const uint8_t v1[5] = {0x81u, 0x06u, 0x01u, 0x04u, 0x03u};
    static const uint8_t v3[4] = {0x83u, 0x06u, 0x03u, 0x03u};
    static const uint8_t api[5] = {0x84u, 0x06u, 0xB0u, 0x01u, 0x0Au};
    static const uint8_t minapi[5] = {0x85u, 0x06u, 0xB1u, 0x01u, 0x04u};
    Spinel_Frame f;

    TU_EQ(Spinel_Decode(v1, 5u, &f), 1);
    TU_EQ(f.tid, 1u);
    TU_EQ(f.cmd, SPINEL_CMD_PROP_VALUE_IS);
    TU_EQ(f.prop, 1u);
    TU_EQ(f.valueLen, 2u);
    TU_EQ(f.value[0], 0x04u); /* 'ii' = 4.3 */
    TU_EQ(f.value[1], 0x03u);

    TU_EQ(Spinel_Decode(v3, 4u, &f), 1);
    TU_EQ(f.prop, 3u);
    TU_EQ(f.valueLen, 1u);
    TU_EQ(f.value[0], 3u);

    /* A property id above 0x7F arrives as a two-byte packed uint (docs/protocol.md §4). */
    TU_EQ(Spinel_Decode(api, 5u, &f), 1);
    TU_EQ(f.prop, 0xB0u);
    TU_EQ(f.valueLen, 1u);
    TU_EQ(f.value[0], 10u);
    TU_EQ(Spinel_Decode(minapi, 5u, &f), 1);
    TU_EQ(f.prop, 0xB1u);
    TU_EQ(f.value[0], 4u);
}

TU_TEST(decodes_captured_formnetwork_payloads)
{
    static const uint8_t phyEn[4] = {0x87u, 0x06u, 0x20u, 0x01u};
    static const uint8_t chan[4] = {0x88u, 0x06u, 0x21u, 0x14u};
    static const uint8_t txpow[4] = {0x89u, 0x06u, 0x25u, 0x13u};
    static const uint8_t panid[5] = {0x8Cu, 0x06u, 0x36u, 0xD9u, 0x85u};
    static const uint8_t raw[4] = {0x8Eu, 0x06u, 0x37u, 0x01u};
    Spinel_Frame f;

    TU_EQ(Spinel_Decode(phyEn, 4u, &f), 1);
    TU_EQ(f.tid, 7u);
    TU_EQ(f.prop, 0x20u);
    TU_EQ(f.value[0], 1u);

    TU_EQ(Spinel_Decode(chan, 4u, &f), 1);
    TU_EQ(f.prop, 0x21u);
    TU_EQ(f.value[0], 20u); /* channel 20 */

    TU_EQ(Spinel_Decode(txpow, 4u, &f), 1);
    TU_EQ(f.prop, 0x25u);
    TU_EQ(f.value[0], 19u); /* 0x13 dBm */

    TU_EQ(Spinel_Decode(panid, 5u, &f), 1);
    TU_EQ(f.prop, 0x36u);
    TU_EQ(f.valueLen, 2u); /* UINT16, little endian */
    TU_EQ(f.value[0] | ((uint16_t)f.value[1] << 8), 0x85D9u);

    TU_EQ(Spinel_Decode(raw, 4u, &f), 1);
    TU_EQ(f.prop, 0x37u);
    TU_EQ(f.value[0], 1u);
}

TU_TEST(decodes_unsolicited_last_status)
{
    /* captured RESET -> notify: payload 80 06 00 70 (TID 0, LAST_STATUS, 112). */
    static const uint8_t frame[4] = {0x80u, 0x06u, 0x00u, 0x70u};
    Spinel_Frame f;

    TU_EQ(Spinel_Decode(frame, 4u, &f), 1);
    TU_EQ(f.tid, 0u);
    TU_EQ(f.cmd, SPINEL_CMD_PROP_VALUE_IS);
    TU_EQ(f.prop, 0u); /* LAST_STATUS */
    TU_EQ(f.valueLen, 1u);
    TU_EQ(f.value[0], SPINEL_STATUS_RESET_POWER_ON);
}

TU_TEST(decodes_requests_and_reset)
{
    static const uint8_t get[3] = {0x81u, 0x02u, 0x21u}; /* GET PHY_CHAN, TID 1 */
    static const uint8_t set[4] = {0x88u, 0x03u, 0x21u, 0x14u};
    static const uint8_t reset[3] = {0x80u, 0x01u, 0x02u}; /* RESET, type 2 = stack */
    Spinel_Frame f;

    TU_EQ(Spinel_Decode(get, 3u, &f), 1);
    TU_EQ(f.cmd, SPINEL_CMD_PROP_VALUE_GET);
    TU_EQ(f.prop, 0x21u);
    TU_EQ(f.valueLen, 0u);

    TU_EQ(Spinel_Decode(set, 4u, &f), 1);
    TU_EQ(f.cmd, SPINEL_CMD_PROP_VALUE_SET);
    TU_EQ(f.prop, 0x21u);
    TU_EQ(f.valueLen, 1u);

    /* RESET carries no property id; whatever follows is its own payload (docs/protocol.md §4). */
    TU_EQ(Spinel_Decode(reset, 3u, &f), 1);
    TU_EQ(f.cmd, SPINEL_CMD_RESET);
    TU_EQ(f.valueLen, 1u);
    TU_EQ(f.value[0], 2u);
}

TU_TEST(build_matches_the_captured_frame_byte_for_byte)
{
    static const uint8_t want[5] = {0x81u, 0x06u, 0x01u, 0x04u, 0x03u};
    static const uint8_t value[2] = {0x04u, 0x03u};
    uint8_t buf[16];
    U_BStream bs;
    uint16_t n;

    U_bstream_init(&bs, buf, sizeof(buf));
    n = Spinel_Build(&bs, 1u, SPINEL_CMD_PROP_VALUE_IS, 1u, value, 2u);
    TU_EQ(n, 5u);
    TU_CHECK(sameBytes(buf, want, 5u));
}

TU_TEST(build_uses_a_packed_property_id)
{
    static const uint8_t want[5] = {0x84u, 0x06u, 0xB0u, 0x01u, 0x0Au};
    uint8_t buf[16];
    U_BStream bs;
    uint16_t n;

    U_bstream_init(&bs, buf, sizeof(buf));
    n = Spinel_Build(&bs, 4u, SPINEL_CMD_PROP_VALUE_IS, 0xB0u, want + 4u, 1u);
    TU_EQ(n, 5u);
    TU_CHECK(sameBytes(buf, want, 5u));
}

TU_TEST(build_status_matches_the_spec_reset_notification)
{
    /* "We emit 114 (RESET_SOFTWARE) with TID=0" (docs/protocol.md §6). */
    static const uint8_t want[4] = {0x80u, 0x06u, 0x00u, 0x72u};
    uint8_t buf[16];
    U_BStream bs;

    U_bstream_init(&bs, buf, sizeof(buf));
    TU_EQ(Spinel_BuildLastStatus(&bs, SPINEL_STATUS_RESET_SOFTWARE), 4u);
    TU_CHECK(sameBytes(buf, want, 4u));

    /* ... and an error answer carries the *command's* TID (docs/protocol.md §4). */
    U_bstream_init(&bs, buf, sizeof(buf));
    TU_EQ(Spinel_BuildStatus(&bs, 13u, SPINEL_STATUS_PROP_NOT_FOUND), 4u);
    TU_EQ(buf[0], 0x8Du); /* FLG 0b10 + TID 13 */
    TU_EQ(buf[3], SPINEL_STATUS_PROP_NOT_FOUND);
}

TU_TEST(build_refuses_when_it_does_not_fit)
{
    uint8_t buf[4];
    U_BStream bs;
    static const uint8_t value[4] = {1u, 2u, 3u, 4u};

    U_bstream_init(&bs, buf, sizeof(buf));
    TU_EQ(Spinel_Build(&bs, 1u, SPINEL_CMD_PROP_VALUE_IS, 0x21u, value, 4u), 0u);
}

/* A packed uint is the library's unsigned LEB128 (spinel.h): one decode and one encode. */
static uint32_t packedRead(const uint8_t *data, uint16_t len, uint16_t *used, int *ok)
{
    U_BStream bs;
    uint32_t v;

    U_bstream_init(&bs, (void *)data, len);
    v = (uint32_t)U_bstream_get_leb128_u32(&bs);
    *ok = bs.status == U_BSTREAM_OK;
    *used = (uint16_t)U_bstream_pos(&bs);
    return v;
}

static uint16_t packedWrite(uint8_t *dst, uint16_t cap, uint32_t v)
{
    U_BStream bs;

    U_bstream_init(&bs, dst, cap);
    U_bstream_put_leb128_u32(&bs, v);
    return bs.status == U_BSTREAM_OK ? (uint16_t)U_bstream_pos(&bs) : 0u;
}

TU_TEST(packed_uint_matches_the_spec_examples)
{
    static const uint8_t ex0B[1] = {0x0Bu};
    static const uint8_t exB0[2] = {0xB0u, 0x01u};
    static const uint8_t ex0403[2] = {0x83u, 0x08u}; /* the varint, not the
                                                        * spec's little-endian
                                                        * "03 04" */
    uint8_t buf[8];
    uint16_t used = 0u;
    uint16_t n;
    int ok = 0;

    TU_EQ(packedRead(ex0B, 1u, &used, &ok), 0x0Bu);
    TU_EQ(ok, 1);
    TU_EQ(used, 1u);
    TU_EQ(packedRead(exB0, 2u, &used, &ok), 0xB0u);
    TU_EQ(ok, 1);
    TU_EQ(used, 2u);
    TU_EQ(packedRead(ex0403, 2u, &used, &ok), 0x0403u);
    TU_EQ(ok, 1);
    TU_EQ(used, 2u);

    n = packedWrite(buf, sizeof(buf), 0x0Bu);
    TU_EQ(n, 1u);
    TU_CHECK(sameBytes(buf, ex0B, 1u));
    n = packedWrite(buf, sizeof(buf), 0xB0u);
    TU_EQ(n, 2u);
    TU_CHECK(sameBytes(buf, exB0, 2u));
    n = packedWrite(buf, sizeof(buf), 0x0403u);
    TU_EQ(n, 2u);
    TU_CHECK(sameBytes(buf, ex0403, 2u));
}

TU_TEST(packed_uint_round_trips_and_bounds)
{
    static const uint32_t values[] = {
        0u, 1u, 0x7Fu, 0x80u, 0x3FFFu, 0x4000u, 0x1FFFFFu, 0x200000u, 0x0FFFFFFFu, 0xFFFFFFFFu};
    uint8_t buf[8];
    uint16_t used = 0u;
    int ok = 0;
    unsigned i;

    for (i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
    {
        uint16_t n = packedWrite(buf, sizeof(buf), values[i]);

        TU_EQ(n, U_bstream_leb128_u32_size(values[i]));
        TU_EQ(packedRead(buf, n, &used, &ok), values[i]);
        TU_EQ(ok, 1);
        TU_EQ(used, n);
    }
    /* 0xFFFFFFFF needs five bytes; a sixth continuation byte, a fifth group with bits above 32 and a
     * value cut short are not this encoding, and a value that does not fit is not written in part. */
    TU_EQ(U_bstream_leb128_u32_size(0xFFFFFFFFu), 5u);
    {
        static const uint8_t six[6] = {0x80u, 0x80u, 0x80u, 0x80u, 0x80u, 0x01u};
        static const uint8_t above32[5] = {0xFFu, 0xFFu, 0xFFu, 0xFFu, 0x1Fu};
        static const uint8_t cut[2] = {0x80u, 0x80u};

        (void)packedRead(six, 6u, &used, &ok);
        TU_EQ(ok, 0);
        (void)packedRead(above32, 5u, &used, &ok);
        TU_EQ(ok, 0);
        (void)packedRead(cut, 2u, &used, &ok);
        TU_EQ(ok, 0);
        TU_EQ(packedWrite(buf, 2u, 0x4000u), 0u);
    }
}

/* A property id is read the same way, so one that does not decode leaves the frame unanswered. */
TU_TEST(a_property_id_past_32_bits_is_not_a_frame)
{
    static const uint8_t frame[7] = {0x81u, SPINEL_CMD_PROP_VALUE_GET, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0x1Fu};
    static const uint8_t wide[7] = {0x81u, SPINEL_CMD_PROP_VALUE_GET, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0x0Fu};
    Spinel_Frame f;

    TU_EQ(Spinel_Decode(frame, sizeof(frame), &f), 0);
    TU_EQ(Spinel_Decode(wide, sizeof(wide), &f), 1);
    TU_EQ(f.prop, 0xFFFFFFFFu);
    TU_EQ(f.valueLen, 0u);
}

TU_TEST(rejects_malformed_headers_and_truncation)
{
    static const uint8_t notSpinel[4] = {0x00u, 0x06u, 0x01u, 0x04u};
    static const uint8_t oneByte[1] = {0x81u};
    static const uint8_t truncated[2] = {0x81u, 0x06u};
    static const uint8_t noValue[3] = {0x81u, 0x06u, 0x01u};
    static const uint8_t openVarint[5] = {0x81u, 0x06u, 0x80u, 0x80u, 0x80u};
    Spinel_Frame f;

    TU_EQ(Spinel_Decode(notSpinel, 4u, &f), 0);
    TU_EQ(Spinel_Decode(oneByte, 1u, &f), 0);
    TU_EQ(Spinel_Decode(truncated, 2u, &f), 0);  /* property id missing */
    TU_EQ(Spinel_Decode(openVarint, 5u, &f), 0); /* varint never terminates */
    /* A property with no value at all is a valid GET. */
    TU_EQ(Spinel_Decode(noValue, 3u, &f), 1);
    TU_EQ(f.valueLen, 0u);
}

TU_TEST(random_frames_never_escape_the_buffer)
{
    uint8_t buf[32]; /* len below can reach 17 */
    Spinel_Frame f;
    uint32_t seed = 0xBEEF01u;
    unsigned i;
    unsigned decoded = 0;
    unsigned rejected = 0;

    for (i = 0; i < 20000u; ++i)
    {
        uint16_t len;
        unsigned j;

        seed = (seed * 1103515245u) + 12345u;
        len = (uint16_t)(2u + ((seed >> 16) & 0x0Fu));

        for (j = 0; j < len; ++j)
        {
            seed = (seed * 1103515245u) + 12345u;
            buf[j] = (uint8_t)((seed >> 16) & 0xFFu);
        }

        if (Spinel_Decode(buf, len, &f))
        {
            decoded++;
            /* Whatever it decoded as, the value window must lie inside the frame. */
            TU_CHECK(f.value >= buf && f.value <= buf + len);
            TU_CHECK((uint32_t)(f.value - buf) + f.valueLen <= len);
        }
        else
            rejected++;
    }

    TU_NOTE("random decoded", decoded);
    TU_NOTE("random rejected", rejected);
}

int main(void)
{
    static const Tu_Test tests[] = {
        {"header_byte_is_flg_10_plus_tid", header_byte_is_flg_10_plus_tid},
        {"nli_or_iid_bits_are_tolerated", nli_or_iid_bits_are_tolerated},
        {"decodes_captured_bringup_payloads", decodes_captured_bringup_payloads},
        {"decodes_captured_formnetwork_payloads", decodes_captured_formnetwork_payloads},
        {"decodes_unsolicited_last_status", decodes_unsolicited_last_status},
        {"decodes_requests_and_reset", decodes_requests_and_reset},
        {"build_matches_the_captured_frame_byte_for_byte",
         build_matches_the_captured_frame_byte_for_byte},
        {"build_uses_a_packed_property_id", build_uses_a_packed_property_id},
        {"build_status_matches_the_spec_reset_notification",
         build_status_matches_the_spec_reset_notification},
        {"build_refuses_when_it_does_not_fit", build_refuses_when_it_does_not_fit},
        {"packed_uint_matches_the_spec_examples", packed_uint_matches_the_spec_examples},
        {"packed_uint_round_trips_and_bounds", packed_uint_round_trips_and_bounds},
        {"a_property_id_past_32_bits_is_not_a_frame", a_property_id_past_32_bits_is_not_a_frame},
        {"rejects_malformed_headers_and_truncation", rejects_malformed_headers_and_truncation},
        {"random_frames_never_escape_the_buffer", random_frames_never_escape_the_buffer}};

    Tu_Run("spinel", tests, (int)(sizeof(tests) / sizeof(tests[0])));
    return 0; /* Tu_Run never returns */
}
