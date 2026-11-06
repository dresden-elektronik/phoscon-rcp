/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mon_proto.c — the bench register monitor's command format and address policy (mon_proto.h). */
#include "mon_proto.h"

#include "u_bstream.h"

#define MON_HEAD_LEN 2u           /* op, tag */
#define MON_READ_LEN 7u           /* op, tag, addr, count */
#define MON_WRITE_FIXED_LEN 7u    /* op, tag, addr, count; then the words */
#define MON_TRACE_LEN 23u         /* op, tag, waddr, wvalue, taddr, mask, count, cycles */

/* The scalar fields only: values[] is meaningful up to count, and a zeroing loop over it is one a
 * compiler turns into a memset call this freestanding image does not link. */
static void monClear(Mon_Cmd *cmd)
{
    cmd->op = 0u;
    cmd->tag = 0u;
    cmd->count = 0u;
    cmd->addr = 0u;
    cmd->traceAddr = 0u;
    cmd->traceMask = 0u;
    cmd->traceCycles = 0u;
}

static Mon_Status monParseRead(U_BStream *bs, uint16_t len, Mon_Cmd *cmd)
{
    if (len != MON_READ_LEN)
        return MON_ST_BAD_LEN;

    cmd->addr = (uint32_t)U_bstream_get_u32_le(bs);
    cmd->count = U_bstream_get_u8(bs);

    if (cmd->count == 0u || cmd->count > MON_READ_MAX)
        return MON_ST_BAD_COUNT;

    return MON_ST_OK;
}

static Mon_Status monParseWrite(U_BStream *bs, uint16_t len, Mon_Cmd *cmd)
{
    uint16_t i;

    if (len < MON_WRITE_FIXED_LEN)
        return MON_ST_BAD_LEN;

    cmd->addr = (uint32_t)U_bstream_get_u32_le(bs);
    cmd->count = U_bstream_get_u8(bs);

    if (cmd->count == 0u || cmd->count > MON_WRITE_MAX)
        return MON_ST_BAD_COUNT;

    if (len != (uint16_t)(MON_WRITE_FIXED_LEN + 4u * cmd->count))
        return MON_ST_BAD_LEN;

    for (i = 0u; i < cmd->count; ++i)
        cmd->values[i] = (uint32_t)U_bstream_get_u32_le(bs);

    return MON_ST_OK;
}

static Mon_Status monParseTrace(U_BStream *bs, uint16_t len, Mon_Cmd *cmd)
{
    if (len != MON_TRACE_LEN)
        return MON_ST_BAD_LEN;

    cmd->addr = (uint32_t)U_bstream_get_u32_le(bs);
    cmd->values[0] = (uint32_t)U_bstream_get_u32_le(bs);
    cmd->traceAddr = (uint32_t)U_bstream_get_u32_le(bs);
    cmd->traceMask = (uint32_t)U_bstream_get_u32_le(bs);
    cmd->count = U_bstream_get_u8(bs);
    cmd->traceCycles = (uint32_t)U_bstream_get_u32_le(bs);

    if (cmd->count == 0u || cmd->count > MON_TRACE_MAX)
        return MON_ST_BAD_COUNT;

    if (cmd->traceCycles == 0u || cmd->traceCycles > MON_TRACE_CYCLES_MAX)
        return MON_ST_BAD_CYCLES;

    return MON_ST_OK;
}

Mon_Status Mon_Parse(const uint8_t *payload, uint16_t len, Mon_Cmd *cmd)
{
    U_BStream bs;
    Mon_Status st;

    if (cmd == 0)
        return MON_ST_BAD_OP;

    monClear(cmd);

    if (payload == 0 || len == 0u)
        return MON_ST_BAD_OP;

    U_bstream_init(&bs, (void *)payload, (unsigned long)len);
    cmd->op = U_bstream_get_u8(&bs);

    if (len < MON_HEAD_LEN)
        return MON_ST_BAD_LEN;

    cmd->tag = U_bstream_get_u8(&bs);

    switch (cmd->op)
    {
        case MON_OP_READ:
            st = monParseRead(&bs, len, cmd);
            break;

        case MON_OP_WRITE:
            st = monParseWrite(&bs, len, cmd);
            break;

        case MON_OP_TRACE:
            st = monParseTrace(&bs, len, cmd);
            break;

        default:
            st = MON_ST_BAD_OP;
            break;
    }

    if (st == MON_ST_OK && bs.status != U_BSTREAM_OK)
        st = MON_ST_BAD_LEN;

    return st;
}

int Mon_RangeAllowed(const Mon_Region *regions, uint16_t nRegions, uint32_t addr, uint32_t count,
                     int write)
{
    uint32_t i;
    uint16_t r;

    if (regions == 0 || count == 0u)
        return 0;

    /* Word by word, so a range that runs from a writable region into a read-only hole is refused
     * at the first word the hole owns, and a range that wraps the address space is refused at the
     * wrap rather than folded back to address 0. */
    for (i = 0u; i < count; ++i)
    {
        uint32_t a = addr + 4u * i;
        int decided = 0;

        if (a < addr)
            return 0;

        for (r = 0u; r < nRegions && !decided; ++r)
        {
            const Mon_Region *g = &regions[r];

            if (g->size >= 4u && a >= g->base && (a - g->base) <= g->size - 4u)
            {
                if (write && !g->writable)
                    return 0;

                decided = 1;
            }
        }

        if (!decided)
            return 0;
    }

    return 1;
}

Mon_Status Mon_Check(const Mon_Cmd *cmd, const Mon_Region *regions, uint16_t nRegions)
{
    if (cmd == 0)
        return MON_ST_BAD_OP;

    switch (cmd->op)
    {
        case MON_OP_READ:
            if ((cmd->addr & 3u) != 0u)
                return MON_ST_BAD_ALIGN;
            return Mon_RangeAllowed(regions, nRegions, cmd->addr, cmd->count, 0) ? MON_ST_OK
                                                                                  : MON_ST_DENIED;

        case MON_OP_WRITE:
            if ((cmd->addr & 3u) != 0u)
                return MON_ST_BAD_ALIGN;
            return Mon_RangeAllowed(regions, nRegions, cmd->addr, cmd->count, 1) ? MON_ST_OK
                                                                                  : MON_ST_DENIED;

        case MON_OP_TRACE:
            if ((cmd->addr & 3u) != 0u || (cmd->traceAddr & 3u) != 0u)
                return MON_ST_BAD_ALIGN;
            if (!Mon_RangeAllowed(regions, nRegions, cmd->addr, 1u, 1))
                return MON_ST_DENIED;
            return Mon_RangeAllowed(regions, nRegions, cmd->traceAddr, 1u, 0) ? MON_ST_OK
                                                                               : MON_ST_DENIED;

        default:
            return MON_ST_BAD_OP;
    }
}
