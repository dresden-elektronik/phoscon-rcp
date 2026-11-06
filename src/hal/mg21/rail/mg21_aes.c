/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mg21_aes.c — the block cipher behind lib/aes/aes.h on the part's RADIOAES engine.
 *
 * Software takes 219 us a block on this 38.4 MHz core and an Enhanced Ack leaves no time for four of
 * them. The engine is a pair of DMA channels: the fetcher reads a descriptor list holding the key, the
 * mode and the block, the pusher writes the result (EFR32xG21 reference manual, RADIOAES). One block
 * runs with interrupts masked, because the engine cannot be preempted and an Ack is computed from the
 * radio interrupt. */
#include <stdint.h>

#include "aes.h"
#include "mg21_rail_shims.h"

#define AES_FETCHADDR (*(volatile uint32_t *)0x54000000u)
#define AES_PUSHADDR (*(volatile uint32_t *)0x54000010u)
#define AES_CTRL (*(volatile uint32_t *)0x54000034u)
#define AES_CMD (*(volatile uint32_t *)0x54000038u)
#define AES_STATUS (*(volatile uint32_t *)0x5400003Cu)

#define AES_CTRL_SCATTER_GATHER 0x3u /* both channels follow descriptors */
#define AES_CMD_START 0x3u           /* both channels */
#define AES_STATUS_BUSY 0x3u

#define AES_NEXT_STOP 0x1u
#define AES_ENGINE 0x1u /* data flows through the AES core */
#define AES_TAG_CONFIG 0x10u
#define AES_TAG_LAST 0x20u
#define AES_TAG_OFFSET(a) ((uint32_t)(a) << 8)
#define AES_OFFSET_MODE 0u
#define AES_OFFSET_KEY 8u
#define AES_MODE_ECB_128_ENCRYPT 0x100u
#define AES_BLOCK 16u
#define AES_KEY 16u

typedef struct
{
    uint32_t address;
    uint32_t next;
    uint32_t length;
    uint32_t tag;
} Aes_Descriptor;

/* lib/aes/aes.h's object is kept whole, but the engine takes the key itself, so its schedule stays empty. */
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
    uint32_t mode = AES_MODE_ECB_128_ENCRYPT;
    Aes_Descriptor push = {(uint32_t)out, AES_NEXT_STOP, AES_BLOCK, AES_ENGINE | AES_TAG_LAST};
    Aes_Descriptor data = {(uint32_t)in, AES_NEXT_STOP, AES_BLOCK, AES_ENGINE | AES_TAG_LAST};
    Aes_Descriptor conf = {(uint32_t)&mode, (uint32_t)&data, sizeof(mode), AES_ENGINE | AES_TAG_CONFIG | AES_TAG_OFFSET(AES_OFFSET_MODE)};
    Aes_Descriptor keyd = {(uint32_t)key->key, (uint32_t)&conf, AES_KEY, AES_ENGINE | AES_TAG_CONFIG | AES_TAG_OFFSET(AES_OFFSET_KEY)};
    uint32_t depth = CORE_EnterAtomic();

    AES_CTRL = AES_CTRL_SCATTER_GATHER;
    AES_FETCHADDR = (uint32_t)&keyd;
    AES_PUSHADDR = (uint32_t)&push;
    AES_CMD = AES_CMD_START;

    while ((AES_STATUS & AES_STATUS_BUSY) != 0u)
    {
    }

    CORE_ExitAtomic(depth);
}
