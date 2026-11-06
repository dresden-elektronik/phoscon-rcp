/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mg21_mon.c — the ConBee III side of the bench register monitor (hal/mon_drv.h).

Glue only (docs/coding-rules.md §4): the region table is this part's address map as the monitor may
use it, and the accesses are plain volatile word reads and writes. The policy that applies the table
is app/mon_proto.c.

The table keeps every write away from what can outlive a reset. The flash controller (MSC) can
erase and program flash, including the bootloader's sector; the Secure Engine mailbox takes commands
that can lock debug access or erase; the SMU decides which master may reach which peripheral. Each
is listed read-only before the writable window that contains it, in both its secure and non-secure
alias and across its SET/CLR/TGL windows, which sit at +0x1000..+0x3FFF of every block. Flash, the
user data page, DEVINFO and the SRAM this image runs from are readable and never writable. The radio
subsystem (SEQRAM, FRCRAM and the radio blocks at 0xA0000000..0xBFFFFFFF), the other peripherals and
the core's private peripheral bus are writable: a wrong write there is undone by a reset, and a fault
it causes resets into the bootloader window (hal/mg21/hal_mg21.c). */
#include <stdint.h>

#include "hal.h"
#include "mg21_clock.h"
#include "mon_drv.h"

#define MG21_MON_BLOCK_WINDOWS 0x4000u /* a block and its SET, CLR and TGL aliases */

static const Mon_Region mg21MonRegions[] = {
    /* read-only holes first: first match wins */
    {0x40030000u, MG21_MON_BLOCK_WINDOWS, 0}, /* MSC */
    {0x50030000u, MG21_MON_BLOCK_WINDOWS, 0}, /* MSC, non-secure alias */
    {0x44008000u, MG21_MON_BLOCK_WINDOWS, 0}, /* SMU */
    {0x54008000u, MG21_MON_BLOCK_WINDOWS, 0}, /* SMU, non-secure alias */
    {0x4C000000u, 0x01000000u, 0},            /* SEMAILBOX_HOST and its window */
    {0x5C000000u, 0x01000000u, 0},            /* the same, non-secure alias */
    /* memories, read-only */
    {0x00000000u, 0x00100000u, 0}, /* main flash, bootloader included */
    {0x0FE00000u, 0x00000400u, 0}, /* user data page */
    {0x0FE08000u, 0x00000400u, 0}, /* DEVINFO */
    {0x20000000u, 0x00018000u, 0}, /* SRAM: this image's own */
    /* writable windows */
    {0x40000000u, 0x20000000u, 1}, /* peripherals, both aliases */
    {0xA0000000u, 0x20000000u, 1}, /* radio subsystem: SEQRAM, FRCRAM, radio blocks */
    {0xE0000000u, 0x00100000u, 1}, /* private peripheral bus */
};

const Mon_Region *MonDrv_Regions(uint16_t *count)
{
    *count = (uint16_t)(sizeof(mg21MonRegions) / sizeof(mg21MonRegions[0]));
    return mg21MonRegions;
}

uint32_t MonDrv_Read32(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

void MonDrv_Write32(uint32_t addr, uint32_t value)
{
    *(volatile uint32_t *)addr = value;
}

/* Interrupts are masked for the whole trace, so the sampling loop is the only thing running and its
 * cycle stamps are not stretched by a vector. The budget (MON_TRACE_CYCLES_MAX, half a second) stays
 * well inside the 2 s watchdog, and the millisecond tick loses that much time. Without a cycle counter
 * the budget is counted in loop rounds instead, each charged as 16 cycles - more than one round of
 * this loop takes - so the budget still ends inside the watchdog period, and the samples say so
 * through cycles == 0. */
uint8_t MonDrv_Trace(const Mon_Cmd *cmd, Mon_Sample *out, int *timedOut)
{
    volatile uint32_t *traced = (volatile uint32_t *)cmd->traceAddr;
    uint32_t start;
    uint32_t elapsed;
    uint32_t last;
    uint32_t v;
    uint32_t rounds = 0u;
    uint8_t n = 0u;
    int cycles = Clock_CyclesRunning();

    *timedOut = 0;

    HAL_IrqLock();

    start = Tick_CycleStamp();
    *(volatile uint32_t *)cmd->addr = cmd->values[0];

    last = *traced & cmd->traceMask;
    out[n].value = last;
    out[n].cycles = cycles ? Tick_CyclesSince(start) : 0u;
    n++;

    while (n < cmd->count)
    {
        v = *traced & cmd->traceMask;
        elapsed = cycles ? Tick_CyclesSince(start) : (rounds++ << 4);

        if (v != last)
        {
            last = v;
            out[n].value = v;
            out[n].cycles = cycles ? elapsed : 0u;
            n++;
        }
        else if (elapsed >= cmd->traceCycles)
        {
            *timedOut = 1;
            break;
        }
    }

    HAL_IrqUnlock();
    return n;
}
