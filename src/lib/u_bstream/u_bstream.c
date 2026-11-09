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

unsigned long U_bstream_pos(U_BStream *bs)
{
    if (!bs->data)
    {
        bs->status = U_BSTREAM_NOT_INITIALISED;
        return 0;
    }

    /* The current position, which is the number of bytes committed to the buffer.
     * A stream that failed part way still reports what was written here, where
     * the size minus U_bstream_remaining of it would not. */
    return bs->pos;
}

int U_bstream_fits(U_BStream *bs, unsigned long n)
{
    return n <= U_bstream_remaining(bs);
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

void U_bstream_put_s8(U_BStream *bs, signed char v)
{
    U_bstream_put_u8(bs, (unsigned char)v);
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
        bs->data[bs->pos++] = (unsigned char)((v >> 0) & 0xFF);
        bs->data[bs->pos++] = (unsigned char)((v >> 8) & 0xFF);
    }
}

void U_bstream_put_s16_le(U_BStream *bs, signed short v)
{
    U_bstream_put_u16_le(bs, (unsigned short)v);
}

void U_bstream_put_u16_be(U_BStream *bs, unsigned short v)
{
    if (U_bstream_verify_write(bs, 2))
    {
        bs->data[bs->pos++] = (unsigned char)((v >> 8) & 0xFF);
        bs->data[bs->pos++] = (unsigned char)((v >> 0) & 0xFF);
    }
}

void U_bstream_put_s16_be(U_BStream *bs, signed short v)
{
    U_bstream_put_u16_be(bs, (unsigned short)v);
}

void U_bstream_put_u32_le(U_BStream *bs, unsigned long v)
{
    if (U_bstream_verify_write(bs, 4))
    {
        bs->data[bs->pos++] = (unsigned char)((v >> 0) & 0xFF);
        bs->data[bs->pos++] = (unsigned char)((v >> 8) & 0xFF);
        bs->data[bs->pos++] = (unsigned char)((v >> 16) & 0xFF);
        bs->data[bs->pos++] = (unsigned char)((v >> 24) & 0xFF);
    }
}

void U_bstream_put_s32_le(U_BStream *bs, signed long v)
{
    U_bstream_put_u32_le(bs, (unsigned long)v);
}

void U_bstream_put_u32_be(U_BStream *bs, unsigned long v)
{
    if (U_bstream_verify_write(bs, 4))
    {
        bs->data[bs->pos++] = (unsigned char)((v >> 24) & 0xFF);
        bs->data[bs->pos++] = (unsigned char)((v >> 16) & 0xFF);
        bs->data[bs->pos++] = (unsigned char)((v >> 8) & 0xFF);
        bs->data[bs->pos++] = (unsigned char)((v >> 0) & 0xFF);
    }
}

void U_bstream_put_s32_be(U_BStream *bs, signed long v)
{
    U_bstream_put_u32_be(bs, (unsigned long)v);
}

/* Unsigned LEB128, the varint a Spinel packed uint is: 7 value bits per byte,
 * least significant group first, the MSB of a byte meaning "more bytes follow".
 * A 32-bit value needs at most 5 bytes. Only the low 32 bits of v are encoded,
 * as put_u32_le writes them. */
unsigned long U_bstream_leb128_u32_size(unsigned long v)
{
    unsigned long n;

    v &= 0xFFFFFFFFUL;
    n = 1;

    while (v >= 0x80UL)
    {
        v >>= 7;
        n++;
    }

    return n;
}

void U_bstream_put_leb128_u32(U_BStream *bs, unsigned long v)
{
    unsigned long n;

    v &= 0xFFFFFFFFUL;
    n = U_bstream_leb128_u32_size(v);

    /* The length is known up front, so the whole value is written or none of it. */
    if (U_bstream_verify_write(bs, n))
    {
        while (n > 1)
        {
            bs->data[bs->pos++] = (unsigned char)((v & 0x7FUL) | 0x80UL);
            v >>= 7;
            n--;
        }

        bs->data[bs->pos++] = (unsigned char)(v & 0x7FUL);
    }
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

signed char U_bstream_get_s8(U_BStream *bs)
{
    return (signed char)U_bstream_get_u8(bs);
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

signed short U_bstream_get_s16_be(U_BStream *bs)
{
    unsigned short u16;
    u16 = U_bstream_get_u16_be(bs);
    return (signed short)u16;
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

signed long U_bstream_get_s32_be(U_BStream *bs)
{
    unsigned long u;
    u = U_bstream_get_u32_be(bs);
    /* sign-extend the low 32 bits; portable on ILP32/LP64/LLP64, no long long */
    if (u & 0x80000000UL)
        u |= (unsigned long)~0xFFFFFFFFUL;
    return (signed long)u;
}

unsigned long U_bstream_get_leb128_u32(U_BStream *bs)
{
    unsigned long result;
    unsigned long shift;
    unsigned long chunk;
    unsigned char byte;

    result = 0;
    shift = 0;

    if (!bs->data)
    {
        bs->status = U_BSTREAM_NOT_INITIALISED;
        return 0;
    }

    if (bs->status != U_BSTREAM_OK)
        return 0;

    while (bs->pos < bs->size)
    {
        byte = bs->data[bs->pos++];
        chunk = (unsigned long)(byte & 0x7F);

        if (shift >= 32 || (shift > 25 && chunk > (0xFFFFFFFFUL >> shift)))
        {
            /* more than 5 bytes, or a group whose bits fall above 32 */
            bs->status = U_BSTREAM_DECODE_ERROR;
            return 0;
        }

        result |= chunk << shift;

        if ((byte & 0x80) == 0)
            return result;

        shift += 7;
    }

    /* ran out of bytes before a terminating group */
    bs->status = U_BSTREAM_DECODE_ERROR;
    return 0;
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
    unsigned long i;
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

void U_bstream_put_u64_be(U_BStream *bs, unsigned long long v)
{
    int i;
    if (U_bstream_verify_write(bs, 8))
    {
        for (i = 7; i >= 0; i--)
            bs->data[bs->pos++] = (unsigned char)((v >> (8 * i)) & 0xFF);
    }
}

void U_bstream_put_s64_be(U_BStream *bs, long long v)
{
    U_bstream_put_u64_be(bs, (unsigned long long)v);
}

unsigned long long U_bstream_get_u64_be(U_BStream *bs)
{
    unsigned long i;
    unsigned long long result;
    result = 0;

    if (U_bstream_verify_read(bs, 8))
    {
        for (i = 0; i < 8; i++)
            result |= (unsigned long long)bs->data[bs->pos + i] << (8 * (7 - i));
        bs->pos += 8;
    }

    return result;
}

long long U_bstream_get_s64_be(U_BStream *bs)
{
    return (long long)U_bstream_get_u64_be(bs);
}
#endif
