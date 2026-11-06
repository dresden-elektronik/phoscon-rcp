/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* monitor_app.c — the bench register monitor in a probe image (monitor_app.h).

The parts are the ones already in the tree: HDLC framing from rcp/hdlc.c, the command format and the
address policy from app/mon_proto.c, the accesses from the board (hal/mon_drv.h), and the reply as
records through the debug queue. The only state is the decoder and a one-command mailbox: the link
interrupt fills it, the main loop empties it, and a full mailbox drops the newcomer (docs/coding-rules.md
§4.3). The flasher's reset frame is C0-delimited, never 7E, so it passes through the decoder as noise
and still reaches the flasher trap in app/main.c. */
#include <stdint.h>

#include "dbg.h"
#include "hdlc.h"
#include "mon_drv.h"
#include "mon_proto.h"
#include "monitor_app.h"

typedef enum
{
    MONITOR_SLOT_EMPTY = 0,
    MONITOR_SLOT_FULL
} Monitor_Slot;

static Hdlc_Rx monitorRx;
static int monitorRxReady;
/* The handoff from the link interrupt to the main loop. The command is written only inside
 * Mon_Parse, an out-of-line call that completes before the slot is marked full, and what the main
 * loop reads directly - the parse result and the tag - travels in volatile copies, so the order
 * holds without leaning on a compiler barrier in the HAL's core instructions. */
static Mon_Cmd monitorCmd;
static volatile Mon_Status monitorParsed;
static volatile uint8_t monitorTag;
static volatile Monitor_Slot monitorSlot;
static uint16_t monitorDrops;
static Mon_Sample monitorSamples[MON_TRACE_MAX];

static void monitorEmit32(uint32_t v, uint16_t rsnLo, uint16_t rsnHi)
{
    Debug_Emit(DBG_COMP_MON, (uint16_t)v, rsnLo);
    Debug_Emit(DBG_COMP_MON, (uint16_t)(v >> 16), rsnHi);
}

static void monitorEnd(Mon_Status st, uint8_t items)
{
    Debug_Emit(DBG_COMP_MON, (uint16_t)((uint16_t)st | ((uint16_t)items << 8)), DBG_MON_RSN_END);
}

void Monitor_Feed(const uint8_t *buf, uint16_t len)
{
    uint16_t i;

    if (buf == 0)
        return;

    if (!monitorRxReady)
    {
        Hdlc_RxInit(&monitorRx);
        monitorRxReady = 1;
    }

    for (i = 0u; i < len; ++i)
    {
        if (Hdlc_RxByte(&monitorRx, buf[i]) != HDLC_RX_FRAME)
            continue;

        if (monitorSlot != MONITOR_SLOT_EMPTY)
        {
            if (monitorDrops < 0xFFFFu)
                monitorDrops++;
            Debug_Emit(DBG_COMP_MON, monitorDrops, DBG_MON_RSN_DROPPED);
            continue;
        }

        monitorParsed = Mon_Parse(monitorRx.buf, monitorRx.len, &monitorCmd);
        monitorTag = monitorCmd.tag;
        monitorSlot = MONITOR_SLOT_FULL;
    }
}

static uint8_t monitorRun(const Mon_Cmd *cmd, Mon_Status *st)
{
    uint8_t i;
    uint8_t n;
    int timedOut;

    switch (cmd->op)
    {
        case MON_OP_READ:
            for (i = 0u; i < cmd->count; ++i)
                monitorEmit32(MonDrv_Read32(cmd->addr + 4u * i), DBG_MON_RSN_VALUE_LO,
                              DBG_MON_RSN_VALUE_HI);
            return cmd->count;

        case MON_OP_WRITE:
            for (i = 0u; i < cmd->count; ++i)
                MonDrv_Write32(cmd->addr + 4u * i, cmd->values[i]);
            return cmd->count;

        case MON_OP_TRACE:
            n = MonDrv_Trace(cmd, monitorSamples, &timedOut);
            for (i = 0u; i < n; ++i)
            {
                monitorEmit32(monitorSamples[i].value, DBG_MON_RSN_VALUE_LO, DBG_MON_RSN_VALUE_HI);
                monitorEmit32(monitorSamples[i].cycles, DBG_MON_RSN_CYCLES_LO,
                              DBG_MON_RSN_CYCLES_HI);
            }
            if (timedOut)
                *st = MON_ST_TIMEOUT;
            return n;

        default:
            *st = MON_ST_BAD_OP;
            return 0u;
    }
}

void Monitor_Poll(void)
{
    const Mon_Region *regions;
    uint16_t nRegions;
    Mon_Status st;
    uint8_t items = 0u;

    if (monitorSlot != MONITOR_SLOT_FULL)
        return;

    Debug_Emit(DBG_COMP_MON, monitorTag, DBG_MON_RSN_BEGIN);

    st = monitorParsed;

    if (st == MON_ST_OK)
    {
        regions = MonDrv_Regions(&nRegions);
        st = Mon_Check(&monitorCmd, regions, nRegions);
    }

    if (st == MON_ST_OK)
        items = monitorRun(&monitorCmd, &st);

    monitorEnd(st, items);
    monitorSlot = MONITOR_SLOT_EMPTY;
}
