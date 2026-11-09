/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* flasher_trap.c - the host flasher's reset request (flasher_trap.h). */
#include "flasher_trap.h"

#include "hal.h"

/* GCF WRITE_PARAMETER frame (the flasher's PROT_SendFlagged): C0, 12 payload bytes, 2-byte CRC, C0;
 * no payload byte equals C0/DB, so nothing is escaped. */
static const uint8_t flasherResetFrame[16] = {
    0xC0, 0x0B, 0x03, 0x00, 0x0C, 0x00, 0x05, 0x00, 0x26, 0x02, 0x00, 0x00, 0x00, 0xB9, 0xFF, 0xC0};

#define FRAME_LEN 16u

/* The response: command 0x0B, sequence 3, status 0, frame length 8, payload length 1, parameter 0x26,
 * then the checksum - the two's complement of the byte sum, low byte first - and the closing C0. */
static const uint8_t flasherAckFrame[FLASHER_TRAP_ACK_LEN] = {
    0xC0, 0x0B, 0x03, 0x00, 0x08, 0x00, 0x01, 0x00, 0x26, 0xC3, 0xFF, 0xC0};

const uint8_t *FlasherTrap_Ack(void)
{
    return flasherAckFrame;
}

#define HDLC_FLAG 0x7Eu
#define SPINEL_HEADER_MASK 0xC0u
#define SPINEL_HEADER_FLAG 0x80u

static uint8_t tail[FLASHER_TRAP_TAIL];
static uint8_t tailPos;
static uint32_t tailCount;
/* Where the stream is: just past a flag, inside a Spinel frame, or anywhere else. */
static uint8_t afterFlag;
static uint8_t inSpinel;
static uint32_t lastFeedMs;
/* Written from the link interrupt, read from the main loop -> volatile. The deadline is separate
 * from the flag because a monotonic deadline can legitimately be 0. */
static volatile uint32_t armedAtMs;
static volatile int armed;

static int tailMatchesFrame(void)
{
    uint32_t start = ((uint32_t)tailPos - FRAME_LEN) & (FLASHER_TRAP_TAIL - 1u);
    uint16_t i;

    for (i = 0; i < FRAME_LEN; ++i)
    {
        if (tail[(start + i) & (FLASHER_TRAP_TAIL - 1u)] != flasherResetFrame[i])
            return 0;
    }

    return 1;
}

int FlasherTrap_Feed(const uint8_t *buf, uint16_t len)
{
    uint32_t now;
    uint16_t i;

    if (buf == 0)
        return 0;

    Tick_GetMs(&now);

    if ((uint32_t)(now - lastFeedMs) >= FLASHER_TRAP_IDLE_MS)
        inSpinel = 0u;

    lastFeedMs = now;

    for (i = 0; i < len; ++i)
    {
        uint8_t b = buf[i];

        if (b == HDLC_FLAG)
        {
            inSpinel = 0u;
            afterFlag = 1u;
            tailCount = 0u;
            continue;
        }

        if (afterFlag)
        {
            afterFlag = 0u;
            inSpinel = ((b & SPINEL_HEADER_MASK) == SPINEL_HEADER_FLAG) ? 1u : 0u;
        }

        if (inSpinel)
        {
            tailCount = 0u;
            continue;
        }

        tail[tailPos] = b;
        tailPos = (uint8_t)((tailPos + 1u) & (FLASHER_TRAP_TAIL - 1u));
        tailCount++;
    }

    if (tailCount < FRAME_LEN)
        return 0;

    if (!tailMatchesFrame())
        return 0;

    armedAtMs = now + FLASHER_TRAP_DELAY_MS;
    armed = 1;

    return 1;
}

int FlasherTrap_Due(uint32_t ms)
{
    if (!armed)
        return 0;

    if ((int32_t)(ms - armedAtMs) < 0)
        return 0;

    armed = 0;
    return 1;
}
