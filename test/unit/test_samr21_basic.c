/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* test_samr21_basic.c — the ConBee II / RaspBee II NVRAM basic set (hal/samr21/samr21_basic.h).

   The id 2 CRC of the MAC 00:21:2E:FF:FF:00:1C:53 stored least significant first is the non-volatile
   memory application note's own example, 0xCD72; the id 1 CRC (0xF4E2) and the old-prefix one (0x72AE)
   were computed outside this tree's C code from the vendor's two algorithms, so the suite does not check
   the parser against itself. */

#include <stdint.h>

#include "samr21_basic.h"
#include "tu.h"

static const uint8_t MAC_LE[8] = {0x53, 0x1C, 0x00, 0xFF, 0xFF, 0x2E, 0x21, 0x00};

/* The 12 bytes the CRC covers, 18 reserved, the CRC as the two stored bytes. */
static void basicSet(uint8_t set[32], uint8_t magicLo, uint8_t id, uint8_t len, const uint8_t mac[8],
                     uint8_t crc0, uint8_t crc1)
{
    uint8_t i;

    for (i = 0u; i < 32u; ++i)
        set[i] = 0xFFu;

    set[0] = magicLo;
    set[1] = 0xDEu;
    set[2] = id;
    set[3] = len;

    for (i = 0u; i < 8u; ++i)
        set[4u + i] = mac[i];

    set[30] = crc0;
    set[31] = crc1;
}

static int untouched(const uint8_t mac[8])
{
    uint8_t i;

    for (i = 0u; i < 8u; ++i)
    {
        if (mac[i] != 0xA5u)
            return 0;
    }

    return 1;
}

static void fill(uint8_t mac[8])
{
    uint8_t i;

    for (i = 0u; i < 8u; ++i)
        mac[i] = 0xA5u;
}

TU_TEST(id2_the_application_notes_example)
{
    uint8_t set[32];
    uint8_t mac[8];
    uint8_t i;

    basicSet(set, 0x90u, 2u, 8u, MAC_LE, 0x72u, 0xCDu);
    TU_EQ(Samr21Basic_Mac(set, mac), 1);

    for (i = 0u; i < 8u; ++i)
        TU_EQ(mac[i], MAC_LE[i]); /* deCONZ order is the stored order */
}

TU_TEST(id1_the_vendor_crc_little_endian)
{
    uint8_t set[32];
    uint8_t mac[8];

    basicSet(set, 0x90u, 1u, 8u, MAC_LE, 0xE2u, 0xF4u);
    TU_EQ(Samr21Basic_Mac(set, mac), 1);
    TU_EQ(mac[0], 0x53u);
    TU_EQ(mac[7], 0x00u);

    /* The SAMR21 firmware compares the CRC as the packed struct's little-endian field only. */
    fill(mac);
    basicSet(set, 0x90u, 1u, 8u, MAC_LE, 0xF4u, 0xE2u);
    TU_EQ(Samr21Basic_Mac(set, mac), 0);
    TU_CHECK(untouched(mac));
}

TU_TEST(the_old_big_endian_prefix_is_turned_round)
{
    static const uint8_t oldBe[8] = {0x00, 0x50, 0xC2, 0xFF, 0xFF, 0x01, 0x02, 0x03};
    uint8_t set[32];
    uint8_t mac[8];

    basicSet(set, 0x90u, 2u, 8u, oldBe, 0xAEu, 0x72u);
    TU_EQ(Samr21Basic_Mac(set, mac), 1);
    TU_EQ(mac[0], 0x03u);
    TU_EQ(mac[7], 0x00u);
}

TU_TEST(a_damaged_set_is_refused_and_writes_nothing)
{
    uint8_t set[32];
    uint8_t mac[8];

    fill(mac);
    basicSet(set, 0x90u, 2u, 8u, MAC_LE, 0x73u, 0xCDu); /* CRC off by one */
    TU_EQ(Samr21Basic_Mac(set, mac), 0);
    basicSet(set, 0x00u, 2u, 8u, MAC_LE, 0x6Au, 0x76u); /* 0xDE00, with its own CRC: not this board's */
    TU_EQ(Samr21Basic_Mac(set, mac), 0);
    basicSet(set, 0x90u, 3u, 8u, MAC_LE, 0x72u, 0xCDu);
    TU_EQ(Samr21Basic_Mac(set, mac), 0);
    basicSet(set, 0x90u, 2u, 7u, MAC_LE, 0x72u, 0xCDu);
    TU_EQ(Samr21Basic_Mac(set, mac), 0);
    TU_CHECK(untouched(mac));

    /* The erased page. */
    {
        uint8_t i;

        for (i = 0u; i < 32u; ++i)
            set[i] = 0xFFu;
    }
    TU_EQ(Samr21Basic_Mac(set, mac), 0);
    TU_EQ(Samr21Basic_Mac(0, mac), 0);
    TU_EQ(Samr21Basic_Mac(set, 0), 0);
    TU_CHECK(untouched(mac));
}

static const Tu_Test tests[] = {
    {"id2_the_application_notes_example", id2_the_application_notes_example},
    {"id1_the_vendor_crc_little_endian", id1_the_vendor_crc_little_endian},
    {"the_old_big_endian_prefix_is_turned_round", the_old_big_endian_prefix_is_turned_round},
    {"a_damaged_set_is_refused_and_writes_nothing", a_damaged_set_is_refused_and_writes_nothing},
};

int main(void)
{
    Tu_Run("samr21_basic", tests, (int)(sizeof(tests) / sizeof(tests[0])));
    return 0;
}
