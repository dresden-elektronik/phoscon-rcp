/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* samr21_basic.h — the deCONZ NVRAM basic set of a ConBee II / RaspBee II (docs/hardware_samr21.md §3).
 *
 * The vendor firmware keeps the address a host programmed in a 32-byte set at the end of the last flash
 * page: magic 0xDE90 (u16, little-endian), id 1 or 2, length 8, the MAC in eight bytes least significant
 * first, 18 reserved bytes and a 16-bit CRC over the first twelve, stored little-endian. id 2 uses
 * CRC-16/MCRF4XX (reflected 0x8408, init 0xFFFF); id 1 uses the vendor's older byte-wise CRC. A MAC that
 * begins 00:50:C2:FF:FF was written most significant first by older modules and is turned round. These
 * are the SAMR21 vendor firmware's rules (nv_basic.c of its source); the AVR boards' set has rules of
 * its own (hal/avr/avr_ident.c), and the two parsers are separate on purpose (docs/architecture.md §2).
 *
 * A pure function over the bytes, no flash access, so the host suite pins it; samr21_nvram.c reads
 * them. */
#ifndef SAMR21_BASIC_H
#define SAMR21_BASIC_H

#include <stdint.h>

/* The last page's last 32 bytes: page 0x3FF of 256 bytes, offset 224. */
#define SAMR21_BASIC_ADDR 0x0003FFE0u
#define SAMR21_BASIC_LEN 32u

/* The set's MAC in deCONZ order, least significant byte first; 1 = a valid set. 0 for a wrong magic,
 * id or length, or a CRC that does not match; mac8 is left alone then. */
int Samr21Basic_Mac(const uint8_t set[SAMR21_BASIC_LEN], uint8_t mac8[8]);

#endif /* SAMR21_BASIC_H */
