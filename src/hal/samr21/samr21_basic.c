/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* samr21_basic.c — the deCONZ NVRAM basic set's MAC (samr21_basic.h). */
#include "samr21_basic.h"

#include "u_bstream.h"

#define BASIC_MAGIC 0xDE90u
#define BASIC_ID_V1 1u
#define BASIC_ID_V2 2u
#define BASIC_MAC_LEN 8u
#define BASIC_HDR_LEN 4u
#define BASIC_CRC_SPAN (BASIC_HDR_LEN + BASIC_MAC_LEN)
#define BASIC_CRC_AT 30u

/* CRC-16/MCRF4XX: reflected polynomial 0x8408, init 0xFFFF, no final XOR (id 2). */
static uint16_t basicCrcV2(const uint8_t *buf, uint8_t len)
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

/* The vendor's older CRC for id 1: two byte-wide halves. */
static uint16_t basicCrcV1(const uint8_t *buf, uint8_t len)
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

int Samr21Basic_Mac(const uint8_t set[SAMR21_BASIC_LEN], uint8_t mac8[8])
{
    uint8_t span[BASIC_CRC_SPAN];
    U_BStream bs;
    uint16_t magic;
    uint16_t stored;
    uint16_t crc;
    uint8_t id;
    uint8_t len;
    int reversed;
    uint8_t i;

    if (set == 0 || mac8 == 0)
        return 0;

    U_bstream_init(&bs, (void *)set, SAMR21_BASIC_LEN);
    U_bstream_get_bytes(&bs, span, BASIC_CRC_SPAN);
    U_bstream_seek(&bs, BASIC_CRC_AT);
    stored = U_bstream_get_u16_le(&bs);

    if (bs.status != U_BSTREAM_OK)
        return 0;

    U_bstream_init(&bs, span, BASIC_CRC_SPAN);
    magic = U_bstream_get_u16_le(&bs);
    id = U_bstream_get_u8(&bs);
    len = U_bstream_get_u8(&bs);

    if (magic != BASIC_MAGIC || len != BASIC_MAC_LEN)
        return 0;

    if (id == BASIC_ID_V2)
        crc = basicCrcV2(span, BASIC_CRC_SPAN);
    else if (id == BASIC_ID_V1)
        crc = basicCrcV1(span, BASIC_CRC_SPAN);
    else
        return 0;

    if (crc != stored)
        return 0;

    reversed = (span[4] == 0x00u && span[5] == 0x50u && span[6] == 0xC2u && span[7] == 0xFFu &&
                span[8] == 0xFFu);

    for (i = 0u; i < BASIC_MAC_LEN; ++i)
        mac8[i] = reversed ? span[BASIC_HDR_LEN + BASIC_MAC_LEN - 1u - i] : span[BASIC_HDR_LEN + i];

    return 1;
}
