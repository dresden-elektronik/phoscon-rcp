/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* Debug record emission and the record queue (docs/coding-rules.md §5).
 * Debug_Emit is the only producer, reached from the USB vector; the queue is
 * the transport-agnostic half.
 */
#include "dbg.h"
#include "hal.h"
#include "u_bstream.h"

#define DBG_QUEUE_CAPACITY (DBG_QUEUE_SLOTS - 1u)

static Dbg_Sink dbgSink;

/* One ring of wire-format frames; the indices are volatile (the USB vector
 * races the drain) and one slot stays free for a drain to reserve its batch. */
typedef struct
{
    uint8_t frames[DBG_QUEUE_SLOTS * DBG_FRAME_SIZE];
    volatile uint16_t oldest;
    volatile uint16_t count;
} DbgQueue;

/* Two rings, selected by producer context, so a thread record and an IRQ
 * record never aim at the same slot; this file may not mask interrupts. */
static DbgQueue dbgQueueThread;
static DbgQueue dbgQueueIrq;

static uint32_t dbgSeq;
static volatile uint16_t dbgDrops;
static uint32_t dbgMarkSeq;

static void dbgStore(DbgQueue *q, uint16_t component, uint16_t line, uint16_t reason)
{
    U_BStream bs;
    uint16_t slot;

    slot = (uint16_t)((q->oldest + q->count) % DBG_QUEUE_SLOTS);
    U_bstream_init(&bs, &q->frames[(uint32_t)slot * DBG_FRAME_SIZE], DBG_FRAME_SIZE);
    U_bstream_put_u16_le(&bs, DBG_MAGIC);
    U_bstream_put_u16_le(&bs, component);
    U_bstream_put_u16_le(&bs, line);
    U_bstream_put_u16_le(&bs, reason);

    if (q->count < DBG_QUEUE_CAPACITY)
        q->count = (uint16_t)(q->count + 1u);
    else
    {
        q->oldest = (uint16_t)((q->oldest + 1u) % DBG_QUEUE_SLOTS);

        if (dbgDrops < 0xFFFFu)
            dbgDrops = (uint16_t)(dbgDrops + 1u);
    }

    dbgSeq++;
}

static void dbgQueueFrame(uint16_t component, uint16_t line, uint16_t reason)
{
    DbgQueue *q = HAL_InIrqContext() ? &dbgQueueIrq : &dbgQueueThread;
    int overflowing = (int)(q->count >= DBG_QUEUE_CAPACITY);

    /* Overflow drops the oldest frame; one marker per full overwrite cycle
     * carries the drop count in its line field (the record is stored first). */
    dbgStore(q, component, line, reason);

    if (overflowing && dbgSeq - dbgMarkSeq >= DBG_QUEUE_SLOTS)
    {
        dbgMarkSeq = dbgSeq;
        dbgStore(q, (uint16_t)DBG_COMP_DBGQ, dbgDrops, DBG_DBGQ_RSN_OVERFLOW);
    }
}

static uint16_t dbgTakeRing(DbgQueue *q, U_BStream *bs)
{
    uint16_t room;
    uint16_t whole;
    uint16_t first;
    uint16_t oldestAt;
    uint16_t countAt;
    uint16_t oldestEnd;
    uint16_t countEnd;

    room = (uint16_t)U_bstream_remaining(bs);

    if (room < DBG_FRAME_SIZE || q->count == 0u)
        return 0;

    room = (uint16_t)(room / DBG_FRAME_SIZE);
    whole = q->count;

    if (whole > room)
        whole = room;

    oldestAt = q->oldest;
    countAt = q->count;
    oldestEnd = (uint16_t)((oldestAt + whole) % DBG_QUEUE_SLOTS);
    countEnd = (uint16_t)(countAt - whole);
    q->oldest = oldestEnd;
    q->count = countEnd;

    /* At most one wrap: a ring never holds more than DBG_QUEUE_SLOTS-1. */
    first = whole;

    if (oldestAt + first > DBG_QUEUE_SLOTS)
        first = (uint16_t)(DBG_QUEUE_SLOTS - oldestAt);

    U_bstream_put_bytes(
        bs, &q->frames[(uint32_t)oldestAt * DBG_FRAME_SIZE], (unsigned long)first * DBG_FRAME_SIZE);

    if (first < whole)
        U_bstream_put_bytes(bs, &q->frames[0], (unsigned long)(whole - first) * DBG_FRAME_SIZE);

    if (q->oldest != oldestEnd || q->count != countEnd)
    {
        if (dbgDrops < 0xFFFFu)
            dbgDrops = (uint16_t)(dbgDrops + 1u);
    }

    return (uint16_t)(whole * DBG_FRAME_SIZE);
}

/* Bring-up records held until the transport exists; wire-format, one frame per
 * record, so a replay is a byte copy and nothing is re-derived. */
static uint8_t dbgShadow[DBG_QUEUE_SLOTS * DBG_FRAME_SIZE];
static uint16_t dbgShadowLen;

void Debug_ShadowHold(int on)
{
    if (on)
        dbgShadowLen = 0;
}

/* Re-queue the held frames, oldest first; returns the number replayed. The marker goes in
 * first so a reader can tell a replay from a fresh boot. A ring drops its oldest frame on
 * overflow, so a full shadow plus the marker would push out the marker and the first records -
 * the reset cause and the start of the bring-up, the ones a late host came for. Only as many
 * as fit beside the marker are replayed: what a long bring-up gives up is its tail. */
#define DBG_REPLAY_MAX (DBG_QUEUE_CAPACITY - 1u)

uint16_t Debug_ShadowReplay(void)
{
    uint16_t at;
    uint16_t n = 0;
    uint16_t held = (uint16_t)(dbgShadowLen / DBG_FRAME_SIZE);

    if (held == 0u)
        return 0;

    if (held > DBG_REPLAY_MAX)
        held = DBG_REPLAY_MAX;

    dbgQueueFrame((uint16_t)DBG_COMP_DBGQ, held, (uint16_t)DBG_DBGQ_RSN_REPLAY);

    for (at = 0; n < held; at = (uint16_t)(at + DBG_FRAME_SIZE))
    {
        dbgQueueFrame((uint16_t)(dbgShadow[at + 2u] | (dbgShadow[at + 3u] << 8)),
                      (uint16_t)(dbgShadow[at + 4u] | (dbgShadow[at + 5u] << 8)),
                      (uint16_t)(dbgShadow[at + 6u] | (dbgShadow[at + 7u] << 8)));
        n++;
    }

    return n;
}

void Debug_SetSink(Dbg_Sink fn)
{
    dbgSink = fn;
}

void Debug_Emit(uint16_t component, uint16_t line, uint16_t reason)
{
    if (dbgShadowLen + DBG_FRAME_SIZE <= (uint16_t)sizeof(dbgShadow))
    {
        U_BStream ss;

        U_bstream_init(&ss, &dbgShadow[dbgShadowLen], DBG_FRAME_SIZE);
        U_bstream_put_u16_le(&ss, DBG_MAGIC);
        U_bstream_put_u16_le(&ss, component);
        U_bstream_put_u16_le(&ss, line);
        U_bstream_put_u16_le(&ss, reason);
        dbgShadowLen = (uint16_t)(dbgShadowLen + DBG_FRAME_SIZE);
    }

    dbgQueueFrame(component, line, reason);

    if (dbgSink)
        dbgSink(component, line, reason);
}

uint16_t Debug_QueueTake(uint8_t *dst, uint16_t maxBytes)
{
    U_BStream bs;
    uint16_t taken;

    if (!dst || maxBytes < DBG_FRAME_SIZE)
        return 0;

    U_bstream_init(&bs, dst, maxBytes);
    /* Thread ring first: bring-up records precede the enumeration records the
     * transport mostly carries. */
    taken = dbgTakeRing(&dbgQueueThread, &bs);
    taken = (uint16_t)(taken + dbgTakeRing(&dbgQueueIrq, &bs));
    return taken;
}

uint16_t Debug_QueueDrops(void)
{
    return dbgDrops;
}
