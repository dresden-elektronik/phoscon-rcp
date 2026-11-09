/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* test_srcmatch.c — the source-match table's suite (docs/testing.md §2). The table is what
 * the Ack's Frame Pending bit reads, so the cases that matter are the ones where a wrong
 * answer would claim "nothing pending" for a peer that has a frame waiting: a refused
 * insert, a partial list, and the extended-address byte order. */
#include "srcmatch.h"
#include "tu.h"

static void fillShort(SrcMatch_Table *t, uint16_t base, uint8_t n)
{
    uint8_t i;

    for (i = 0; i < n; ++i)
        (void)SrcMatch_InsertShort(t, (uint16_t)(base + i));
}

static void fillExt(SrcMatch_Table *t, uint8_t base, uint8_t n)
{
    uint8_t e[SRCMATCH_EXT_LEN];
    uint8_t i;
    uint8_t k;

    for (i = 0; i < n; ++i)
    {
        for (k = 0; k < SRCMATCH_EXT_LEN; ++k)
            e[k] = (uint8_t)(base + i + k);

        (void)SrcMatch_InsertExt(t, e);
    }
}

TU_TEST(a_fresh_table_is_empty_and_disabled)
{
    SrcMatch_Table t;

    SrcMatch_Init(&t);
    TU_EQ(t.enabled, 0);
    TU_EQ(t.shortCount, 0);
    TU_EQ(t.extCount, 0);
    TU_EQ(SrcMatch_HasShort(&t, 0x1234u), 0);
    TU_EQ(SrcMatch_HasExt(&t, (const uint8_t *)"\1\2\3\4\5\6\7\10"), 0);
}

TU_TEST(a_short_list_round_trips_through_the_wire_form)
{
    SrcMatch_Table t;
    uint8_t buf[SRCMATCH_SHORT_MAX * 2u];
    U_BStream bs;
    /* little endian on the wire, as 'A(S)' carries it */
    static const uint8_t wire[6] = {0x34u, 0x12u, 0xCDu, 0xABu, 0xFFu, 0xFFu};

    SrcMatch_Init(&t);
    TU_EQ(SrcMatch_SetShortList(&t, wire, sizeof(wire)), SRCMATCH_OK);
    TU_EQ(t.shortCount, 3);
    TU_EQ(SrcMatch_HasShort(&t, 0x1234u), 1);
    TU_EQ(SrcMatch_HasShort(&t, 0xABCDu), 1);
    TU_EQ(SrcMatch_HasShort(&t, 0xFFFFu), 1);
    TU_EQ(SrcMatch_HasShort(&t, 0x1235u), 0);

    U_bstream_init(&bs, buf, sizeof(buf));
    TU_EQ(SrcMatch_EncodeShort(&t, &bs), 6);
    TU_EQ(buf[0], 0x34u);
    TU_EQ(buf[1], 0x12u);
    TU_EQ(buf[5], 0xFFu);
}

TU_TEST(an_empty_set_clears_the_list)
{
    SrcMatch_Table t;

    SrcMatch_Init(&t);
    fillShort(&t, 0x0100u, 4);
    TU_EQ(t.shortCount, 4);
    TU_EQ(SrcMatch_SetShortList(&t, 0, 0), SRCMATCH_OK);
    TU_EQ(t.shortCount, 0);
}

TU_TEST(a_list_that_does_not_fit_is_refused_whole)
{
    SrcMatch_Table t;
    uint8_t wire[(SRCMATCH_SHORT_MAX + 1u) * 2u];
    uint8_t i;

    SrcMatch_Init(&t);
    (void)SrcMatch_InsertShort(&t, 0x0042u);

    for (i = 0; i < SRCMATCH_SHORT_MAX + 1u; ++i)
    {
        wire[2u * i] = (uint8_t)i;
        wire[2u * i + 1u] = 0x00u;
    }

    /* 33 entries: the old table survives, because a partial list would claim "nothing
     * pending" for addresses the host believes it wrote. */
    TU_EQ(SrcMatch_SetShortList(&t, wire, (uint16_t)sizeof(wire)), SRCMATCH_FULL);
    TU_EQ(t.shortCount, 1);
    TU_EQ(SrcMatch_HasShort(&t, 0x0042u), 1);
    TU_EQ(SrcMatch_HasShort(&t, 0x0000u), 0);
}

TU_TEST(a_length_that_is_not_a_multiple_of_the_entry_size_is_refused)
{
    SrcMatch_Table t;
    static const uint8_t odd[3] = {0x01u, 0x02u, 0x03u};
    static const uint8_t badExt[9] = {0};

    SrcMatch_Init(&t);
    TU_EQ(SrcMatch_SetShortList(&t, odd, sizeof(odd)), SRCMATCH_BAD_LEN);
    TU_EQ(SrcMatch_SetExtList(&t, badExt, sizeof(badExt)), SRCMATCH_BAD_LEN);
    TU_EQ(t.shortCount, 0);
    TU_EQ(t.extCount, 0);
    TU_EQ(SrcMatch_SetShortList(&t, 0, 2), SRCMATCH_BAD_LEN); /* a list with no bytes */
    TU_EQ(SrcMatch_SetShortList(0, odd, 2), SRCMATCH_BAD_LEN);
}

TU_TEST(the_full_short_pool_refuses_rather_than_evicts)
{
    SrcMatch_Table t;

    SrcMatch_Init(&t);
    fillShort(&t, 0x0100u, SRCMATCH_SHORT_MAX);
    TU_EQ(t.shortCount, SRCMATCH_SHORT_MAX);
    TU_EQ(SrcMatch_HasShort(&t, 0x0100u), 1);
    TU_EQ(SrcMatch_InsertShort(&t, 0x9999u), SRCMATCH_FULL);
    TU_EQ(SrcMatch_HasShort(&t, 0x9999u), 0);
    TU_EQ(t.shortCount, SRCMATCH_SHORT_MAX);
    TU_EQ(SrcMatch_HasShort(&t, 0x0100u), 1);
}

TU_TEST(the_full_extended_pool_refuses_rather_than_evicts)
{
    SrcMatch_Table t;
    uint8_t e[SRCMATCH_EXT_LEN];

    SrcMatch_Init(&t);
    fillExt(&t, 0x10u, SRCMATCH_EXT_MAX);
    TU_EQ(t.extCount, SRCMATCH_EXT_MAX);

    for (uint8_t k = 0; k < SRCMATCH_EXT_LEN; ++k)
        e[k] = 0xEEu;

    TU_EQ(SrcMatch_InsertExt(&t, e), SRCMATCH_FULL);
    TU_EQ(SrcMatch_HasExt(&t, e), 0);
    TU_EQ(t.extCount, SRCMATCH_EXT_MAX);
}

TU_TEST(inserting_what_is_already_there_stores_it_once)
{
    SrcMatch_Table t;
    uint8_t e[SRCMATCH_EXT_LEN] = {1, 2, 3, 4, 5, 6, 7, 8};

    SrcMatch_Init(&t);
    TU_EQ(SrcMatch_InsertShort(&t, 0x1111u), SRCMATCH_OK);
    TU_EQ(SrcMatch_InsertShort(&t, 0x1111u), SRCMATCH_OK);
    TU_EQ(t.shortCount, 1);

    TU_EQ(SrcMatch_InsertExt(&t, e), SRCMATCH_OK);
    TU_EQ(SrcMatch_InsertExt(&t, e), SRCMATCH_OK);
    TU_EQ(t.extCount, 1);

    /* One REMOVE takes the address away, not one of two copies. */
    TU_EQ(SrcMatch_RemoveShort(&t, 0x1111u), SRCMATCH_OK);
    TU_EQ(SrcMatch_HasShort(&t, 0x1111u), 0);
    TU_EQ(t.shortCount, 0);
}

TU_TEST(removing_an_absent_entry_says_so)
{
    SrcMatch_Table t;
    uint8_t e[SRCMATCH_EXT_LEN] = {9, 9, 9, 9, 9, 9, 9, 9};

    SrcMatch_Init(&t);
    TU_EQ(SrcMatch_RemoveShort(&t, 0x2222u), SRCMATCH_ABSENT);
    TU_EQ(SrcMatch_RemoveExt(&t, e), SRCMATCH_ABSENT);
    TU_EQ(SrcMatch_RemoveShort(0, 0x2222u), SRCMATCH_BAD_LEN);
}

TU_TEST(removal_keeps_the_other_entries)
{
    SrcMatch_Table t;
    uint8_t e[SRCMATCH_EXT_LEN];

    SrcMatch_Init(&t);
    fillShort(&t, 0x0100u, 4);
    TU_EQ(SrcMatch_RemoveShort(&t, 0x0101u), SRCMATCH_OK);
    TU_EQ(t.shortCount, 3);
    TU_EQ(SrcMatch_HasShort(&t, 0x0100u), 1);
    TU_EQ(SrcMatch_HasShort(&t, 0x0101u), 0);
    TU_EQ(SrcMatch_HasShort(&t, 0x0102u), 1);
    TU_EQ(SrcMatch_HasShort(&t, 0x0103u), 1);

    fillExt(&t, 0x20u, 3);

    for (uint8_t k = 0; k < SRCMATCH_EXT_LEN; ++k)
        e[k] = (uint8_t)(0x21u + k);

    TU_EQ(SrcMatch_RemoveExt(&t, e), SRCMATCH_OK);
    TU_EQ(t.extCount, 2);

    for (uint8_t k = 0; k < SRCMATCH_EXT_LEN; ++k)
        e[k] = (uint8_t)(0x20u + k);

    TU_EQ(SrcMatch_HasExt(&t, e), 1);

    for (uint8_t k = 0; k < SRCMATCH_EXT_LEN; ++k)
        e[k] = (uint8_t)(0x22u + k);

    TU_EQ(SrcMatch_HasExt(&t, e), 1);
}

TU_TEST(an_extended_list_round_trips_in_wire_order)
{
    SrcMatch_Table t;
    /* The wire form is MSB first: OpenThread writes otExtAddress.m8 unchanged, ziggurat's
     * eui64_to_spinel_bytes reverses its on-air bytes into the same order, and it is the order
     * MAC_15_4_LADDR uses. This vector is the EUI-64 00:12:4b:00:1c:aa:bb:cc. */
    static const uint8_t wire[8] = {0x00u, 0x12u, 0x4Bu, 0x00u, 0x1Cu, 0xAAu, 0xBBu, 0xCCu};
    uint8_t out[SRCMATCH_EXT_MAX * SRCMATCH_EXT_LEN];
    U_BStream bs;

    SrcMatch_Init(&t);
    TU_EQ(SrcMatch_SetExtList(&t, wire, sizeof(wire)), SRCMATCH_OK);
    TU_EQ(t.extCount, 1);
    TU_EQ(SrcMatch_HasExt(&t, wire), 1);

    U_bstream_init(&bs, out, sizeof(out));
    TU_EQ(SrcMatch_EncodeExt(&t, &bs), 8);

    for (uint8_t k = 0; k < 8u; ++k)
        TU_EQ(out[k], wire[k]);
}

/* The radio compares a received frame's source address, which travels LSB first. */
TU_TEST(the_radios_copy_holds_the_extended_addresses_in_on_air_order)
{
    SrcMatch_Table t;
    SrcMatch_Table air;
    static const uint8_t wire[16] = {0x00u, 0x12u, 0x4Bu, 0x00u, 0x1Cu, 0xAAu, 0xBBu, 0xCCu,
                                     0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u, 0x08u};
    static const uint8_t onAir0[8] = {0xCCu, 0xBBu, 0xAAu, 0x1Cu, 0x00u, 0x4Bu, 0x12u, 0x00u};
    static const uint8_t onAir1[8] = {0x08u, 0x07u, 0x06u, 0x05u, 0x04u, 0x03u, 0x02u, 0x01u};

    SrcMatch_Init(&t);
    t.enabled = 1u;
    TU_EQ(SrcMatch_SetExtList(&t, wire, sizeof(wire)), SRCMATCH_OK);
    TU_EQ(SrcMatch_InsertShort(&t, 0x1234u), SRCMATCH_OK);

    SrcMatch_CopyOnAir(&air, &t);
    TU_EQ(air.enabled, 1);
    TU_EQ(air.extCount, 2);
    TU_EQ(SrcMatch_HasExt(&air, onAir0), 1);
    TU_EQ(SrcMatch_HasExt(&air, onAir1), 1);
    TU_EQ(SrcMatch_HasExt(&air, wire), 0);
    TU_EQ(SrcMatch_HasShort(&air, 0x1234u), 1);
    /* The session's table is untouched. */
    TU_EQ(SrcMatch_HasExt(&t, wire), 1);
}

TU_TEST(every_mutation_marks_the_table_changed_and_nothing_else_does)
{
    SrcMatch_Table t;
    uint8_t e[SRCMATCH_EXT_LEN] = {1, 1, 1, 1, 1, 1, 1, 1};

    /* An emptied table is a change: a RESET must reach the radio. */
    SrcMatch_Init(&t);
    TU_EQ(t.changed, 1u);
    t.changed = 0u;

    TU_EQ(SrcMatch_HasShort(&t, 1u), 0);
    TU_EQ(t.changed, 0u);
    TU_EQ(SrcMatch_InsertShort(&t, 1u), SRCMATCH_OK);
    TU_EQ(t.changed, 1u);
    t.changed = 0u;

    /* A duplicate insert changes nothing, so it must not look like a change to the app. */
    TU_EQ(SrcMatch_InsertShort(&t, 1u), SRCMATCH_OK);
    TU_EQ(t.changed, 0u);

    TU_EQ(SrcMatch_InsertExt(&t, e), SRCMATCH_OK);
    TU_EQ(t.changed, 1u);
    t.changed = 0u;

    TU_EQ(SrcMatch_RemoveShort(&t, 1u), SRCMATCH_OK);
    TU_EQ(t.changed, 1u);
    t.changed = 0u;

    TU_EQ(SrcMatch_RemoveShort(&t, 1u), SRCMATCH_ABSENT);
    TU_EQ(t.changed, 0u);

    TU_EQ(SrcMatch_SetShortList(&t, 0, 0), SRCMATCH_OK);
    TU_EQ(t.changed, 1u);
}

TU_TEST(a_copy_carries_the_whole_table)
{
    SrcMatch_Table a;
    SrcMatch_Table b;
    uint8_t e[SRCMATCH_EXT_LEN];

    SrcMatch_Init(&a);
    a.enabled = 1u;
    fillShort(&a, 0x0300u, 5);
    fillExt(&a, 0x40u, 2);

    SrcMatch_Init(&b);
    SrcMatch_Copy(&b, &a);

    TU_EQ(b.enabled, 1);
    TU_EQ(b.shortCount, 5);
    TU_EQ(b.extCount, 2);
    TU_EQ(b.changed, a.changed);
    TU_EQ(SrcMatch_HasShort(&b, 0x0304u), 1);
    TU_EQ(SrcMatch_HasShort(&b, 0x0305u), 0);

    for (uint8_t k = 0; k < SRCMATCH_EXT_LEN; ++k)
        e[k] = (uint8_t)(0x41u + k);

    TU_EQ(SrcMatch_HasExt(&b, e), 1);

    /* The copy is independent: a mutation of one must not reach the other. */
    TU_EQ(SrcMatch_RemoveShort(&b, 0x0300u), SRCMATCH_OK);
    TU_EQ(SrcMatch_HasShort(&a, 0x0300u), 1);
}

TU_TEST(encoding_into_a_stream_that_is_too_small_reports_zero)
{
    SrcMatch_Table t;
    uint8_t small[3];
    U_BStream bs;

    SrcMatch_Init(&t);
    fillShort(&t, 0x0500u, 4);

    U_bstream_init(&bs, small, sizeof(small));
    TU_EQ(SrcMatch_EncodeShort(&t, &bs), 0);
    TU_EQ(U_bstream_remaining(&bs), 3); /* nothing was written */
}

int main(void)
{
    static const Tu_Test tests[] = {{"a_fresh_table_is_empty_and_disabled",
                                     a_fresh_table_is_empty_and_disabled},
                                    {"a_short_list_round_trips_through_the_wire_form",
                                     a_short_list_round_trips_through_the_wire_form},
                                    {"an_empty_set_clears_the_list", an_empty_set_clears_the_list},
                                    {"a_list_that_does_not_fit_is_refused_whole",
                                     a_list_that_does_not_fit_is_refused_whole},
                                    {"a_length_that_is_not_a_multiple_of_the_entry_size_is_refused",
                                     a_length_that_is_not_a_multiple_of_the_entry_size_is_refused},
                                    {"the_full_short_pool_refuses_rather_than_evicts",
                                     the_full_short_pool_refuses_rather_than_evicts},
                                    {"the_full_extended_pool_refuses_rather_than_evicts",
                                     the_full_extended_pool_refuses_rather_than_evicts},
                                    {"inserting_what_is_already_there_stores_it_once",
                                     inserting_what_is_already_there_stores_it_once},
                                    {"removing_an_absent_entry_says_so",
                                     removing_an_absent_entry_says_so},
                                    {"removal_keeps_the_other_entries", removal_keeps_the_other_entries},
                                    {"an_extended_list_round_trips_in_wire_order",
                                     an_extended_list_round_trips_in_wire_order},
                                    {"the_radios_copy_holds_the_extended_addresses_in_on_air_order",
                                     the_radios_copy_holds_the_extended_addresses_in_on_air_order},
                                    {"every_mutation_marks_the_table_changed_and_nothing_else_does",
                                     every_mutation_marks_the_table_changed_and_nothing_else_does},
                                    {"a_copy_carries_the_whole_table", a_copy_carries_the_whole_table},
                                    {"encoding_into_a_stream_that_is_too_small_reports_zero",
                                     encoding_into_a_stream_that_is_too_small_reports_zero}};

    Tu_Run("srcmatch", tests, (int)(sizeof(tests) / sizeof(tests[0])));
    return 0;
}
