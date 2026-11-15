/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* esp32_ident.c — the factory EUI-64 out of the eFuse MAC words (esp32_ident.h). */
#include "esp32_ident.h"

int Esp32Ident_Eui64(uint32_t mac0, uint32_t mac1, uint8_t eui8[8])
{
    uint32_t high = mac1 & 0xFFFFu;
    uint32_t ext = mac1 >> 16;

    if ((mac0 == 0u && high == 0u) || (mac0 == 0xFFFFFFFFu && high == 0xFFFFu))
        return 0;

    /* The I/G bit is bit 0 of the first octet in display order, the MAC's [40]. */
    if ((high & 0x0100u) != 0u)
        return 0;

    eui8[0] = (uint8_t)(high >> 8);
    eui8[1] = (uint8_t)high;
    eui8[2] = (uint8_t)(mac0 >> 24);
    /* MAC_EXT is stored as the MAC is, its first octet in the high byte. */
    eui8[3] = (uint8_t)(ext >> 8);
    eui8[4] = (uint8_t)ext;
    eui8[5] = (uint8_t)(mac0 >> 16);
    eui8[6] = (uint8_t)(mac0 >> 8);
    eui8[7] = (uint8_t)mac0;
    return 1;
}
