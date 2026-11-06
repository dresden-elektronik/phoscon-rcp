/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* avr_ident.h — the ConBee I / RaspBee I factory EUI-64, from the NV basic set at the end of the
 * EEPROM (docs/hardware_avr.md §11.3).
 *
 * The basic set is the last 32 bytes of the 8 KiB EEPROM: magic 0xDE90 (u16 LE), id, length 8, the
 * MAC, 18 reserved bytes, and a CRC over magic, id, length and MAC. id 2 stores the MAC least
 * significant byte first and its CRC as CRC-16/MCRF4XX (reflected 0x8408, init 0xFFFF) little-endian;
 * id 1 stores the MAC most significant byte first and its CRC with the vendor's own CRC-16. A pure
 * function over the bytes, no register, so the host suite pins it; the AVR glue reads the EEPROM. */
#ifndef AVR_IDENT_H
#define AVR_IDENT_H

#include <stdint.h>

#define AVR_IDENT_BASIC_ADDR 0x1FE0u /* EEPROM address: (E2END + 1) - 32 */
#define AVR_IDENT_BASIC_LEN 32u

/* The MAC in Spinel HWADDR order, most significant byte first; 1 = a valid basic set holding a factory
 * address. 0 for a wrong magic, id or length, a CRC mismatch, and an erased, all-zero or group
 * address (I/G bit set), the cases hal.h's Nvram_FactoryEui64 excludes; eui8 is left alone then. */
int AvrIdent_Eui64(const uint8_t set[AVR_IDENT_BASIC_LEN], uint8_t eui8[8]);

#endif /* AVR_IDENT_H */
