/*
 * Copyright (c) 2023 Manuel Pietschmann.
 * All rights reserved.
 *
 * The software in this package is published under the terms of the BSD
 * style license a copy of which has been included with this distribution in
 * the LICENSE.txt file.
 *
 */

#include "u_bstream.h"

void U_bstream_init(U_BStream *bs, void *data, unsigned long size)
{
    bs->data = (unsigned char*)data;
    bs->pos = 0;
    bs->size = size;
    bs->status = U_BSTREAM_OK;
}

unsigned long U_bstream_remaining(U_BStream *bs)
{
    if (!bs->data)
    {
        bs->status = U_BSTREAM_NOT_INITIALISED;
        return 0;
    }

    if (bs->status == U_BSTREAM_OK && bs->pos <= bs->size)
        return bs->size - bs->pos;

    return 0;
}

void U_bstream_advance(U_BStream *bs, unsigned long n)
{
    if (!bs->data)
    {
        bs->status = U_BSTREAM_NOT_INITIALISED;
        return;
    }

    if (bs->status == U_BSTREAM_OK)
    {
        if (n <= U_bstream_remaining(bs))
        {
            bs->pos += n;
        }
        else
        {
            bs->status = U_BSTREAM_WRITE_PAST_END;
        }
    }
}

void U_bstream_seek(U_BStream *bs, unsigned long pos)
{
    if (!bs->data)
    {
        bs->status = U_BSTREAM_NOT_INITIALISED;
        return;
    }

    if (bs->status == U_BSTREAM_OK)
    {
        if (pos <= bs->size)
        {
            bs->pos = pos;
        }
        else
        {
            bs->status = U_BSTREAM_WRITE_PAST_END;
        }
    }
}

static int U_bstream_verify_write(U_BStream *bs, unsigned long size)
{
    if (bs->status != U_BSTREAM_OK)
        return 0;

    if (!bs->data)
    {
        bs->status = U_BSTREAM_NOT_INITIALISED;
        return 0;
    }

    if ((bs->pos + size) > bs->size)
    {
        bs->status = U_BSTREAM_WRITE_PAST_END;
        return 0;
    }

    return 1;
}

static int U_bstream_verify_read(U_BStream *bs, unsigned long size)
{
    if (bs->status != U_BSTREAM_OK)
        return 0;

    if (!bs->data)
    {
        bs->status = U_BSTREAM_NOT_INITIALISED;
        return 0;
    }

    if ((bs->pos + size) > bs->size)
    {
        bs->status = U_BSTREAM_READ_PAST_END;
        return 0;
    }

    return 1;
}

void U_bstream_put_u8(U_BStream *bs, unsigned char v)
{
    if (U_bstream_verify_write(bs, 1))
    {
        bs->data[bs->pos++] = v;
    }
}

void U_bstream_put_bytes(U_BStream *bs, const void *src, unsigned long n)
{
    const unsigned char *s;
    unsigned long i;

    s = (const unsigned char *)src;

    if (U_bstream_verify_write(bs, n))
    {
        for (i = 0; i < n; i++)
            bs->data[bs->pos++] = s[i];
    }
}

void U_bstream_put_u16_le(U_BStream *bs, unsigned short v)
{
    if (U_bstream_verify_write(bs, 2))
    {
        bs->data[bs->pos++] = (v >> 0) & 0xFF;
        bs->data[bs->pos++] = (v >> 8) & 0xFF;
    }
}

void U_bstream_put_s16_le(U_BStream *bs, signed short v)
{
    U_bstream_put_u16_le(bs, (unsigned short)v);
}

void U_bstream_put_u32_le(U_BStream *bs, unsigned long v)
{
    if (U_bstream_verify_write(bs, 4))
    {
        bs->data[bs->pos++] = (v >> 0) & 0xFF;
        bs->data[bs->pos++] = (v >> 8) & 0xFF;
        bs->data[bs->pos++] = (v >> 16) & 0xFF;
        bs->data[bs->pos++] = (v >> 24) & 0xFF;
    }
}

void U_bstream_put_s32_le(U_BStream *bs, signed long v)
{
    U_bstream_put_u32_le(bs, (unsigned long)v);
}

unsigned char U_bstream_get_u8(U_BStream *bs)
{
    unsigned char result;
    result = 0;

    if (U_bstream_verify_read(bs, 1))
    {
        result = bs->data[bs->pos];
        bs->pos++;
    }

    return result;
}

void U_bstream_get_bytes(U_BStream *bs, void *dst, unsigned long n)
{
    unsigned char *d;
    unsigned long i;

    d = (unsigned char *)dst;

    if (U_bstream_verify_read(bs, n))
    {
        for (i = 0; i < n; i++)
            d[i] = bs->data[bs->pos++];
    }
}

unsigned short U_bstream_get_u16_le(U_BStream *bs)
{
    unsigned short result;
    result = 0;

    if (U_bstream_verify_read(bs, 2))
    {
        result = bs->data[bs->pos + 1];
        result <<= 8;
        result |= bs->data[bs->pos];
        bs->pos += 2;
    }

    return result;
}

signed short U_bstream_get_s16_le(U_BStream *bs)
{
    unsigned short u16;
    u16 = U_bstream_get_u16_le(bs);
    return (signed short)u16;
}

unsigned short U_bstream_get_u16_be(U_BStream *bs)
{
    unsigned short result;
    result = 0;

    if (U_bstream_verify_read(bs, 2))
    {
        result = bs->data[bs->pos];
        result <<= 8;
        result |= bs->data[bs->pos + 1];
        bs->pos += 2;
    }

    return result;
}

unsigned long U_bstream_get_u32_le(U_BStream *bs)
{
    unsigned long result;
    result = 0;

    if (U_bstream_verify_read(bs, 4))
    {
        result = bs->data[bs->pos + 3];
        result <<= 8;
        result |= bs->data[bs->pos + 2];
        result <<= 8;
        result |= bs->data[bs->pos + 1];
        result <<= 8;
        result |= bs->data[bs->pos + 0];
        bs->pos += 4;
    }

    return result;
}

signed long U_bstream_get_s32_le(U_BStream *bs)
{
    unsigned long u;
    u = U_bstream_get_u32_le(bs);
    /* sign-extend the low 32 bits; portable on ILP32/LP64/LLP64, no long long */
    if (u & 0x80000000UL)
        u |= (unsigned long)~0xFFFFFFFFUL;
    return (signed long)u;
}

unsigned long U_bstream_get_u32_be(U_BStream *bs)
{
    unsigned long result;
    result = 0;

    if (U_bstream_verify_read(bs, 4))
    {
        result = bs->data[bs->pos];
        result <<= 8;
        result |= bs->data[bs->pos + 1];
        result <<= 8;
        result |= bs->data[bs->pos + 2];
        result <<= 8;
        result |= bs->data[bs->pos + 3];
        bs->pos += 4;
    }

    return result;
}

#ifdef U_BSTREAM_HAS_LONG_LONG
void U_bstream_put_leb128_s64(U_BStream *bs, long long v)
{

    unsigned char byte;
    unsigned long long u;

    if (!bs->data)
    {
        bs->status = U_BSTREAM_NOT_INITIALISED;
        return;
    }

    /* zigzag encoding to unsigned (UB-free, correct for LLONG_MIN) */
    u = (unsigned long long)v;
    u = (u << 1) ^ (0ULL - (u >> 63));

    if (bs->status != U_BSTREAM_OK)
        return;

    do
    {
        byte = (unsigned char)(u & 0x7F);
        u >>= 7;
        if (u != 0) byte |= 0x80; /* continuation */
        if (bs->pos >= bs->size)
        {
            bs->status = U_BSTREAM_WRITE_PAST_END;
            return;
        }

        bs->data[bs->pos++] = byte;
    } while (u != 0);
}

long long U_bstream_get_leb128_s64(U_BStream *bs)
{
    unsigned shift;
    unsigned char byte;
    unsigned long long uval;

    if (!bs->data)
    {
        bs->status = U_BSTREAM_NOT_INITIALISED;
        return 0;
    }

    if (bs->status != U_BSTREAM_OK)
        return 0;

    shift = 0;
    uval = 0;

    for (;bs->pos < bs->size;)
    {
        byte = bs->data[bs->pos++];
        uval |= (unsigned long long)(byte & 0x7F) << shift;

        if ((byte & 0x80) == 0)
        {
            /* zigzag decode */
            if ((uval & 1) == 0)
                return (long long)(uval >> 1);

            uval >>= 1; /* floor(uval/2) */
            return -(long long)(uval + 1);
        }

        shift += 7;
        if (shift >= (unsigned)(sizeof(unsigned long long) * 8))
        {
            /* overflow / value too large for chosen result type */
            uval = 0;
            bs->status = U_BSTREAM_DECODE_ERROR;
            return 0;
        }
    }

    /* ran out of bytes before terminating */
    bs->status = U_BSTREAM_DECODE_ERROR;
    return 0;
}

void U_bstream_put_u64_le(U_BStream *bs, unsigned long long v)
{
    int i;
    if (U_bstream_verify_write(bs, 8))
    {
        for (i = 0; i < 8; i++)
            bs->data[bs->pos++] = (unsigned char)((v >> (8 * i)) & 0xFF);
    }
}

void U_bstream_put_s64_le(U_BStream *bs, long long v)
{
    U_bstream_put_u64_le(bs, (unsigned long long)v);
}

unsigned long long U_bstream_get_u64_le(U_BStream *bs)
{
    int i;
    unsigned long long result;
    result = 0;

    if (U_bstream_verify_read(bs, 8))
    {
        for (i = 0; i < 8; i++)
            result |= (unsigned long long)bs->data[bs->pos + i] << (8 * i);
        bs->pos += 8;
    }

    return result;
}

long long U_bstream_get_s64_le(U_BStream *bs)
{
    return (long long)U_bstream_get_u64_le(bs);
}
#endif
