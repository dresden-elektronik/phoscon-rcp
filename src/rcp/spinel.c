/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* spinel.c — Spinel framing (src/rcp/spinel.h, docs/protocol.md §4). */
#include "spinel.h"

static int spinelCmdHasProperty(uint8_t cmd)
{
    return (cmd == SPINEL_CMD_PROP_VALUE_GET || cmd == SPINEL_CMD_PROP_VALUE_SET ||
            cmd == SPINEL_CMD_PROP_VALUE_INSERT || cmd == SPINEL_CMD_PROP_VALUE_REMOVE ||
            cmd == SPINEL_CMD_PROP_VALUE_IS || cmd == SPINEL_CMD_PROP_VALUE_INSERTED ||
            cmd == SPINEL_CMD_PROP_VALUE_REMOVED);
}

uint8_t Spinel_Header(uint8_t tid)
{
    /* FLG 0b10, NLI/IID 0, TID in the low nibble (docs/protocol.md §4). */
    return (uint8_t)(SPINEL_FLG_VALUE | (tid & SPINEL_TID_MASK));
}

int Spinel_HeaderTid(uint8_t header)
{
    if ((header & SPINEL_FLG_MASK) != SPINEL_FLG_VALUE)
        return -1;

    return (int)(header & SPINEL_TID_MASK);
}

int Spinel_PackedRead(const uint8_t *data, uint16_t len, uint32_t *value, uint16_t *used)
{
    uint32_t v = 0u;
    uint16_t i;
    uint16_t shift = 0u;

    if (data == 0 || value == 0)
        return 0;
    /* Five bytes is the most a 32-bit value can need at 7 bits per byte (docs/protocol.md §4). */
    for (i = 0u; i < len && i < 5u; ++i)
    {
        v |= (uint32_t)(data[i] & 0x7Fu) << shift;

        if ((data[i] & 0x80u) == 0u)
        {
            *value = v;

            if (used != 0)
                *used = (uint16_t)(i + 1u);

            return 1;
        }

        shift = (uint16_t)(shift + 7u);
    }

    return 0;
}

uint16_t Spinel_PackedSize(uint32_t value)
{
    uint16_t n = 1u;

    while (value >= 0x80u)
    {
        value >>= 7;
        n++;
    }

    return n;
}

uint16_t Spinel_PackedWrite(uint8_t *dst, uint16_t max, uint32_t value)
{
    uint16_t n = 0u;

    if (dst == 0)
        return 0u;

    do
    {
        if (n >= max)
            return 0u;

        dst[n] = (uint8_t)(value & 0x7Fu);
        value >>= 7;

        if (value != 0u)
            dst[n] = (uint8_t)(dst[n] | 0x80u);

        n++;
    } while (value != 0u);
    return n;
}

int Spinel_Decode(const uint8_t *data, uint16_t len, Spinel_Frame *out)
{
    uint16_t used = 0u;
    uint32_t prop = 0u;
    int tid;

    if (data == 0 || out == 0 || len < 2u)
        return 0;

    tid = Spinel_HeaderTid(data[0]);

    if (tid < 0)
        return 0;

    out->tid = (uint8_t)tid;
    out->cmd = data[1];
    out->prop = 0u;
    out->value = 0;
    out->valueLen = 0u;

    if (!spinelCmdHasProperty(out->cmd))
    {
        out->value = &data[2];
        out->valueLen = (uint16_t)(len - 2u);
        return 1;
    }

    if (!Spinel_PackedRead(&data[2], (uint16_t)(len - 2u), &prop, &used))
        return 0;

    out->prop = prop;
    out->value = &data[2u + used];
    out->valueLen = (uint16_t)(len - 2u - used);
    return 1;
}

uint16_t Spinel_Build(U_BStream *dst,
                      uint8_t tid,
                      uint8_t cmd,
                      uint32_t prop,
                      const uint8_t *value,
                      uint16_t valueLen)
{
    uint8_t packed[5];
    uint16_t packedLen;
    uint16_t need;
    uint16_t i;

    if (dst == 0 || (value == 0 && valueLen != 0u))
        return 0u;

    packedLen = Spinel_PackedWrite(packed, (uint16_t)sizeof(packed), prop);

    if (packedLen == 0u)
        return 0u;

    need = (uint16_t)(2u + packedLen + valueLen);

    if ((uint32_t)need > U_bstream_remaining(dst))
        return 0u;

    U_bstream_put_u8(dst, Spinel_Header(tid));
    U_bstream_put_u8(dst, cmd);

    for (i = 0u; i < packedLen; ++i)
        U_bstream_put_u8(dst, packed[i]);

    for (i = 0u; i < valueLen; ++i)
        U_bstream_put_u8(dst, value[i]);

    return need;
}

uint16_t Spinel_BuildStatus(U_BStream *dst, uint8_t tid, uint32_t status)
{
    uint8_t packed[5];
    uint16_t n;

    n = Spinel_PackedWrite(packed, (uint16_t)sizeof(packed), status);

    if (n == 0u)
        return 0u;
    /* LAST_STATUS is property 0 and carries a packed uint (docs/protocol.md §4). */
    return Spinel_Build(dst, tid, SPINEL_CMD_PROP_VALUE_IS, 0u, packed, n);
}

uint16_t Spinel_BuildTxStatus(U_BStream *dst, uint8_t tid, uint32_t status, int framePending)
{
    /* docs/protocol.md §7.1: OpenThread's HandleTransmitDone unpacks status, framePending
     * and headerUpdated in that order before the optional ACK frame. framePending is the
     * received ACK's Frame Pending subfield; headerUpdated is 0 because TRANSMIT_SEC is not
     * advertised; the ACK frame itself may be empty. */
    uint8_t value[7];
    uint16_t n = Spinel_PackedWrite(value, (uint16_t)sizeof(value), status);

    if (n == 0u)
        return 0u;

    value[n] = framePending ? 1u : 0u;
    value[n + 1u] = 0u;
    return Spinel_Build(dst, tid, SPINEL_CMD_PROP_VALUE_IS, 0u, value, (uint16_t)(n + 2u));
}

uint16_t Spinel_BuildLastStatus(U_BStream *dst, uint32_t status)
{
    /* Unsolicited: TID 0 (docs/protocol.md §4). */
    return Spinel_BuildStatus(dst, 0u, status);
}

uint16_t Spinel_BuildValueIs(U_BStream *dst, uint32_t prop, const uint8_t *value, uint16_t valueLen)
{
    /* Notifications carry TID 0; a response keeps the command's TID (docs/protocol.md §4). */
    return Spinel_Build(dst, 0u, SPINEL_CMD_PROP_VALUE_IS, prop, value, valueLen);
}
