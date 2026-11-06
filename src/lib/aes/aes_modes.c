/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Manuel Pietschmann */
#include "aes.h"

/*
 * The mode implementations of the AES band: CTR (SP 800-38A) and CMAC (SP 800-38B),
 * every block encryption through the Aes_EncryptBlock seam. The CCM* functions
 * follow in a later pass.
 */

/* Bump the counter as one 16-octet big-endian integer (SP 800-38A §6.5): the carry
   may propagate from the last octet to the first. */
static void aesDoCtrAddOne(uint8_t counter[16])
{
    uint32_t i;

    for (i = 15; i >= 1; i--)
    {
        counter[i] = (uint8_t)(counter[i] + 1);

        if (counter[i] != 0)
            return;
    }

    counter[0] = (uint8_t)(counter[0] + 1);
}

/* The subkey shift (SP 800-38B): the 16 octets left-shifted by 1; when the
   most-significant bit was dropped, 0x87 is folded into the last octet (the
   reduction constant for a 128-bit block). */
static void aesDoSubkeyShift(const uint8_t in[16], uint8_t out[16])
{
    uint8_t overflow = in[0] >> 7;
    uint32_t i;

    for (i = 0; i < 15; i++)
        out[i] = (uint8_t)((in[i] << 1) | (in[i + 1] >> 7));

    out[15] = (uint8_t)(in[15] << 1);

    if (overflow != 0)
        out[15] = (uint8_t)(out[15] ^ 0x87);
}

void Aes_CtrCipher(const Aes_Key *key, uint8_t counter[16],
                   const uint8_t *in, uint32_t inLen, uint8_t *out)
{
    uint8_t keystream[16];
    uint32_t offset;
    uint32_t blockLen;
    uint32_t octet;

    if (inLen == 0)
        return;

    for (offset = 0; offset < inLen; offset += 16)
    {
        blockLen = inLen - offset;

        if (blockLen > 16)
            blockLen = 16;

        Aes_EncryptBlock(key, counter, keystream);

        for (octet = 0; octet < blockLen; octet++)
            out[offset + octet] = (uint8_t)(in[offset + octet] ^ keystream[octet]);

        aesDoCtrAddOne(counter);
    }
}

ZbStatus Aes_Cmac(const Aes_Key *key, const uint8_t *in, uint32_t inLen,
                  uint8_t *mic, uint32_t micLen)
{
    uint8_t zero[16] = {0};
    uint8_t l[16];
    uint8_t k1[16];
    uint8_t k2[16];
    uint8_t x[16] = {0};
    uint8_t y[16];
    uint8_t last[16];
    uint8_t t[16];
    uint32_t fullBlocks;
    uint32_t rem;
    uint32_t block;
    uint32_t i;

    if (in == 0 || mic == 0 || micLen < 8 || micLen > 16)
        return ZB_ERR_BAD_ARGUMENT;

    Aes_EncryptBlock(key, zero, l);

    aesDoSubkeyShift(l, k1);

    aesDoSubkeyShift(k1, k2);

    rem = inLen % 16;

    if (inLen != 0 && rem == 0)
        fullBlocks = inLen / 16 - 1;
    else
        fullBlocks = inLen / 16;

    for (block = 0; block < fullBlocks; block++)
    {
        for (i = 0; i < 16; i++)
            y[i] = (uint8_t)(x[i] ^ in[16 * block + i]);

        Aes_EncryptBlock(key, y, x);
    }

    if (inLen != 0 && rem == 0)
    {
        for (i = 0; i < 16; i++)
            last[i] = (uint8_t)(in[inLen - 16 + i] ^ k1[i]);
    }
    else
    {
        for (i = 0; i < 16; i++)
            last[i] = 0;

        for (i = 0; i < rem; i++)
            last[i] = in[inLen - rem + i];

        last[rem] = 0x80;

        for (i = 0; i < 16; i++)
            last[i] = (uint8_t)(last[i] ^ k2[i]);
    }

    for (i = 0; i < 16; i++)
        y[i] = (uint8_t)(x[i] ^ last[i]);

    Aes_EncryptBlock(key, y, t);

    for (i = 0; i < micLen; i++)
        mic[i] = t[i];

    return ZB_OK;
}

/* CCM* (IEEE 802.15.4-2024 Annex B.3; identical to SP 800-38C): the tag is the
   leftmost micLen octets of S0 XOR the CBC-MAC over B0, the A' blocks, and the
   P' blocks, and the payload blocks ride the counter from index 1, so Ctr_0
   is reserved for S0. */

/* The largest frame the security band protects; the decrypt staging buffer is
   this many octets, and an inLen above it is a bad argument. */
#define Aes_MAX_LEN 1024

/* The argument check shared by both directions; in 0 is allowed with inLen 0. */
static ZbStatus aesDoCcmCheck(const Aes_Key *key, const uint8_t *nonce, uint32_t nonceLen,
                              const uint8_t *adata, uint32_t adataLen,
                              const uint8_t *in, uint32_t inLen,
                              const uint8_t *mic, uint32_t micLen)
{
    uint32_t l;

    if (key == 0 || nonce == 0 || mic == 0 || (in == 0 && inLen > 0))
        return ZB_ERR_BAD_ARGUMENT;

    if (nonceLen < 7 || nonceLen > 13)
        return ZB_ERR_BAD_ARGUMENT;

    if (micLen != 4 && micLen != 6 && micLen != 8 &&
        micLen != 10 && micLen != 12 && micLen != 14 && micLen != 16)
        return ZB_ERR_BAD_ARGUMENT;

    l = 15 - nonceLen;

    if (l < 4 && inLen >= (uint32_t)1 << (8 * l))
        return ZB_ERR_BAD_ARGUMENT;

    if (adata == 0 && adataLen > 0)
        return ZB_ERR_BAD_ARGUMENT;

    if (inLen > Aes_MAX_LEN)
        return ZB_ERR_BAD_ARGUMENT;

    return ZB_OK;
}

/* The l-octet big-endian form of a uint32 count in field (l in 2..8): the count
   fits in 4 octets, so the leftmost l - 4 octets are zero and no shift of 32
   or more occurs. */
static void aesDoCountField(uint8_t *field, uint32_t l, uint32_t value)
{
    uint32_t octet;

    for (octet = 0; octet < l; octet++)
        field[octet] = 0;

    if (l >= 4)
    {
        for (octet = 0; octet < 4; octet++)
            field[l - 4 + octet] = (uint8_t)(value >> (8 * (3 - octet)));
    }
    else
    {
        for (octet = 0; octet < l; octet++)
            field[octet] = (uint8_t)(value >> (8 * (l - 1 - octet)));
    }
}

/* Ctr_i = (L - 1) || nonce || i, with i in L octets, big-endian; the flag octet
   carries only the L field. */
static void aesDoCcmCounter(const uint8_t *nonce, uint32_t nonceLen, uint32_t l,
                            uint32_t index, uint8_t ctr[16])
{
    uint32_t octet;

    ctr[0] = (uint8_t)(l - 1);

    for (octet = 0; octet < nonceLen; octet++)
        ctr[1 + octet] = nonce[octet];

    aesDoCountField(ctr + 16 - l, l, index);
}

/* B0 = flags || nonce || inLen (L octets, big-endian); the flags octet packs the
   Adata, M, and L fields: (M - 2) / 2 and L - 1 in their 3-bit slots. */
static void aesDoCcmB0(uint8_t *b0, const uint8_t *nonce, uint32_t nonceLen,
                       uint32_t adataLen, uint32_t inLen, uint32_t micLen)
{
    uint32_t l = 15 - nonceLen;
    uint32_t flags;
    uint32_t octet;

    flags = (((micLen - 2) / 2) << 3) | (l - 1);

    if (adataLen > 0)
        flags = flags | 0x40;

    b0[0] = (uint8_t)flags;

    for (octet = 0; octet < nonceLen; octet++)
        b0[1 + octet] = nonce[octet];

    aesDoCountField(b0 + 16 - l, l, inLen);
}

/* The octet at offset off of A' = length prefix || adata || zero padding. The
   prefix is 2 octets below 65280 and 0xff 0xfe plus the 4-octet length above,
   so that encodings of distinct lengths never overlap. */
static uint8_t aesDoCcmAOctet(uint32_t prefixLen, const uint8_t *adata,
                              uint32_t adataLen, uint32_t off)
{
    if (off < prefixLen)
    {
        if (prefixLen == 2)
            return (uint8_t)(adataLen >> (8 * (1 - off)));

        if (off == 0)
            return 0xff;

        if (off == 1)
            return 0xfe;

        return (uint8_t)(adataLen >> (8 * (5 - off)));
    }

    if (off - prefixLen < adataLen)
        return adata[off - prefixLen];

    return 0;
}

/* The octet at offset off of P' = in || zero padding. */
static uint8_t aesDoCcmPOctet(const uint8_t *in, uint32_t inLen, uint32_t off)
{
    if (off < inLen)
        return in[off];

    return 0;
}

/* The xor-accumulate of two octet strings: 0 when they agree on all len octets. */
static uint8_t aesDoCcmDiff(const uint8_t a[16], const uint8_t b[16], uint32_t len)
{
    uint8_t diff = 0;
    uint32_t octet;

    for (octet = 0; octet < len; octet++)
        diff = diff | (a[octet] ^ b[octet]);

    return diff;
}

/* The CCM tag: S0 = E(Ctr_0); X = E(X XOR B0), then X = E(X XOR block) over every
   A' block, then every P' block; the tag is the leftmost micLen octets of S0 XOR X.
   adata is streamed into the MAC block by block and never staged. */
static void aesDoCcmTag(const Aes_Key *key, const uint8_t b0[16],
                        const uint8_t *nonce, uint32_t nonceLen,
                        const uint8_t *adata, uint32_t adataLen,
                        const uint8_t *in, uint32_t inLen,
                        uint8_t *tag, uint32_t micLen)
{
    uint8_t ctr[16];
    uint8_t s0[16];
    uint8_t x[16];
    uint8_t block[16];
    uint32_t prefixLen;
    uint32_t adataBlocks;
    uint32_t inBlocks;
    uint32_t blockIndex;
    uint32_t octet;

    aesDoCcmCounter(nonce, nonceLen, 15 - nonceLen, 0, ctr);

    Aes_EncryptBlock(key, ctr, s0);

    for (octet = 0; octet < 16; octet++)
        x[octet] = 0;

    for (octet = 0; octet < 16; octet++)
        block[octet] = b0[octet];

    Aes_EncryptBlock(key, block, x);

    if (adataLen > 0)
    {
        if (adataLen < 65280)
            prefixLen = 2;
        else
            prefixLen = 6;

        adataBlocks = adataLen / 16 + (prefixLen + adataLen % 16 + 15) / 16;

        for (blockIndex = 0; blockIndex < adataBlocks; blockIndex++)
        {
            for (octet = 0; octet < 16; octet++)
                block[octet] = (uint8_t)(x[octet] ^ aesDoCcmAOctet(prefixLen, adata, adataLen, 16 * blockIndex + octet));

            Aes_EncryptBlock(key, block, x);
        }
    }

    inBlocks = (inLen + 15) / 16;

    for (blockIndex = 0; blockIndex < inBlocks; blockIndex++)
    {
        for (octet = 0; octet < 16; octet++)
            block[octet] = (uint8_t)(x[octet] ^ aesDoCcmPOctet(in, inLen, 16 * blockIndex + octet));

        Aes_EncryptBlock(key, block, x);
    }

    for (octet = 0; octet < micLen; octet++)
        tag[octet] = (uint8_t)(s0[octet] ^ x[octet]);
}

ZbStatus Aes_CcmEncrypt(const Aes_Key *key,
                        const uint8_t *nonce, uint32_t nonceLen,
                        const uint8_t *adata, uint32_t adataLen,
                        const uint8_t *in, uint32_t inLen,
                        uint32_t micLen,
                        uint8_t *out, uint8_t *mic)
{
    uint8_t b0[16];
    uint8_t ctr[16];
    uint8_t ks[16];
    uint32_t l;
    uint32_t inBlocks;
    uint32_t blockIndex;
    uint32_t base;
    uint32_t blockLen;
    uint32_t octet;

    if (aesDoCcmCheck(key, nonce, nonceLen, adata, adataLen, in, inLen, mic, micLen) != ZB_OK)
        return ZB_ERR_BAD_ARGUMENT;

    if (out == 0)
        return ZB_ERR_BAD_ARGUMENT;

    l = 15 - nonceLen;

    aesDoCcmB0(b0, nonce, nonceLen, adataLen, inLen, micLen);

    aesDoCcmTag(key, b0, nonce, nonceLen, adata, adataLen, in, inLen, mic, micLen);

    inBlocks = (inLen + 15) / 16;

    for (blockIndex = 0; blockIndex < inBlocks; blockIndex++)
    {
        base = 16 * blockIndex;
        blockLen = inLen - base;

        if (blockLen > 16)
            blockLen = 16;

        aesDoCcmCounter(nonce, nonceLen, l, blockIndex + 1, ctr);

        Aes_EncryptBlock(key, ctr, ks);

        for (octet = 0; octet < blockLen; octet++)
            out[base + octet] = (uint8_t)(in[base + octet] ^ ks[octet]);
    }

    return ZB_OK;
}

ZbStatus Aes_CcmDecrypt(const Aes_Key *key,
                        const uint8_t *nonce, uint32_t nonceLen,
                        const uint8_t *adata, uint32_t adataLen,
                        const uint8_t *in, uint32_t inLen,
                        const uint8_t *mic, uint32_t micLen,
                        uint8_t *out)
{
    uint8_t b0[16];
    uint8_t ctr[16];
    uint8_t ks[16];
    uint8_t p[Aes_MAX_LEN];
    uint8_t tag[16];
    uint32_t l;
    uint32_t inBlocks;
    uint32_t blockIndex;
    uint32_t base;
    uint32_t blockLen;
    uint32_t octet;

    if (aesDoCcmCheck(key, nonce, nonceLen, adata, adataLen, in, inLen, mic, micLen) != ZB_OK)
        return ZB_ERR_BAD_ARGUMENT;

    l = 15 - nonceLen;

    aesDoCcmB0(b0, nonce, nonceLen, adataLen, inLen, micLen);

    inBlocks = (inLen + 15) / 16;

    for (blockIndex = 0; blockIndex < inBlocks; blockIndex++)
    {
        base = 16 * blockIndex;
        blockLen = inLen - base;

        if (blockLen > 16)
            blockLen = 16;

        aesDoCcmCounter(nonce, nonceLen, l, blockIndex + 1, ctr);

        Aes_EncryptBlock(key, ctr, ks);

        for (octet = 0; octet < blockLen; octet++)
            p[base + octet] = (uint8_t)(in[base + octet] ^ ks[octet]);
    }

    aesDoCcmTag(key, b0, nonce, nonceLen, adata, adataLen, p, inLen, tag, micLen);

    if (aesDoCcmDiff(tag, mic, micLen) != 0)
        return ZB_ERR_AUTH;

    for (octet = 0; octet < inLen; octet++)
        out[octet] = p[octet];

    return ZB_OK;
}
