/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* srcmatch.h — the source-match table: which addresses have a frame queued for them
 * (docs/protocol.md §10.2). Pure state over plain arrays, so the host suite drives it
 * directly and the radio interrupt can read it without a lock (docs/architecture.md §7).
 *
 * The table answers one question the Ack's Frame Pending bit needs: does this peer have
 * something waiting? It never allocates, never evicts and never blocks; a full table
 * refuses the insert and the caller reports it (docs/coding-rules.md §4.3). */
#ifndef RCP_SRCMATCH_H
#define RCP_SRCMATCH_H

#include <stdint.h>

#include "u_bstream.h"

/* One short address per child, one extended address per child, and room for a host that
 * keeps both forms of the same device: 32 x 2 + 16 x 8 = 192 bytes. The largest echo this
 * table can produce is 64 value bytes, inside session.h RCP_VALUE_MAX. */
#define SRCMATCH_SHORT_MAX 32u
#define SRCMATCH_EXT_MAX 16u
#define SRCMATCH_EXT_LEN 8u

typedef struct
{
    uint8_t enabled; /* 0: every Ack to a Data Request claims pending (docs/protocol.md §10.2) */
    uint16_t shortAddr[SRCMATCH_SHORT_MAX];
    uint8_t shortCount;
    /* The session's table holds the wire order, MSB first - Spinel's 'E', the order
     * MAC_15_4_LADDR and OpenThread's otExtAddress use - so a GET echoes what was written. The
     * radio's copy, made by SrcMatch_CopyOnAir, holds on-air order (LSB first), the order a
     * received frame's source address is compared in. */
    uint8_t extAddr[SRCMATCH_EXT_MAX][SRCMATCH_EXT_LEN];
    uint8_t extCount;
    uint8_t generation; /* bumps on every mutation: how the app knows to push the table down */
} SrcMatch_Table;

/* Status returns of the mutating calls, mapped to Spinel by the caller. */
enum
{
    SRCMATCH_OK = 0,
    SRCMATCH_FULL = 1,
    SRCMATCH_ABSENT = 2,
    SRCMATCH_BAD_LEN = 3
};

void SrcMatch_Init(SrcMatch_Table *t);

/* Replace a list from wire bytes: 'A(S)' is a multiple of 2, 'A(E)' a multiple of 8.
 * All or nothing - a list that does not fit leaves the table untouched. */
int SrcMatch_SetShortList(SrcMatch_Table *t, const uint8_t *value, uint16_t len);
int SrcMatch_SetExtList(SrcMatch_Table *t, const uint8_t *value, uint16_t len);

/* One entry. Inserting what is already there succeeds and stores nothing twice, so a
 * REMOVE afterwards removes the address rather than one copy of it. */
int SrcMatch_InsertShort(SrcMatch_Table *t, uint16_t addr);
int SrcMatch_InsertExt(SrcMatch_Table *t, const uint8_t ext[SRCMATCH_EXT_LEN]);
int SrcMatch_RemoveShort(SrcMatch_Table *t, uint16_t addr);
int SrcMatch_RemoveExt(SrcMatch_Table *t, const uint8_t ext[SRCMATCH_EXT_LEN]);

/* The lookup the Ack decision makes, on the radio's on-air copy: ext is on-air order. */
int SrcMatch_HasShort(const SrcMatch_Table *t, uint16_t addr);
int SrcMatch_HasExt(const SrcMatch_Table *t, const uint8_t ext[SRCMATCH_EXT_LEN]);

/* GET's value: the stored list as wire bytes, no count prefix. */
uint16_t SrcMatch_EncodeShort(const SrcMatch_Table *t, U_BStream *dst);
uint16_t SrcMatch_EncodeExt(const SrcMatch_Table *t, U_BStream *dst);

/* Copy for the double buffer the HAL swaps under the interrupt (docs/architecture.md §7). */
void SrcMatch_Copy(SrcMatch_Table *dst, const SrcMatch_Table *src);

/* The radio's copy: the same table with every extended address reversed into on-air order,
 * which is the one reversal between Spinel's 'E' and the frame (mac.h Mac_Hdr). */
void SrcMatch_CopyOnAir(SrcMatch_Table *dst, const SrcMatch_Table *src);

#endif
