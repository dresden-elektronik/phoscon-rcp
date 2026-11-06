/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* avr_ident.c — the factory EUI-64 out of the EEPROM's NV basic set (avr_ident.h). */
#include "avr_ident.h"

#include "u_bstream.h"

#define BASIC_MAGIC 0xDE90u
#define BASIC_MAGIC_EARLY 0xDE00u /* written by early modules, CRC still over 0xDE90 */
#define BASIC_ID_V1 1u
#define BASIC_ID_V2 2u
#define BASIC_MAC_LEN 8u
#define BASIC_HDR_LEN 4u
#define BASIC_CRC_SPAN (BASIC_HDR_LEN + BASIC_MAC_LEN)
#define BASIC_CRC_AT 30u

/* CRC-16/MCRF4XX: reflected polynomial 0x8408, init 0xFFFF, no final XOR (id 2). */
static uint16_t identCrcV2(const uint8_t *buf, uint8_t len)
{
    uint16_t crc = 0xFFFFu;
    uint8_t i;
    uint8_t bit;

    for (i = 0u; i < len; ++i)
    {
        crc = (uint16_t)(crc ^ buf[i]);

        for (bit = 0u; bit < 8u; ++bit)
            crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0x8408u) : (uint16_t)(crc >> 1);
    }

    return crc;
}

/* The vendor's CRC-16 for id 1, two byte-wide halves, as the non-volatile memory application note
 * gives it. */
static uint16_t identCrcV1(const uint8_t *buf, uint8_t len)
{
    uint8_t high = 0xFFu;
    uint8_t low = 0xFFu;
    uint8_t i;

    for (i = 0u; i < len; ++i)
    {
        uint8_t t;

        high = (uint8_t)(high ^ buf[i]);
        high = (uint8_t)(high ^ ((high >> 4) & 0x0Fu));
        t = (uint8_t)(high >> 3);
        low = (uint8_t)(low ^ (t & 0x1Fu));
        low = (uint8_t)(low ^ ((t >> 1) & 0x0Fu));
        t = (uint8_t)((t & 0x0Eu) ^ high);
        high = low;
        low = t;
    }

    return (uint16_t)((uint16_t)high << 8 | low);
}

int AvrIdent_Eui64(const uint8_t set[AVR_IDENT_BASIC_LEN], uint8_t eui8[8])
{
    uint8_t span[BASIC_CRC_SPAN];
    uint8_t stored[2];
    U_BStream bs;
    uint16_t magic;
    uint8_t id;
    uint8_t len;
    int bigEndianMac;
    uint8_t all = 0xFFu;
    uint8_t any = 0x00u;
    uint8_t i;

    if (set == 0 || eui8 == 0)
        return 0;

    U_bstream_init(&bs, (void *)set, AVR_IDENT_BASIC_LEN);
    U_bstream_get_bytes(&bs, span, BASIC_CRC_SPAN);
    U_bstream_init(&bs, span, BASIC_CRC_SPAN);
    magic = U_bstream_get_u16_le(&bs);
    id = U_bstream_get_u8(&bs);
    len = U_bstream_get_u8(&bs);
    U_bstream_init(&bs, (void *)set, AVR_IDENT_BASIC_LEN);
    U_bstream_seek(&bs, BASIC_CRC_AT);
    U_bstream_get_bytes(&bs, stored, 2u);

    if (bs.status != U_BSTREAM_OK || len != BASIC_MAC_LEN)
        return 0;

    if (magic == BASIC_MAGIC_EARLY)
        span[0] = (uint8_t)(BASIC_MAGIC & 0xFFu); /* the CRC was taken over the intended magic */
    else if (magic != BASIC_MAGIC)
        return 0;

    if (id == BASIC_ID_V2)
    {
        uint16_t crc = identCrcV2(span, BASIC_CRC_SPAN);

        if (stored[0] != (uint8_t)(crc & 0xFFu) || stored[1] != (uint8_t)(crc >> 8))
            return 0;

        /* Least significant first, except the older modules that wrote it the other way round and
         * the legacy firmware recognises by their 00:50:C2:FF:FF prefix. */
        bigEndianMac = (span[4] == 0x00u && span[5] == 0x50u && span[6] == 0xC2u && span[7] == 0xFFu &&
                        span[8] == 0xFFu);
    }
    else if (id == BASIC_ID_V1)
    {
        uint16_t crc = identCrcV1(span, BASIC_CRC_SPAN);

        /* The note stores it big-endian, the legacy firmware compares it as a little-endian field:
         * either order is the vendor's. */
        if (!((stored[0] == (uint8_t)(crc >> 8) && stored[1] == (uint8_t)(crc & 0xFFu)) ||
              (stored[0] == (uint8_t)(crc & 0xFFu) && stored[1] == (uint8_t)(crc >> 8))))
            return 0;

        bigEndianMac = 1;
    }
    else
    {
        return 0;
    }

    for (i = 0u; i < BASIC_MAC_LEN; ++i)
    {
        all = (uint8_t)(all & span[BASIC_HDR_LEN + i]);
        any = (uint8_t)(any | span[BASIC_HDR_LEN + i]);
    }

    {
        /* The I/G bit is bit 0 of the first octet in display order. */
        uint8_t first = bigEndianMac ? span[BASIC_HDR_LEN] : span[BASIC_HDR_LEN + BASIC_MAC_LEN - 1u];

        if (all == 0xFFu || any == 0x00u || (first & 0x01u) != 0u)
            return 0;
    }

    for (i = 0u; i < BASIC_MAC_LEN; ++i)
        eui8[i] = bigEndianMac ? span[BASIC_HDR_LEN + i] : span[BASIC_HDR_LEN + BASIC_MAC_LEN - 1u - i];

    return 1;
}
