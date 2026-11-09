/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* fake_board.c — the core instructions, the tick, the board and its identity for a host suite
 * (fakes.h). */
#include "fakes.h"

#include "board.h"
#include "hal.h"

uint32_t fakeNowMs;
int fakeIrqDepth;
int fakeIrqUnbalanced;

void HAL_Wfi(void)
{
}

void HAL_Dsb(void)
{
}

void HAL_IrqLock(void)
{
    fakeIrqDepth++;
}

void HAL_IrqUnlock(void)
{
    if (fakeIrqDepth == 0)
        fakeIrqUnbalanced = 1;
    else
        fakeIrqDepth--;
}

int HAL_InIrqContext(void)
{
    return 0;
}

void Tick_GetMs(uint32_t *ms)
{
    *ms = fakeNowMs;
}

int Nvram_FactoryEui64(uint8_t *eui8)
{
    static const uint8_t eui[8] = {0x00u, 0x21u, 0x2Eu, 0xFFu, 0xFFu, 0x00u, 0x00u, 0x01u};
    uint8_t i;

    for (i = 0u; i < 8u; ++i)
        eui8[i] = eui[i];

    return 1;
}

static const Board_Descriptor fakeBoard = {
    .product = "host",
    .ncpVersion = "DSPINEL/1.0; HOST; phoscon-rcp test",
    .rxSensitivityDbm = -100,
    .txPowerMaxDbm = 10,
    .txPowerMaxCh26Dbm = 10,
    .linkBaud = 115200u,
};

const Board_Descriptor *Board_Get(void)
{
    return &fakeBoard;
}
