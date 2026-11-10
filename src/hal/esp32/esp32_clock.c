/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* esp32_clock.c — the millisecond and the stopwatch on SYSTIMER (TRM ch. 13).

Unit 0 counts at 16 MHz from the crystal (XTAL_CLK / 2.5 on the C6), independent of the CPU clock the
bootloader left, and comparator 0 in period mode raises an interrupt every 16 000 counts: the millisecond.
The period alarm is the hardware's, so the tick does not drift with the latency of its vector; the vector
only acknowledges and counts. Until interrupts open, Clock_BringupPump credits the same alarm from its raw
flag, so a bounded wait during bring-up still sees time pass.

A count is read by asking the unit for a snapshot and waiting for it to be valid (TRM 13.5.1). The
stopwatch reads only the snapshot's low word, so a second snapshot taken by an interrupt between the
request and the read only makes the stamp later, never inconsistent. */
#include <stdint.h>

#include "dbg.h"
#include "esp32_clock.h"
#include "esp32_irq.h"
#include "esp32_regs.h"
#include "esp32_wdt.h"
#include "hal.h"

#define SYSTIMER(offset) ESP32_REG(ESP32_SYSTIMER_BASE, (offset))

#define CLOCK_TICKS_PER_MS (ESP32_SYSTIMER_TICKS_PER_US * 1000u)
/* A snapshot is valid within a few APB cycles; a counter that never answers is a dead block. */
#define CLOCK_SNAPSHOT_SPINS 1000u
#define CLOCK_TICK_PRIORITY 1u

static volatile uint32_t tickMs;
static int tickRunning;
static int tickVectored;

static void clockTickIsr(void);

__attribute__((section(".iram.text.esp32_clock"))) static void clockTickIsr(void)
{
    SYSTIMER(ESP32_SYSTIMER_INT_CLR) = ESP32_SYSTIMER_INT_TARGET0;
    tickMs++;
}

int Clock_Init(void)
{
    uint32_t conf;

    conf = SYSTIMER(ESP32_SYSTIMER_CONF);
    conf &= ~ESP32_SYSTIMER_CONF_TARGET0_WORK_EN;
    conf |= ESP32_SYSTIMER_CONF_UNIT0_WORK_EN;
    SYSTIMER(ESP32_SYSTIMER_CONF) = conf;

    /* The period-mode sequence of TRM 13.5.3: unit 0, the period, load it, period mode cleared and set,
     * then the comparator and its interrupt. */
    SYSTIMER(ESP32_SYSTIMER_TARGET0_CONF) = CLOCK_TICKS_PER_MS & ESP32_SYSTIMER_TARGET0_CONF_PERIOD_MASK;
    SYSTIMER(ESP32_SYSTIMER_COMP0_LOAD) = ESP32_SYSTIMER_COMP0_LOAD_GO;
    SYSTIMER(ESP32_SYSTIMER_TARGET0_CONF) = (CLOCK_TICKS_PER_MS & ESP32_SYSTIMER_TARGET0_CONF_PERIOD_MASK)
                                            | ESP32_SYSTIMER_TARGET0_CONF_PERIOD_MODE;
    SYSTIMER(ESP32_SYSTIMER_CONF) = conf | ESP32_SYSTIMER_CONF_TARGET0_WORK_EN;
    SYSTIMER(ESP32_SYSTIMER_INT_CLR) = ESP32_SYSTIMER_INT_TARGET0;
    SYSTIMER(ESP32_SYSTIMER_INT_ENA) |= ESP32_SYSTIMER_INT_TARGET0;

    if (!Esp32Irq_Route(ESP32_SRC_SYSTIMER_TARGET0, ESP32_LINE_TICK, CLOCK_TICK_PRIORITY, clockTickIsr))
        return 0;

    tickRunning = 1;
    return 1;
}

void Clock_TickCountedByIrq(int on)
{
    tickVectored = (on != 0) ? 1 : 0;
}

void Clock_BringupPump(void)
{
    if (!tickRunning || tickVectored)
        return;

    if ((SYSTIMER(ESP32_SYSTIMER_INT_ST) & ESP32_SYSTIMER_INT_TARGET0) != 0u)
    {
        SYSTIMER(ESP32_SYSTIMER_INT_CLR) = ESP32_SYSTIMER_INT_TARGET0;
        tickMs++;
    }
}

void Tick_GetMs(uint32_t *ms)
{
    *ms = tickMs;
}

void Delay_Ms(uint32_t ms)
{
    uint32_t now;
    uint32_t deadline;
    uint32_t round;

    if (ms == 0u || !tickRunning)
        return;

    Tick_GetMs(&now);
    deadline = now + ms;
    round = 0u;

    while ((int32_t)(deadline - tickMs) > 0)
    {
        Clock_BringupPump();
        Wdt_FeedPoll(round++);
    }
}

/* 16 MHz counts, the low word of unit 0; 0 when the snapshot never became valid. */
uint32_t Tick_CycleStamp(void)
{
    uint32_t spins;

    SYSTIMER(ESP32_SYSTIMER_UNIT0_OP) = ESP32_SYSTIMER_UNIT0_OP_UPDATE;

    for (spins = 0u; spins < CLOCK_SNAPSHOT_SPINS; ++spins)
    {
        if ((SYSTIMER(ESP32_SYSTIMER_UNIT0_OP) & ESP32_SYSTIMER_UNIT0_OP_VALUE_VALID) != 0u)
            return SYSTIMER(ESP32_SYSTIMER_UNIT0_VALUE_LO);
    }

    return 0u;
}

uint32_t Tick_CyclesSince(uint32_t stamp)
{
    return Tick_CycleStamp() - stamp;
}

void Clock_WitnessReport(void)
{
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)(SYSTIMER(ESP32_SYSTIMER_CONF) >> 16),
               DBG_WITNESS_RSN_ESP32_SYSTIMER_CONF);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)ESP32_LINE_TICK, DBG_WITNESS_RSN_ESP32_TICK_LINE);
    Debug_Emit(DBG_COMP_CLOCK, (uint16_t)tickRunning, DBG_CLOCK_RSN_DONE);
}
