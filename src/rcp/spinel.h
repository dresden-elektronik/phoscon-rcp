/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* spinel.h — Spinel framing: the header byte, commands, packed uints, and the
 * property-id field (docs/protocol.md §4). */
#ifndef RCP_SPINEL_H
#define RCP_SPINEL_H

#include <stdint.h>

#include "u_bstream.h"

/* FLG lives in bits 7:6 and is 0b10, so a header is 0x80..0xBF. The spec's prose
 * annotates it "(0x40)", which matches nothing; its diagram and captures are right
 * (docs/protocol.md §4). */
#define SPINEL_FLG_MASK 0xC0u
#define SPINEL_FLG_VALUE 0x80u
#define SPINEL_TID_MASK 0x0Fu

/* Commands we implement or must recognise (docs/protocol.md §4; values per spinel.h). */
enum
{
    SPINEL_CMD_NOOP = 0,
    SPINEL_CMD_RESET = 1,
    SPINEL_CMD_PROP_VALUE_GET = 2,
    SPINEL_CMD_PROP_VALUE_SET = 3,
    SPINEL_CMD_PROP_VALUE_INSERT = 4,
    SPINEL_CMD_PROP_VALUE_REMOVE = 5,
    SPINEL_CMD_PROP_VALUE_IS = 6, /* the only RCP->host command (docs/protocol.md §4) */
    SPINEL_CMD_PROP_VALUE_INSERTED = 7,
    SPINEL_CMD_PROP_VALUE_REMOVED = 8,
    SPINEL_CMD_NET_SAVE = 9,
    SPINEL_CMD_NET_CLEAR = 10,
    SPINEL_CMD_NET_RECALL = 11,
    SPINEL_CMD_PEEK = 18,
    SPINEL_CMD_PEEK_RET = 19,
    SPINEL_CMD_POKE = 20
};

/* Status codes (docs/protocol.md §9; values per spinel.h). */
enum
{
    SPINEL_STATUS_OK = 0,
    SPINEL_STATUS_FAILURE = 1,
    SPINEL_STATUS_UNIMPLEMENTED = 2,
    SPINEL_STATUS_INVALID_ARG = 3,
    SPINEL_STATUS_INVALID_STATE = 4,
    SPINEL_STATUS_INVALID_COMMAND = 5,
    SPINEL_STATUS_INVALID_INTERFACE = 6,
    SPINEL_STATUS_INTERNAL_ERROR = 7,
    SPINEL_STATUS_PARSE_ERROR = 9,
    SPINEL_STATUS_IN_PROGRESS = 10,
    SPINEL_STATUS_NOMEM = 11,
    SPINEL_STATUS_BUSY = 12,
    SPINEL_STATUS_PROP_NOT_FOUND = 13,
    SPINEL_STATUS_DROPPED = 14,
    SPINEL_STATUS_EMPTY = 15,
    SPINEL_STATUS_CMD_TOO_BIG = 16,
    SPINEL_STATUS_NO_ACK = 17,
    SPINEL_STATUS_CCA_FAILURE = 18,
    SPINEL_STATUS_ALREADY = 19,
    /* What upstream answers when a REMOVE names an entry that is not there:
     * OT_ERROR_NOT_FOUND -> SPINEL_STATUS_ITEM_NOT_FOUND (spinel.h:507). */
    SPINEL_STATUS_ITEM_NOT_FOUND = 20,
    SPINEL_STATUS_RESET_POWER_ON = 112,
    SPINEL_STATUS_RESET_EXTERNAL = 113,
    SPINEL_STATUS_RESET_SOFTWARE = 114,
    SPINEL_STATUS_RESET_WATCHDOG = 120
};

#define SPINEL_PROP_MAX 0xFFFFu

typedef struct
{
    uint8_t tid;
    uint8_t cmd;
    uint32_t prop;
    const uint8_t *value;
    uint16_t valueLen;
} Spinel_Frame;

int Spinel_Decode(const uint8_t *data, uint16_t len, Spinel_Frame *out);

uint16_t Spinel_Build(U_BStream *dst,
                      uint8_t tid,
                      uint8_t cmd,
                      uint32_t prop,
                      const uint8_t *value,
                      uint16_t valueLen);

uint16_t Spinel_BuildStatus(U_BStream *dst, uint8_t tid, uint32_t status);
uint16_t Spinel_BuildTxStatus(U_BStream *dst, uint8_t tid, uint32_t status, int framePending);
uint16_t Spinel_BuildLastStatus(U_BStream *dst, uint32_t status);
uint16_t
Spinel_BuildValueIs(U_BStream *dst, uint32_t prop, const uint8_t *value, uint16_t valueLen);

/* A packed uint (docs/protocol.md §4, type "i") is unsigned LEB128: U_bstream_put_leb128_u32 and
 * U_bstream_get_leb128_u32 read and write it. */

uint8_t Spinel_Header(uint8_t tid);
int Spinel_HeaderTid(uint8_t header);

#endif
