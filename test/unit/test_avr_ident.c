/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* test_avr_ident.c — the ConBee I / RaspBee I factory EUI-64 from the EEPROM's NV basic set
   (hal/avr/avr_ident.h).

   The id 2 vector is the non-volatile memory application note's own recovery example: MAC
   00:21:2E:FF:FF:00:1C:53 stored least significant first, CRC 0xCD72. The id 1 CRC (0xB8AE for the same
   MAC stored most significant first) and the other CRCs here were computed outside this tree's C code,
   from the note's listings, so the suite does not check the parser against itself. */

#include <stdint.h>

#include "avr_ident.h"
#include "tu.h"

/* A 32-byte set: the 12 bytes the CRC covers, 18 reserved, the CRC as the two stored bytes. */
static void basicSet(uint8_t set[32], const uint8_t head[12], uint8_t crc0, uint8_t crc1)
{
    uint8_t i;

    for (i = 0u; i < 32u; ++i)
        set[i] = (i < 12u) ? head[i] : 0xFFu;

    set[30] = crc0;
    set[31] = crc1;
}

static const uint8_t AN_V2[12] = {0x90, 0xDE, 0x02, 0x08, 0x53, 0x1C, 0x00, 0xFF, 0xFF, 0x2E, 0x21, 0x00};

static int euiIs(const uint8_t eui[8], const uint8_t want[8])
{
    uint8_t i;

    for (i = 0u; i < 8u; ++i)
        if (eui[i] != want[i])
            return 0;

    return 1;
}

static const uint8_t DE_MAC[8] = {0x00, 0x21, 0x2E, 0xFF, 0xFF, 0x00, 0x1C, 0x53};

TU_TEST(the_set_is_the_last_32_bytes_of_the_eeprom)
{
    TU_EQ(AVR_IDENT_BASIC_ADDR, 0x2000u - 32u);
    TU_EQ(AVR_IDENT_BASIC_LEN, 32u);
}

TU_TEST(the_notes_id2_example_reads_most_significant_byte_first)
{
    uint8_t set[32];
    uint8_t eui[8];

    basicSet(set, AN_V2, 0x72, 0xCD);
    TU_EQ(AvrIdent_Eui64(set, eui), 1);
    TU_EQ(euiIs(eui, DE_MAC), 1);
}

TU_TEST(an_id2_crc_is_little_endian_only)
{
    uint8_t set[32];
    uint8_t eui[8];

    basicSet(set, AN_V2, 0xCD, 0x72);
    TU_EQ(AvrIdent_Eui64(set, eui), 0);
}

TU_TEST(id1_stores_the_mac_most_significant_first_and_either_crc_order_passes)
{
    static const uint8_t head[12] = {0x90, 0xDE, 0x01, 0x08, 0x00, 0x21, 0x2E, 0xFF, 0xFF, 0x00, 0x1C, 0x53};
    uint8_t set[32];
    uint8_t eui[8];

    basicSet(set, head, 0xB8, 0xAE);
    TU_EQ(AvrIdent_Eui64(set, eui), 1);
    TU_EQ(euiIs(eui, DE_MAC), 1);

    basicSet(set, head, 0xAE, 0xB8);
    TU_EQ(AvrIdent_Eui64(set, eui), 1);

    basicSet(set, head, 0xAE, 0xB9);
    TU_EQ(AvrIdent_Eui64(set, eui), 0);
}

TU_TEST(the_early_magic_is_checked_against_the_intended_one)
{
    static const uint8_t head[12] = {0x00, 0xDE, 0x02, 0x08, 0x53, 0x1C, 0x00, 0xFF, 0xFF, 0x2E, 0x21, 0x00};
    uint8_t set[32];
    uint8_t eui[8];

    /* Stored 0xDE00, CRC taken over 0xDE90: the note's known issue. */
    basicSet(set, head, 0x72, 0xCD);
    TU_EQ(AvrIdent_Eui64(set, eui), 1);
    TU_EQ(euiIs(eui, DE_MAC), 1);

    /* A CRC over the bytes as stored is not the vendor's. */
    basicSet(set, head, 0x6A, 0x76);
    TU_EQ(AvrIdent_Eui64(set, eui), 0);
}

TU_TEST(the_older_big_endian_prefix_is_taken_as_stored)
{
    static const uint8_t head[12] = {0x90, 0xDE, 0x02, 0x08, 0x00, 0x50, 0xC2, 0xFF, 0xFF, 0x12, 0x34, 0x56};
    static const uint8_t want[8] = {0x00, 0x50, 0xC2, 0xFF, 0xFF, 0x12, 0x34, 0x56};
    uint8_t set[32];
    uint8_t eui[8];

    basicSet(set, head, 0x05, 0xFF);
    TU_EQ(AvrIdent_Eui64(set, eui), 1);
    TU_EQ(euiIs(eui, want), 1);
}

TU_TEST(a_damaged_set_is_none_and_leaves_the_output_alone)
{
    uint8_t set[32];
    uint8_t eui[8] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
    uint8_t i;

    for (i = 0u; i < 32u; ++i)
        set[i] = 0xFFu;
    TU_EQ(AvrIdent_Eui64(set, eui), 0); /* erased EEPROM */

    basicSet(set, AN_V2, 0x73, 0xCD);
    TU_EQ(AvrIdent_Eui64(set, eui), 0); /* CRC off by one */

    basicSet(set, AN_V2, 0x72, 0xCD);
    set[2] = 0x03u;
    TU_EQ(AvrIdent_Eui64(set, eui), 0); /* unknown id */

    basicSet(set, AN_V2, 0x72, 0xCD);
    set[3] = 0x07u;
    TU_EQ(AvrIdent_Eui64(set, eui), 0); /* length */

    basicSet(set, AN_V2, 0x72, 0xCD);
    set[1] = 0xDFu;
    TU_EQ(AvrIdent_Eui64(set, eui), 0); /* magic */

    basicSet(set, AN_V2, 0x72, 0xCD);
    set[5] = 0x1Du;
    TU_EQ(AvrIdent_Eui64(set, eui), 0); /* a MAC byte the CRC no longer covers */

    TU_EQ(eui[0], 0xAAu);
    TU_EQ(eui[7], 0xAAu);
}

TU_TEST(the_reserved_bytes_are_outside_the_crc)
{
    uint8_t set[32];
    uint8_t eui[8];

    basicSet(set, AN_V2, 0x72, 0xCD);
    set[12] = 0x00u;
    set[29] = 0x5Au;
    TU_EQ(AvrIdent_Eui64(set, eui), 1);
}

static const Tu_Test tests[] = {
    {"the_set_is_the_last_32_bytes_of_the_eeprom", the_set_is_the_last_32_bytes_of_the_eeprom},
    {"the_notes_id2_example_reads_most_significant_byte_first", the_notes_id2_example_reads_most_significant_byte_first},
    {"an_id2_crc_is_little_endian_only", an_id2_crc_is_little_endian_only},
    {"id1_stores_the_mac_most_significant_first_and_either_crc_order_passes",
     id1_stores_the_mac_most_significant_first_and_either_crc_order_passes},
    {"the_early_magic_is_checked_against_the_intended_one", the_early_magic_is_checked_against_the_intended_one},
    {"the_older_big_endian_prefix_is_taken_as_stored", the_older_big_endian_prefix_is_taken_as_stored},
    {"a_damaged_set_is_none_and_leaves_the_output_alone", a_damaged_set_is_none_and_leaves_the_output_alone},
    {"the_reserved_bytes_are_outside_the_crc", the_reserved_bytes_are_outside_the_crc},
};

int main(void)
{
    Tu_Run("avr_ident", tests, (int)(sizeof(tests) / sizeof(tests[0])));
    return 0;
}
