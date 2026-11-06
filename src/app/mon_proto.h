/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mon_proto.h — the bench register monitor's command format and address policy (core).

The ConBee III has no debugger, so a probe image that opts in (DS_MONITOR) takes register commands
over its host link: read words, write words, or write one word and trace how another one changes.
This module is the pure half: it turns an HDLC payload into a command and decides whether every
word the command touches lies in a region the board allows, for reading or for writing. It reads no
register and owns no state; the board's glue executes what it accepts (hal/mon_drv.h) and the app
reports the result as DBG_COMP_MON records (docs/hardware_mg21.md §10.5).

Payloads, little-endian, one HDLC frame each (rcp/hdlc.h framing, FCS checked by the decoder):

  READ   01 tag addr:u32 count:u8                  count words from addr
  WRITE  02 tag addr:u32 count:u8 value:u32*count   count words from addr, in order
  TRACE  03 tag waddr:u32 wvalue:u32 taddr:u32 mask:u32 count:u8 cycles:u32
         write wvalue to waddr, then sample taddr & mask: the first value and every change, up
         to count samples or the cycle budget, each with its cycle offset from the write */
#ifndef MON_PROTO_H
#define MON_PROTO_H

#include <stdint.h>

/* Per-command bounds: a response must fit the 64-slot record queue (hal/dbg.h) with room left for
 * the heartbeat, at two records per word and four per trace sample plus two framing records. */
#define MON_READ_MAX 16u
#define MON_WRITE_MAX 32u
#define MON_TRACE_MAX 8u

/* A trace runs with interrupts masked, so it is bounded well inside the 2 s watchdog: half a second
 * of the ConBee III's 38.4 MHz core clock. */
#define MON_TRACE_CYCLES_MAX 19200000u

typedef enum
{
    MON_OP_READ = 1,
    MON_OP_WRITE = 2,
    MON_OP_TRACE = 3
} Mon_Op;

/* What a command came to. The value travels in the END record's line field. */
typedef enum
{
    MON_ST_OK = 0,
    MON_ST_BAD_OP,     /* unknown opcode, or an empty payload */
    MON_ST_BAD_LEN,    /* the payload length does not match the opcode and count */
    MON_ST_BAD_COUNT,  /* count is zero or above the opcode's bound */
    MON_ST_BAD_ALIGN,  /* an address is not word-aligned */
    MON_ST_BAD_CYCLES, /* a trace budget of zero or above MON_TRACE_CYCLES_MAX */
    MON_ST_DENIED,     /* a word lies outside every region, or a write lands in a read-only one */
    MON_ST_BUSY,       /* a command arrived while the previous one was still pending */
    MON_ST_TIMEOUT     /* a trace ended on its cycle budget rather than its sample count */
} Mon_Status;

/* One address range the board allows. Regions are searched in order and the first that contains a
 * word decides it, so a read-only hole inside a writable window is listed before the window. */
typedef struct
{
    uint32_t base;
    uint32_t size;
    int writable;
} Mon_Region;

typedef struct
{
    uint8_t op;
    uint8_t tag;
    uint8_t count;
    uint32_t addr;                  /* READ, WRITE: the first word; TRACE: the word written */
    uint32_t values[MON_WRITE_MAX]; /* WRITE: the words; TRACE: values[0] is the word written */
    uint32_t traceAddr;
    uint32_t traceMask;
    uint32_t traceCycles;
} Mon_Cmd;

/* Decode one payload. On anything but MON_ST_OK the command still carries the tag, when the
 * payload was long enough to hold one, so the reply can be matched to the request. */
Mon_Status Mon_Parse(const uint8_t *payload, uint16_t len, Mon_Cmd *cmd);

/* 1 when every word [addr, addr + 4 * count) lies in one region that permits the access. */
int Mon_RangeAllowed(const Mon_Region *regions, uint16_t nRegions, uint32_t addr, uint32_t count,
                     int write);

/* The policy for a parsed command: alignment, then every range it reads and writes. */
Mon_Status Mon_Check(const Mon_Cmd *cmd, const Mon_Region *regions, uint16_t nRegions);

#endif /* MON_PROTO_H */
