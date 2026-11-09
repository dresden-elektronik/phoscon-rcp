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

int Spinel_Decode(const uint8_t *data, uint16_t len, Spinel_Frame *out)
{
    U_BStream bs;
    uint32_t prop;
    uint16_t used;
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

    /* At most five bytes, the most a 32-bit value needs; anything longer, or bits above 32, is not a
     * property id (docs/protocol.md §4). */
    U_bstream_init(&bs, (void *)&data[2], (unsigned long)(len - 2u));
    prop = (uint32_t)U_bstream_get_leb128_u32(&bs);

    if (bs.status != U_BSTREAM_OK)
        return 0;

    used = (uint16_t)U_bstream_pos(&bs);

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
    uint16_t need;

    if (dst == 0 || (value == 0 && valueLen != 0u))
        return 0u;

    need = (uint16_t)(2u + U_bstream_leb128_u32_size(prop) + valueLen);

    if (!U_bstream_fits(dst, need))
        return 0u;

    U_bstream_put_u8(dst, Spinel_Header(tid));
    U_bstream_put_u8(dst, cmd);
    U_bstream_put_leb128_u32(dst, prop);
    U_bstream_put_bytes(dst, value, valueLen);
    return need;
}

uint16_t Spinel_BuildStatus(U_BStream *dst, uint8_t tid, uint32_t status)
{
    uint8_t packed[5];
    U_BStream vs;

    /* LAST_STATUS is property 0 and carries a packed uint (docs/protocol.md §4). */
    U_bstream_init(&vs, packed, sizeof(packed));
    U_bstream_put_leb128_u32(&vs, status);
    return Spinel_Build(dst, tid, SPINEL_CMD_PROP_VALUE_IS, 0u, packed, (uint16_t)U_bstream_pos(&vs));
}

uint16_t Spinel_BuildTxStatus(U_BStream *dst, uint8_t tid, uint32_t status, int framePending)
{
    /* docs/protocol.md §7.1: OpenThread's HandleTransmitDone unpacks status, framePending
     * and headerUpdated in that order before the optional ACK frame. framePending is the
     * received ACK's Frame Pending subfield; headerUpdated is 0 because TRANSMIT_SEC is not
     * advertised; the ACK frame itself may be empty. */
    uint8_t value[7];
    U_BStream vs;

    U_bstream_init(&vs, value, sizeof(value));
    U_bstream_put_leb128_u32(&vs, status);
    U_bstream_put_u8(&vs, framePending ? 1u : 0u);
    U_bstream_put_u8(&vs, 0u);
    return Spinel_Build(dst, tid, SPINEL_CMD_PROP_VALUE_IS, 0u, value, (uint16_t)U_bstream_pos(&vs));
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
