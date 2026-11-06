/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* rail_shims.c — the symbols Silicon Labs' RAIL archive expects its host application to supply.
 *
 * The archive (librail_efr32xg21_gcc_release.a, fetched by tools/fetch_rail.py) is not self-contained: it
 * calls into the SDK's emlib, core-critical-section and string code. A link with nothing behind it names
 * every one (docs/hardware_mg21.md §10.6 lists them); each is answered here, from scratch and at the
 * smallest size that keeps the call truthful. The signatures are RAIL's call sites, not a copy of the
 * SDK's declarations. No vendor header is included: the archive links by symbol name. */
#include <stdint.h>

#include "hal.h"
#include "mg21_clock.h"
#include "mg21_rail_pa.h"
#include "mg21_rail_shims.h"
#include "mg21_regs.h"

/* ---- critical sections: CORE_EnterAtomic / CORE_EnterCritical and their exits ----
 * RAIL brackets its shared state with these and hands the returned token back at the exit. The core
 * mask cannot be read back from here (hal/hal.h has the four instructions and no more), so the token is
 * the nesting depth: the outermost exit, and only it, unmasks. The radio back-end is entered from the
 * main loop with interrupts enabled and from RAIL's own interrupts, which also run enabled, so the
 * outermost entry always starts unmasked. */
static uint32_t coreDepth;
static uint32_t coreStayMasked;

uint32_t CORE_EnterAtomic(void)
{
    uint32_t depth;

    HAL_IrqLock();
    depth = coreDepth;
    coreDepth = depth + 1u;
    return depth;
}

void CORE_ExitAtomic(uint32_t depth)
{
    coreDepth = depth;

    if (depth == 0u && coreStayMasked == 0u)
        HAL_IrqUnlock();
}

uint32_t CORE_EnterCritical(void)
{
    return CORE_EnterAtomic();
}

void CORE_ExitCritical(uint32_t depth)
{
    CORE_ExitAtomic(depth);
}

void Mg21RailShim_StayMasked(void)
{
    coreStayMasked = 1u;
}

/* ---- string functions ----
 * rail.h's inline RAIL_CopyRxPacket and the archive's own structure copies. Byte loops; the attribute
 * keeps the compiler from turning the loop back into a call to the function it is defining. */
#define NO_LOOP_PATTERNS __attribute__((optimize("no-tree-loop-distribute-patterns")))

NO_LOOP_PATTERNS void *memcpy(void *dst, const void *src, unsigned int n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    while (n-- != 0u)
        *d++ = *s++;

    return dst;
}

NO_LOOP_PATTERNS void *memset(void *dst, int c, unsigned int n)
{
    uint8_t *d = (uint8_t *)dst;

    while (n-- != 0u)
        *d++ = (uint8_t)c;

    return dst;
}

NO_LOOP_PATTERNS int memcmp(const void *a, const void *b, unsigned int n)
{
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;

    while (n-- != 0u)
    {
        if (*x != *y)
            return (int)*x - (int)*y;

        ++x;
        ++y;
    }

    return 0;
}

/* ---- clocks and device identity ---- */

/* The crystal's frequency, which RAIL checks against the PHY it loads. The adapter refuses to start
 * the radio unless the clock bring-up ended on the HFXO (mg21_rail.c), so the answer is the board's. */
uint32_t SystemHFXOClockGet(void)
{
    return 38400000u;
}

/* The processor clock the bring-up produced (mg21_clock.c). */
uint32_t SystemSYSCLKGet(void)
{
    return Clock_SysclkHz();
}

/* The silicon revision, which RAIL reads to choose between errata paths in its PA and radio set-up.
 * RAIL's call sites read byte 0 as the minor and byte 1 as the major revision of a structure of at most
 * eight bytes; SYSCFG.CHIPREV carries them (MAJOR in bits 5:0, MINOR in bits 19:12) and the hardwired
 * CHIPREVHW is the answer when the programmed word is still zero. */
void SYSTEM_ChipRevisionGet(uint8_t *rev)
{
    uint32_t word = MG21_SYSCFG->CHIPREV;
    uint32_t i;

    if (word == 0u)
        word = MG21_SYSCFG->CHIPREVHW;

    for (i = 0u; i < 8u; ++i)
        rev[i] = 0u;

    rev[0] = (uint8_t)((word >> 12) & 0xFFu);
    rev[1] = (uint8_t)(word & 0x3Fu);
}

/* ---- GPIO ----
 * RAIL routes the packet trace interface and the antenna-select pins through the SDK's GPIO service.
 * This back-end configures neither (PTI disabled, no antenna pins: mg21_rail.c), so no call can arrive;
 * the answer is the SDK's "not supported" status, and no pin is touched. */
#define SHIM_STATUS_NOT_SUPPORTED 0x000Fu

uint32_t sl_gpio_set_pin_mode(const void *gpio, uint32_t mode, int value)
{
    (void)gpio;
    (void)mode;
    (void)value;
    return SHIM_STATUS_NOT_SUPPORTED;
}

uint32_t sl_gpio_set_pin(const void *gpio)
{
    (void)gpio;
    return SHIM_STATUS_NOT_SUPPORTED;
}

uint32_t sl_gpio_clear_pin(const void *gpio)
{
    (void)gpio;
    return SHIM_STATUS_NOT_SUPPORTED;
}

/* ---- transmit power: dBm to raw level ----
 * RAIL calls this while it configures the PA and when a channel limit is applied. A power at or above
 * the "no limit" value of 0x7FFF deci-dBm converts to 255, the level no PA has, so that a comparison with
 * the PA's level never asks for a reduction (the SDK's plugin does the same). Everything else goes
 * through the curve of the amplifier the mode names (mg21_rail_pa.c); a mode that has no curve converts to
 * level 0, the lowest power, never a higher one. */
#define SHIM_POWER_NO_LIMIT 0x7FFF

uint8_t RAIL_ConvertDbmToRaw(void *handle, uint32_t mode, int16_t deciDbm)
{
    (void)handle;

    if (deciDbm >= SHIM_POWER_NO_LIMIT)
        return 255u;

    return Mg21RailPa_Level((int)mode, deciDbm);
}

/* ---- the peripheral reflex system: PRS_GetFreeChannel, PRS_SourceAsyncSignalSet, PRS_ConnectConsumer ----
 * A scheduled transmission starts from the protocol timer through a PRS channel, so these write the
 * real registers (EFR32xG21 reference manual, PRS). RAIL names an asynchronous channel (type 0), hands
 * the source already in its SOURCESEL position, and names a consumer by its register's offset. */
#define PRS_ASYNC_CH_CTRL(ch) (*(volatile uint32_t *)(MG21_PRS_BASE + 0x18u + 4u * (uint32_t)(ch)))
#define PRS_ASYNC_CH_COUNT 12u
#define PRS_CTRL_SOURCESEL_MASK 0x7F00u
#define PRS_CTRL_SIGSEL_MASK 0x0007u
#define PRS_CONSUMER_PRSSEL_MASK 0x000Fu

int32_t PRS_GetFreeChannel(uint32_t type)
{
    uint32_t ch;

    (void)type;

    for (ch = 0u; ch < PRS_ASYNC_CH_COUNT; ++ch)
    {
        if ((PRS_ASYNC_CH_CTRL(ch) & PRS_CTRL_SOURCESEL_MASK) == 0u)
            return (int32_t)ch;
    }

    return -1;
}

void PRS_SourceAsyncSignalSet(uint32_t ch, uint32_t source, uint32_t signal)
{
    PRS_ASYNC_CH_CTRL(ch) = (PRS_ASYNC_CH_CTRL(ch) & ~(uint32_t)(PRS_CTRL_SOURCESEL_MASK | PRS_CTRL_SIGSEL_MASK)) |
                            (source & PRS_CTRL_SOURCESEL_MASK) | (signal & PRS_CTRL_SIGSEL_MASK);
}

void PRS_ConnectConsumer(uint32_t ch, uint32_t type, uint32_t consumer)
{
    volatile uint32_t *reg = (volatile uint32_t *)(MG21_PRS_BASE + consumer);

    (void)type;
    *reg = (*reg & ~(uint32_t)PRS_CONSUMER_PRSSEL_MASK) | (ch & PRS_CONSUMER_PRSSEL_MASK);
}
