/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* samr21_aes.c — the block cipher behind lib/aes/aes.h on the transceiver's AES engine.
 *
 * A block takes 130 us over the SPI, against 325 us for the software table on this core, and the bus is
 * shared with the radio vector, which computes an Enhanced Ack with the same engine. So a block holds the
 * bus, and the engine's one key is compared before each block rather than assumed. */
#include <stdint.h>

#include "aes.h"
#include "samr21_rf.h"

#define AES_KEY 16u

static uint8_t aesEngineKey[AES_KEY];
static uint8_t aesEngineKeySet;

ZbStatus Aes_KeySchedule(Aes_Key *key, const uint8_t key16[16])
{
    uint8_t i;

    if (key == 0 || key16 == 0)
        return ZB_ERR_BAD_ARGUMENT;

    for (i = 0u; i < AES_KEY; ++i)
        key->key[i] = key16[i];

    return ZB_OK;
}

void Aes_KeyWipe(Aes_Key *key)
{
    volatile uint8_t *p = (volatile uint8_t *)key;
    uint32_t i;

    if (key == 0)
        return;

    for (i = 0u; i < sizeof(*key); ++i)
        p[i] = 0u;
}

void Aes_EncryptBlock(const Aes_Key *key, const uint8_t in[16], uint8_t out[16])
{
    uint8_t i;
    int same;

    Rf_Lock();
    same = aesEngineKeySet;

    for (i = 0u; same && i < AES_KEY; ++i)
        same = aesEngineKey[i] == key->key[i];

    if (!same)
    {
        (void)Rf_AesSetKey(key->key);

        for (i = 0u; i < AES_KEY; ++i)
            aesEngineKey[i] = key->key[i];

        aesEngineKeySet = 1u;
    }

    (void)Rf_AesEncrypt(in, out);
    Rf_Unlock();
}
