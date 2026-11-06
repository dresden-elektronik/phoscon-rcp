/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* avr_aes.c — the block cipher behind lib/aes/aes.h on the transceiver's AES engine (docs/hardware_avr.md
 * §11.7).
 *
 * The engine takes 24 us a block and is reached by 16 stores and 16 loads. The radio vector computes an
 * Enhanced Ack on it while the loop may be securing a frame, and its byte counters and its one key belong
 * to one block at a time, so a block holds the interrupts for its 40 us. */
#include <stdint.h>

#include "aes.h"
#include "avr_regs.h"

#define AES_KEY_LEN 16u
#define AES_BLOCK_LEN 16u

static uint8_t aesEngineKey[AES_KEY_LEN];
static uint8_t aesEngineKeySet;

ZbStatus Aes_KeySchedule(Aes_Key *key, const uint8_t key16[16])
{
    uint8_t i;

    if (key == 0 || key16 == 0)
        return ZB_ERR_BAD_ARGUMENT;

    for (i = 0u; i < AES_KEY_LEN; ++i)
        key->key[i] = key16[i];

    return ZB_OK;
}

void Aes_KeyWipe(Aes_Key *key)
{
    volatile uint8_t *p = (volatile uint8_t *)key;
    uint16_t i;

    if (key == 0)
        return;

    for (i = 0u; i < sizeof(*key); ++i)
        p[i] = 0u;
}

void Aes_EncryptBlock(const Aes_Key *key, const uint8_t in[16], uint8_t out[16])
{
    uint8_t sreg = AvrOwn_LockSave();
    uint8_t i;
    uint8_t same = aesEngineKeySet;

    for (i = 0u; same && i < AES_KEY_LEN; ++i)
        same = aesEngineKey[i] == key->key[i];

    if (!same)
    {
        for (i = 0u; i < AES_KEY_LEN; ++i)
        {
            AVR_REG8(AVR_AES_KEY) = key->key[i];
            aesEngineKey[i] = key->key[i];
        }

        aesEngineKeySet = 1u;
    }

    for (i = 0u; i < AES_BLOCK_LEN; ++i)
        AVR_REG8(AVR_AES_STATE) = in[i];

    /* ECB, encryption: both mode bits clear. */
    AVR_REG8(AVR_AES_CTRL) = AVR_BIT(AVR_AES_REQUEST);

    /* Not bounded on purpose: a block handed back unfinished is the plaintext, which CTR would then use as
     * its keystream. An engine that never finishes is the watchdog's. */
    while ((AVR_REG8(AVR_AES_STATUS) & AVR_BIT(AVR_AES_DONE)) == 0u)
    {
    }

    for (i = 0u; i < AES_BLOCK_LEN; ++i)
        out[i] = AVR_REG8(AVR_AES_STATE);

    AvrOwn_LockRestore(sreg);
}
