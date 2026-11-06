/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* test_mg21_ident.c — the ConBee III's factory EUI-64 from its manufacturing token
   (hal/mg21/mg21_ident.h).

   The token bytes are the vendor's: alpha1Config.bin, a user data page image the ConBee III firmware tree
   ships, holds them at +0x002 and the same eight bytes as the DeviceConfig MAC at +0x318; the address the
   vendor firmware reports for a stick is the token read most significant byte first. */

#include <stdint.h>

#include "mg21_ident.h"
#include "tu.h"

TU_TEST(the_token_offset_is_the_sdks)
{
    /* MFG_CUSTOM_EUI_64_LOCATION = USERDATA_TOKENS | 0x002, token-manufacturing-series-2.h. */
    TU_EQ(MG21_IDENT_EUI64_OFFSET, 0x002u);
    TU_EQ(MG21_IDENT_EUI64_LEN, 8u);
}

TU_TEST(the_token_reads_most_significant_byte_first)
{
    static const uint8_t alpha1[8] = {0xE1, 0xFF, 0xEA, 0xFD, 0xFF, 0xEF, 0x12, 0x02};
    static const uint8_t stick[8] = {0xFB, 0xC5, 0x00, 0xFF, 0xFF, 0x2E, 0x21, 0x00};
    uint8_t eui[8];

    TU_EQ(Mg21Ident_Eui64(alpha1, eui), 1);
    TU_EQ(eui[0], 0x02u);
    TU_EQ(eui[1], 0x12u);
    TU_EQ(eui[7], 0xE1u);

    /* 00:21:2E:FF:FF:00:C5:FB, a dresden elektronik address in the order the vendor firmware sends it. */
    TU_EQ(Mg21Ident_Eui64(stick, eui), 1);
    TU_EQ(eui[0], 0x00u);
    TU_EQ(eui[1], 0x21u);
    TU_EQ(eui[2], 0x2Eu);
    TU_EQ(eui[5], 0x00u);
    TU_EQ(eui[6], 0xC5u);
    TU_EQ(eui[7], 0xFBu);
}

TU_TEST(no_address_is_reported_as_none)
{
    static const uint8_t erased[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    static const uint8_t zero[8] = {0};
    static const uint8_t group[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x01};
    uint8_t eui[8] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};

    TU_EQ(Mg21Ident_Eui64(erased, eui), 0);
    TU_EQ(Mg21Ident_Eui64(zero, eui), 0);
    TU_EQ(Mg21Ident_Eui64(group, eui), 0);
    TU_EQ(eui[0], 0xAAu); /* untouched when there is no address */
}

TU_TEST(one_programmed_byte_is_an_address)
{
    static const uint8_t partly[8] = {0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    uint8_t eui[8];

    TU_EQ(Mg21Ident_Eui64(partly, eui), 1);
    TU_EQ(eui[0], 0x00u);
    TU_EQ(eui[7], 0x01u);
}

static const Tu_Test tests[] = {
    {"the_token_offset_is_the_sdks", the_token_offset_is_the_sdks},
    {"the_token_reads_most_significant_byte_first", the_token_reads_most_significant_byte_first},
    {"no_address_is_reported_as_none", no_address_is_reported_as_none},
    {"one_programmed_byte_is_an_address", one_programmed_byte_is_an_address},
};

int main(void)
{
    Tu_Run("mg21_ident", tests, (int)(sizeof(tests) / sizeof(tests[0])));
    return 0;
}
