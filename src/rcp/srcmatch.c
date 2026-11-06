/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* srcmatch.c — see srcmatch.h. */
#include "srcmatch.h"

static int srcMatchShortIndex(const SrcMatch_Table *t, uint16_t addr)
{
    uint8_t i;

    for (i = 0; i < t->shortCount; ++i)
    {
        if (t->shortAddr[i] == addr)
            return (int)i;
    }

    return -1;
}

static int srcMatchExtIndex(const SrcMatch_Table *t, const uint8_t ext[SRCMATCH_EXT_LEN])
{
    uint8_t i;
    uint8_t k;

    for (i = 0; i < t->extCount; ++i)
    {
        for (k = 0; k < SRCMATCH_EXT_LEN; ++k)
        {
            if (t->extAddr[i][k] != ext[k])
                break;
        }

        if (k == SRCMATCH_EXT_LEN)
            return (int)i;
    }

    return -1;
}

void SrcMatch_Init(SrcMatch_Table *t)
{
    if (t == 0)
        return;

    t->enabled = 0u;
    t->shortCount = 0u;
    t->extCount = 0u;
    t->generation = 0u;
}

int SrcMatch_SetShortList(SrcMatch_Table *t, const uint8_t *value, uint16_t len)
{
    uint16_t count;
    uint16_t i;

    if (t == 0 || (len != 0u && value == 0))
        return SRCMATCH_BAD_LEN;

    if ((len % 2u) != 0u)
        return SRCMATCH_BAD_LEN;

    count = (uint16_t)(len / 2u);

    if (count > SRCMATCH_SHORT_MAX)
        return SRCMATCH_FULL;

    /* Into the table only after the whole list has been validated: a partial list would
     * claim "nothing pending" for addresses the host believes it wrote. */
    for (i = 0; i < count; ++i)
        t->shortAddr[i] = (uint16_t)((uint16_t)value[2u * i] | ((uint16_t)value[2u * i + 1u] << 8));

    t->shortCount = (uint8_t)count;
    t->generation = (uint8_t)(t->generation + 1u);
    return SRCMATCH_OK;
}

int SrcMatch_SetExtList(SrcMatch_Table *t, const uint8_t *value, uint16_t len)
{
    uint16_t count;
    uint16_t i;
    uint8_t k;

    if (t == 0 || (len != 0u && value == 0))
        return SRCMATCH_BAD_LEN;

    if ((len % SRCMATCH_EXT_LEN) != 0u)
        return SRCMATCH_BAD_LEN;

    count = (uint16_t)(len / SRCMATCH_EXT_LEN);

    if (count > SRCMATCH_EXT_MAX)
        return SRCMATCH_FULL;

    for (i = 0; i < count; ++i)
    {
        for (k = 0; k < SRCMATCH_EXT_LEN; ++k)
            t->extAddr[i][k] = value[i * SRCMATCH_EXT_LEN + k];
    }

    t->extCount = (uint8_t)count;
    t->generation = (uint8_t)(t->generation + 1u);
    return SRCMATCH_OK;
}

int SrcMatch_InsertShort(SrcMatch_Table *t, uint16_t addr)
{
    if (t == 0)
        return SRCMATCH_BAD_LEN;

    if (srcMatchShortIndex(t, addr) >= 0)
        return SRCMATCH_OK;

    if (t->shortCount >= SRCMATCH_SHORT_MAX)
        return SRCMATCH_FULL;

    t->shortAddr[t->shortCount] = addr;
    t->shortCount = (uint8_t)(t->shortCount + 1u);
    t->generation = (uint8_t)(t->generation + 1u);
    return SRCMATCH_OK;
}

int SrcMatch_InsertExt(SrcMatch_Table *t, const uint8_t ext[SRCMATCH_EXT_LEN])
{
    uint8_t k;

    if (t == 0 || ext == 0)
        return SRCMATCH_BAD_LEN;

    if (srcMatchExtIndex(t, ext) >= 0)
        return SRCMATCH_OK;

    if (t->extCount >= SRCMATCH_EXT_MAX)
        return SRCMATCH_FULL;

    for (k = 0; k < SRCMATCH_EXT_LEN; ++k)
        t->extAddr[t->extCount][k] = ext[k];

    t->extCount = (uint8_t)(t->extCount + 1u);
    t->generation = (uint8_t)(t->generation + 1u);
    return SRCMATCH_OK;
}

int SrcMatch_RemoveShort(SrcMatch_Table *t, uint16_t addr)
{
    int idx;
    uint8_t i;

    if (t == 0)
        return SRCMATCH_BAD_LEN;

    idx = srcMatchShortIndex(t, addr);

    if (idx < 0)
        return SRCMATCH_ABSENT;

    /* Order is not part of the contract, so the hole is filled from the end rather than
     * shifting: the table is at most 32 entries and a shift costs more than it explains. */
    for (i = (uint8_t)idx; (uint8_t)(i + 1u) < t->shortCount; ++i)
        t->shortAddr[i] = t->shortAddr[i + 1u];

    t->shortCount = (uint8_t)(t->shortCount - 1u);
    t->generation = (uint8_t)(t->generation + 1u);
    return SRCMATCH_OK;
}

int SrcMatch_RemoveExt(SrcMatch_Table *t, const uint8_t ext[SRCMATCH_EXT_LEN])
{
    int idx;
    uint8_t i;
    uint8_t k;

    if (t == 0 || ext == 0)
        return SRCMATCH_BAD_LEN;

    idx = srcMatchExtIndex(t, ext);

    if (idx < 0)
        return SRCMATCH_ABSENT;

    for (i = (uint8_t)idx; (uint8_t)(i + 1u) < t->extCount; ++i)
    {
        for (k = 0; k < SRCMATCH_EXT_LEN; ++k)
            t->extAddr[i][k] = t->extAddr[i + 1u][k];
    }

    t->extCount = (uint8_t)(t->extCount - 1u);
    t->generation = (uint8_t)(t->generation + 1u);
    return SRCMATCH_OK;
}

int SrcMatch_HasShort(const SrcMatch_Table *t, uint16_t addr)
{
    if (t == 0)
        return 0;

    return srcMatchShortIndex(t, addr) >= 0 ? 1 : 0;
}

int SrcMatch_HasExt(const SrcMatch_Table *t, const uint8_t ext[SRCMATCH_EXT_LEN])
{
    if (t == 0 || ext == 0)
        return 0;

    return srcMatchExtIndex(t, ext) >= 0 ? 1 : 0;
}

uint16_t SrcMatch_EncodeShort(const SrcMatch_Table *t, U_BStream *dst)
{
    uint8_t i;
    uint16_t n = 0u;

    if (t == 0 || dst == 0)
        return 0u;

    if ((uint32_t)t->shortCount * 2u > (uint32_t)U_bstream_remaining(dst))
        return 0u;

    for (i = 0; i < t->shortCount; ++i)
    {
        U_bstream_put_u8(dst, (uint8_t)(t->shortAddr[i] & 0xFFu));
        U_bstream_put_u8(dst, (uint8_t)(t->shortAddr[i] >> 8));
        n = (uint16_t)(n + 2u);
    }

    return n;
}

uint16_t SrcMatch_EncodeExt(const SrcMatch_Table *t, U_BStream *dst)
{
    uint8_t i;
    uint16_t n = 0u;

    if (t == 0 || dst == 0)
        return 0u;

    if ((uint32_t)t->extCount * SRCMATCH_EXT_LEN > (uint32_t)U_bstream_remaining(dst))
        return 0u;

    for (i = 0; i < t->extCount; ++i)
    {
        U_bstream_put_bytes(dst, t->extAddr[i], SRCMATCH_EXT_LEN);
        n = (uint16_t)(n + SRCMATCH_EXT_LEN);
    }

    return n;
}

/* Byte at a time through a volatile destination: GCC recognises the plain loop as a memmove and
 * this -nostdlib image has none (docs/architecture.md §7, and props.c zeroes its buffers the same
 * way). */
static void srcMatchCopyBytes(volatile uint8_t *dst, const uint8_t *src, uint16_t n)
{
    uint16_t i;

    for (i = 0; i < n; ++i)
        dst[i] = src[i];
}

void SrcMatch_Copy(SrcMatch_Table *dst, const SrcMatch_Table *src)
{
    if (dst == 0 || src == 0)
        return;

    dst->enabled = src->enabled;
    dst->shortCount = src->shortCount;
    dst->extCount = src->extCount;
    dst->generation = src->generation;

    srcMatchCopyBytes((volatile uint8_t *)dst->shortAddr,
                      (const uint8_t *)src->shortAddr,
                      (uint16_t)((uint16_t)src->shortCount * 2u));
    srcMatchCopyBytes((volatile uint8_t *)dst->extAddr,
                      (const uint8_t *)src->extAddr,
                      (uint16_t)((uint16_t)src->extCount * SRCMATCH_EXT_LEN));
}

void SrcMatch_CopyOnAir(SrcMatch_Table *dst, const SrcMatch_Table *src)
{
    uint8_t i;
    uint8_t k;

    if (dst == 0 || src == 0)
        return;

    SrcMatch_Copy(dst, src);

    for (i = 0; i < src->extCount; ++i)
    {
        for (k = 0; k < SRCMATCH_EXT_LEN; ++k)
            dst->extAddr[i][k] = src->extAddr[i][SRCMATCH_EXT_LEN - 1u - k];
    }
}
