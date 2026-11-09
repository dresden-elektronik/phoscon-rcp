/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* props_debug.c - packing of debug records for the RCP log property (props_debug.h). */
#include "props_debug.h"

#include "u_bstream.h"

uint16_t PropsDebug_Pack(uint8_t *dst, uint16_t cap, const uint8_t *records, uint16_t count)
{
    U_BStream bs;

    if (dst == 0 || records == 0 || count == 0u || count > PROPS_DEBUG_MAX)
    {
        return 0u;
    }

    U_bstream_init(&bs, dst, cap);

    if (!U_bstream_fits(&bs, PROPS_DEBUG_HEADER + (unsigned long)count * PROPS_DEBUG_RECORD))
    {
        return 0u;
    }

    U_bstream_put_u16_le(&bs, count);
    U_bstream_put_bytes(&bs, records, (unsigned long)count * PROPS_DEBUG_RECORD);
    return (uint16_t)U_bstream_pos(&bs);
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
