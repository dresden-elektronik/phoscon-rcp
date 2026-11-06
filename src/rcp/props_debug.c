/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* props_debug.c - packing of debug records for the RCP log property (props_debug.h). */
#include "props_debug.h"

uint16_t PropsDebug_Pack(uint8_t *dst, uint16_t cap, const uint8_t *records, uint16_t count)
{
    uint16_t payload;
    uint16_t need;
    uint16_t i;

    if (dst == 0 || records == 0 || count == 0u || count > PROPS_DEBUG_MAX)
    {
        return 0u;
    }

    payload = (uint16_t)(count * PROPS_DEBUG_RECORD);
    need = (uint16_t)(PROPS_DEBUG_HEADER + payload);

    if (cap < need)
    {
        return 0u;
    }

    dst[0] = (uint8_t)(count & 0xFFu);
    dst[1] = (uint8_t)(count >> 8);

    for (i = 0u; i < payload; ++i)
    {
        dst[PROPS_DEBUG_HEADER + i] = records[i];
    }

    return need;
}

int PropsDebug_EnableValue(const uint8_t *value, uint16_t len, uint8_t *on)
{
    if (on == 0)
    {
        return 0;
    }

    if (len == 0u)
    {
        return 1;
    }

    if (len == 1u && value != 0)
    {
        *on = (value[0] != 0u) ? 1u : 0u;
        return 1;
    }

    return 0;
}
