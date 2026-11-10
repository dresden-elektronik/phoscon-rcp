/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* test_esp32_ident.c — an ESP32 part's factory EUI-64 from its eFuse MAC words (hal/esp32/esp32_ident.h).

   The vector is ESP-IDF's own example in mac_addr.c: base MAC 60:55:f9:f7:2c:a2 with MAC_EXT ff:fe becomes
   60:55:f9:ff:fe:f7:2c:a2. The eFuse words are what that MAC is in block 1: the low 32 bits of the 48, then
   the high 16 with MAC_EXT above them, its first octet in the low byte (ESP-IDF's eFuse register header for
   the C6, EFUSE_RD_MAC_SPI_SYS_0 and _1). The image has not yet read a board's words through this path. */

#include <stdint.h>

#include "esp32_ident.h"
#include "tu.h"

TU_TEST(esp_idfs_example_reads_as_it_says)
{
    uint8_t eui[8];

    TU_EQ(Esp32Ident_Eui64(0xF9F72CA2u, 0xFEFF6055u, eui), 1);
    TU_EQ(eui[0], 0x60u);
    TU_EQ(eui[1], 0x55u);
    TU_EQ(eui[2], 0xF9u);
    TU_EQ(eui[3], 0xFFu);
    TU_EQ(eui[4], 0xFEu);
    TU_EQ(eui[5], 0xF7u);
    TU_EQ(eui[6], 0x2Cu);
    TU_EQ(eui[7], 0xA2u);
}

TU_TEST(mac_ext_is_taken_as_it_is_not_as_ff_fe)
{
    uint8_t eui[8];

    TU_EQ(Esp32Ident_Eui64(0x00000001u, 0x12340002u, eui), 1);
    TU_EQ(eui[3], 0x34u);
    TU_EQ(eui[4], 0x12u);
    TU_EQ(eui[7], 0x01u);
    TU_EQ(eui[1], 0x02u);
}

TU_TEST(no_address_is_reported_as_none)
{
    uint8_t eui[8] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};

    TU_EQ(Esp32Ident_Eui64(0u, 0u, eui), 0);
    TU_EQ(Esp32Ident_Eui64(0u, 0xFEFF0000u, eui), 0); /* MAC_EXT alone is not a MAC */
    TU_EQ(Esp32Ident_Eui64(0xFFFFFFFFu, 0xFFFFFFFFu, eui), 0);
    TU_EQ(Esp32Ident_Eui64(0x00000001u, 0x00000100u, eui), 0); /* group: I/G set in the first octet */
    TU_EQ(eui[0], 0xAAu); /* untouched when there is no address */
}

static const Tu_Test tests[] = {
    {"esp_idfs_example_reads_as_it_says", esp_idfs_example_reads_as_it_says},
    {"mac_ext_is_taken_as_it_is_not_as_ff_fe", mac_ext_is_taken_as_it_is_not_as_ff_fe},
    {"no_address_is_reported_as_none", no_address_is_reported_as_none},
};

int main(void)
{
    Tu_Run("esp32_ident", tests, (int)(sizeof(tests) / sizeof(tests[0])));
    return 0;
}
