/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* esp32_ident.h — an ESP32 802.15.4 part's factory EUI-64, from its eFuse MAC (docs/hardware_esp32.md §12.2).
 *
 * The factory MAC is 48 bits in eFuse block 1, read as two words: the low 32 bits, and a word holding
 * the high 16 in [15:0] and MAC_EXT in [31:16]. The EUI-64 is the MAC's top three octets, MAC_EXT's two
 * octets high byte first, as the MAC's own are stored, then the MAC's bottom three - the composition ESP-IDF's mac_addr.c makes for
 * the 802.15.4 interface. A pure function over the two words, no register, so the host suite pins it;
 * esp32_nvram.c supplies them. */
#ifndef ESP32_IDENT_H
#define ESP32_IDENT_H

#include <stdint.h>

#define ESP32_IDENT_EUI64_LEN 8u

/* The EUI-64 in Spinel HWADDR order, most significant byte first; 1 = a factory address. 0 for an
 * unprogrammed MAC (all zero, or all ones) and for a group address (I/G bit set); eui8 is left alone
 * then. */
int Esp32Ident_Eui64(uint32_t mac0, uint32_t mac1, uint8_t eui8[8]);

#endif /* ESP32_IDENT_H */
