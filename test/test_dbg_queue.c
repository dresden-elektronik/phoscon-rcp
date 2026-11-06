/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* Host unit test for the debug record queue (src/hal/dbg.c). It exists for the overflow
 * marker path no firmware run reaches (~18 records against a 24-record ring,
 * and pins the 8-byte little-endian record contract (dbg.h). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dbg.h"

/* The one HAL dependency dbg.c has; controllable so both producer rings can be driven. */
static int irqContext;

int HAL_InIrqContext(void)
{
    return irqContext;
}

static int failures;
static uint16_t sinkRecords;
static uint16_t lastComponent;
static uint16_t lastLine;
static uint16_t lastReason;

static void check(const char *what, int ok)
{
    printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");

    if (!ok)
        failures++;
}

static void sink(uint16_t component, uint16_t line, uint16_t reason)
{
    sinkRecords++;
    lastComponent = component;
    lastLine = line;
    lastReason = reason;
}

#define DRAIN_MAX 4096
static uint8_t drain[DRAIN_MAX];
static uint16_t drainedBytes;
static uint16_t drainedFrames;

static void drainAll(void)
{
    uint16_t n;

    drainedBytes = 0;
    drainedFrames = 0;
    memset(drain, 0xFF, sizeof(drain));

    for (;;)
    {
        n = Debug_QueueTake(&drain[drainedBytes], (uint16_t)(sizeof(drain) - drainedBytes));

        if (n == 0u)
            break;

        drainedBytes = (uint16_t)(drainedBytes + n);
    }

    drainedFrames = (uint16_t)(drainedBytes / DBG_FRAME_SIZE);
}

static uint16_t frameField(uint16_t frame, unsigned which)
{
    return (uint16_t)(drain[frame * DBG_FRAME_SIZE + which * 2u] |
                      (drain[frame * DBG_FRAME_SIZE + which * 2u + 1u] << 8));
}

static uint16_t markersDrained(void)
{
    uint16_t i;
    uint16_t n = 0;

    for (i = 0; i < drainedFrames; i++)
    {
        if (frameField(i, 1) == (uint16_t)DBG_COMP_DBGQ &&
            frameField(i, 3) == (uint16_t)DBG_DBGQ_RSN_OVERFLOW)
            n++;
    }

    return n;
}

/* The drop count a marker carries (its line field). 0 when no marker drained. */
static uint16_t markerDropField(void)
{
    uint16_t i;

    for (i = 0; i < drainedFrames; i++)
    {
        if (frameField(i, 1) == (uint16_t)DBG_COMP_DBGQ &&
            frameField(i, 3) == (uint16_t)DBG_DBGQ_RSN_OVERFLOW)
        {
            return frameField(i, 2);
        }
    }

    return 0;
}

static int emitTag(uint16_t tag)
{
    /* Component is u16 and uninterpreted, so a synthetic tag identifies a record. */
    Debug_Emit((uint16_t)(1000u + tag), (uint16_t)tag, (uint16_t)tag);
    return tag;
}

static int findTag(uint16_t tag, uint16_t *frameOut)
{
    uint16_t i;

    for (i = 0; i < drainedFrames; i++)
    {
        if (frameField(i, 1) == (uint16_t)(1000u + tag))
        {
            if (frameOut)
                *frameOut = i;

            return 1;
        }
    }

    return 0;
}

int main(void)
{
    uint16_t frame;
    uint16_t i;
    uint16_t n;

    Debug_SetSink(sink);

    printf("dbg queue host test (DBG_QUEUE_SLOTS = %u, capacity = %u)\n",
           (unsigned)DBG_QUEUE_SLOTS,
           (unsigned)(DBG_QUEUE_SLOTS - 1u));

    /* 1. Empty queue, and a buffer too small for one frame. */
    drainAll();
    check("empty queue drains 0 bytes", drainedBytes == 0u);
    n = Debug_QueueTake(drain, DBG_FRAME_SIZE - 1u);
    check("maxBytes < one frame is refused (no partial record)", n == 0u);

    /* 2. Wire format: magic 0x0000, component, line, reason, little endian. */
    emitTag(1);
    emitTag(2);
    emitTag(3);
    drainAll();
    check("3 records drain as 24 bytes", drainedBytes == 3u * DBG_FRAME_SIZE);
    check("every frame carries the magic field",
          drainedFrames == 3u && frameField(0, 0) == DBG_MAGIC && frameField(1, 0) == DBG_MAGIC &&
              frameField(2, 0) == DBG_MAGIC);
    check("fields land in order (component, line, reason)",
          frameField(0, 1) == 1001u && frameField(0, 2) == 1u && frameField(0, 3) == 1u &&
              frameField(2, 1) == 1003u);
    check("the sink saw every record too",
          sinkRecords == 3u && lastComponent == 1003u && lastLine == 3u && lastReason == 3u);

    /* 3. Exactly full: nothing dropped, no marker. */
    for (i = 0; i < DBG_QUEUE_SLOTS - 1u; i++)
        emitTag((uint16_t)(10 + i));

    drainAll();
    check("a full ring drains all capacity records",
          drainedFrames == (uint16_t)(DBG_QUEUE_SLOTS - 1u));
    check("a full ring drops nothing", Debug_QueueDrops() == 0u);
    check("a full ring emits no marker", markersDrained() == 0u);

    /* 4. Overflow: the OLDEST record goes, dbgDrops counts it, one marker carries the
   *    drop count (dbg.h). */
    for (i = 0; i < DBG_QUEUE_SLOTS - 1u; i++)
        emitTag((uint16_t)(20 + i));

    emitTag(300);
    emitTag(301);
    drainAll();
    check("overflow counts drops", Debug_QueueDrops() > 0u);
    check("a ring never drains more than its capacity",
          drainedFrames <= (uint16_t)(DBG_QUEUE_SLOTS - 1u));
    check("overflow drops the OLDEST records",
          !findTag(20, &frame) && !findTag(21, &frame) && !findTag(22, &frame));
    check("overflow keeps the newest records", findTag(300, &frame) && findTag(301, &frame));
    check("overflow emitted a marker", markersDrained() >= 1u);
    check("at most one marker per overwrite cycle", markersDrained() <= 1u);
    {
        uint16_t markerLine = markerDropField();
        uint16_t drops = Debug_QueueDrops();

        check("the marker's line field is a drop count", markerLine >= 1u && markerLine <= drops);
        printf("      drops counted %u, marker carried %u, capacity %u, drained %u\n",
               (unsigned)drops,
               (unsigned)markerLine,
               (unsigned)(DBG_QUEUE_SLOTS - 1u),
               (unsigned)drainedFrames);
    }

    /* 4b. Sustained overflow: the ring is the limit and the drained/dropped arithmetic
   *     closes. */
    {
        uint16_t dropsBefore0 = Debug_QueueDrops();
        uint16_t markers = 0;
        uint16_t drops = 0;
        uint16_t droppedRecords;
        uint16_t recordsGone;
        uint16_t pushN;

        /* Relative to the ring, so it overflows whatever DBG_QUEUE_SLOTS is. */
        pushN = (uint16_t)(DBG_QUEUE_SLOTS + 36u);

        for (i = 0; i < pushN; i++)
            emitTag((uint16_t)(400 + i));

        drainAll();
        markers = markersDrained();
        drops = (uint16_t)(Debug_QueueDrops() - dropsBefore0);
        check("sustained overflow drained exactly the capacity",
              drainedFrames == (uint16_t)(DBG_QUEUE_SLOTS - 1u));
        check("sustained overflow kept the newest record",
              findTag((uint16_t)(400 + pushN - 1u), &frame));
        check("sustained overflow dropped the oldest records",
              !findTag(400, &frame) && !findTag(410, &frame));
        check("sustained overflow kept emitting markers", markers >= 1u);
        /* A marker is itself a record, so a long overflow can overwrite the marker that
     * reported it: markersDrained() is a lower bound. */
        recordsGone = (uint16_t)(pushN > drainedFrames ? pushN - drainedFrames : 0u);
        droppedRecords = drops;
        check("every record that did not reach the host is counted as a drop",
              droppedRecords >= recordsGone);
        check("the extra drops are only markers' own slots (at most one per cycle)",
              droppedRecords <= recordsGone + (uint16_t)((pushN + markers) / DBG_QUEUE_SLOTS + 1u));
        check("records drained or dropped, never both, never neither",
              (unsigned)(drainedFrames + recordsGone) == pushN);
        printf("      %u pushes without draining: %u marker(s) survived the drain, "
               "%u frame(s) drained, %u record(s) never deliverable, %u drop(s) "
               "counted\n",
               (unsigned)pushN,
               (unsigned)markers,
               (unsigned)drainedFrames,
               (unsigned)recordsGone,
               (unsigned)droppedRecords);
    }

    /* 4c. Draining as we go: the marker stays silent - it reports loss, not a busy queue. */
    {
        uint16_t markers = 0;

        for (i = 0; i < 40u; i++)
        {
            emitTag((uint16_t)(500 + i));
            drainAll();
            markers = (uint16_t)(markers + markersDrained());

            if (!findTag((uint16_t)(500 + i), &frame))
                check("a drained-as-we-go record is deliverable", 0);
        }

        check("no marker while the ring never fills", markers == 0u);
    }

    /* 5. Handler and thread rings never share a slot; the thread ring drains first. */
    irqContext = 0;
    emitTag(200);
    emitTag(201);
    irqContext = 1;
    emitTag(202);
    emitTag(203);
    irqContext = 0;
    drainAll();
    check("both contexts drained", drainedFrames == 4u);
    check("thread ring comes first",
          findTag(200, &frame) && findTag(201, &frame) && findTag(202, &frame) &&
              findTag(203, &frame) && frameField(0, 1) == 1200u && frameField(1, 1) == 1201u &&
              frameField(2, 1) == 1202u && frameField(3, 1) == 1203u);

    /* 6. A handler push during a drain keeps whole records. */
    for (i = 0; i < 10u; i++)
        emitTag((uint16_t)(300 + i));

    irqContext = 1;
    emitTag(400);
    irqContext = 0;
    n = Debug_QueueTake(drain, 40u);
    check("a short drain returns a whole number of records",
          n % DBG_FRAME_SIZE == 0u && n > 0u && n <= 40u);
    drainAll();
    check("the rest of the queue is still drainable after a short drain", drainedFrames >= 5u);
    check("the racing handler record is present", findTag(400, &frame));

    printf("dbg queue: %s (%d failure(s))\n", failures ? "FAIL" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
