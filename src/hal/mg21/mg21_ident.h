/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mg21_ident.h — the ConBee III's factory EUI-64, from the manufacturing token in the user data page
 * (docs/hardware_mg21.md §10.2).
 *
 * The token is Silicon Labs' TOKEN_MFG_CUSTOM_EUI_64: eight bytes, least significant first, at the user
 * data page's +0x002 (the SDK's MFG_CUSTOM_EUI_64_LOCATION, USERDATA_TOKENS | 0x002). It is the factory
 * MAC the vendor firmware reports, and the bootloader's DeviceConfig carries the same eight bytes. A pure
 * function over the bytes, no register, so the host suite pins it; mg21_nvram.c supplies them. */
#ifndef MG21_IDENT_H
#define MG21_IDENT_H

#include <stdint.h>

#define MG21_IDENT_EUI64_OFFSET 0x002u /* from the user data page's base */
#define MG21_IDENT_EUI64_LEN 8u

/* The token in Spinel HWADDR order, most significant byte first; 1 = a factory address. 0 for an
 * erased token (all 0xFF), an all-zero one and a group address (I/G bit set), the cases hal.h's
 * Nvram_FactoryEui64 excludes; eui8 is left alone then. */
int Mg21Ident_Eui64(const uint8_t token[8], uint8_t eui8[8]);

#endif /* MG21_IDENT_H */
