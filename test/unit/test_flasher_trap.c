/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* test_flasher_trap.c - the host flasher's reset request (src/app/flasher_trap.c). */
#include <stdio.h>

#include "flasher_trap.h"

/* The module reads the tick only when the frame matches, so the suite owns time. */
static uint32_t fakeMs;

void Tick_GetMs(uint32_t *ms)
{
    *ms = fakeMs;
}

static int failures;

static void check(const char *what, int ok)
{
    if (!ok)
    {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static const uint8_t frame[16] = {0xC0, 0x0B, 0x03, 0x00, 0x0C, 0x00, 0x05, 0x00,
                                  0x26, 0x02, 0x00, 0x00, 0x00, 0xB9, 0xFF, 0xC0};

int main(void)
{
    uint8_t noise[40];
    uint8_t bad[16];
    uint16_t i;

    for (i = 0u; i < (uint16_t)sizeof(noise); ++i)
        noise[i] = (uint8_t)(0x11u + i);
    for (i = 0u; i < 16u; ++i)
        bad[i] = frame[i];
    bad[7] ^= 0x01u;

    /* A whole frame in one feed arms the trap, and it fires once, after the delay. */
    fakeMs = 1000u;
    check("a whole frame arms", FlasherTrap_Feed(frame, 16u) == 1);
    check("not due before the delay", FlasherTrap_Due(1000u + FLASHER_TRAP_DELAY_MS - 1u) == 0);
    check("due at the delay", FlasherTrap_Due(1000u + FLASHER_TRAP_DELAY_MS) == 1);
    check("fires only once", FlasherTrap_Due(1000u + FLASHER_TRAP_DELAY_MS) == 0);

    /* Split across two packets, which is what the tail is for. */
    check("frame split in two arms", FlasherTrap_Feed(frame, 8u) == 0);
    check("second half arms", FlasherTrap_Feed(frame + 8, 8u) == 1);
    check("split frame is due", FlasherTrap_Due(fakeMs + FLASHER_TRAP_DELAY_MS) == 1);

    /* Surrounding traffic must not hide it, and a wrong byte must not arm it. */
    check("noise before the frame", FlasherTrap_Feed(noise, (uint16_t)sizeof(noise)) == 0);
    check("frame after noise arms", FlasherTrap_Feed(frame, 16u) == 1);
    check("cleared", FlasherTrap_Due(fakeMs + FLASHER_TRAP_DELAY_MS) == 1);

    check("a wrong byte does not arm", FlasherTrap_Feed(bad, 16u) == 0);
    check("nothing due after a near miss", FlasherTrap_Due(fakeMs + FLASHER_TRAP_DELAY_MS) == 0);

    /* A short feed cannot match. */
    check("15 bytes do not arm", FlasherTrap_Feed(frame, 15u) == 0);
    check("no null deref", FlasherTrap_Feed(0, 16u) == 0);

    /* The reply: the request's command and sequence, the parameter where GCFFlasher reads it (byte 7 of
     * the unframed packet), and a checksum the flasher's own rule accepts - the 16-bit sum of every
     * byte up to the checksum, two's complement, low byte first. */
    {
        const uint8_t *ack = FlasherTrap_Ack();
        uint16_t sum = 0u;
        uint16_t crc;

        check("ack is framed", ack[0] == 0xC0u && ack[FLASHER_TRAP_ACK_LEN - 1u] == 0xC0u);
        check("ack command and sequence", ack[1] == frame[1] && ack[2] == frame[2]);
        check("ack status 0", ack[3] == 0x00u);
        check("ack frame length", ack[4] == (uint8_t)(FLASHER_TRAP_ACK_LEN - 4u) && ack[5] == 0x00u);
        check("ack parameter at byte 7", ack[8] == frame[8]);

        for (i = 1u; i < FLASHER_TRAP_ACK_LEN - 3u; ++i)
            sum = (uint16_t)(sum + ack[i]);

        crc = (uint16_t)(~sum + 1u);
        check("ack checksum", ack[FLASHER_TRAP_ACK_LEN - 3u] == (uint8_t)crc &&
                                  ack[FLASHER_TRAP_ACK_LEN - 2u] == (uint8_t)(crc >> 8));

        for (i = 1u; i < FLASHER_TRAP_ACK_LEN - 1u; ++i)
            check("ack needs no escape", ack[i] != 0xC0u && ack[i] != 0xDBu);
    }

    printf("flasher_trap: %s (%d failure(s))\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
