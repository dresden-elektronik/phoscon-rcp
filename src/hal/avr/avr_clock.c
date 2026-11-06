/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* avr_clock.c — the ConBee I / RaspBee I clock, the board's millisecond, and the cycle stopwatch.
 *
 * Glue (docs/coding-rules.md §4): register writes in the order the part wants them (datasheet, system
 * clock chapter for CLKPR, Timer/Counter0 and Timer/Counter1 chapters for the rest).
 *
 *   - The clock: the bootloader leaves CLKPR at divide-by-2, 8 MHz, and this image clears it to the
 *     crystal's 16 MHz. CLKPR takes a new value only within four cycles of the change enable, and the
 *     two stores are plain C; the read-back, not the hope, says which clock the board runs on.
 *   - The millisecond: Timer/Counter0 in CTC mode at clk/64, its compare value chosen so a period is
 *     exactly one millisecond (16 MHz / 64 / 250). During bring-up, with interrupts masked, the
 *     compare flag is polled and credited; once HAL_Init unlocks, the vector credits it.
 *   - The stopwatch: Timer/Counter1 free-running at the system clock, read as 16 bits. It wraps every
 *     4.096 ms at 16 MHz, so a stamp is comparable with a later one inside a millisecond tick, which
 *     is the hal/hal.h contract. */
#include <stdint.h>

#include "avr_clock.h"
#include "avr_clock_us.h"
#include "avr_regs.h"
#include "avr_wdt.h"
#include "dbg.h"
#include "hal.h"

#define TICK_PRESCALE 64u
#define TICK_CS (AVR_BIT(AVR_CS01) | AVR_BIT(AVR_CS00)) /* clk/64 */

/* A bounded wait for one compare period: a millisecond is 16 000 cycles, and a poll is a few. */
#define CLOCK_WAIT_LIMIT 0x8000u

static uint32_t clockCpuHz;
static int clockRegime;

static volatile uint32_t tickMs;
static int tickRunning;
static int tickCreditedByPump;

/* ---- the system clock ---- */

/* CLKPR as the datasheet's change procedure has it: the enable alone, then the value with the enable
 * clear, inside four cycles. Both are constants, so -Os emits two back-to-back stores. */
static uint8_t clockClearPrescaler(void)
{
    uint8_t sreg = AvrOwn_LockSave();

    AVR_REG8(AVR_CLKPR) = AVR_BIT(AVR_CLKPCE);
    AVR_REG8(AVR_CLKPR) = 0u;
    AvrOwn_LockRestore(sreg);

    return (uint8_t)(AVR_REG8(AVR_CLKPR) & 0x0Fu);
}

/* ---- the millisecond ---- */

static void clockCreditPeriod(void)
{
    tickMs++;
}

static int clockArmTick(void)
{
    uint32_t perMs = clockCpuHz / TICK_PRESCALE / 1000u;
    uint32_t n;

    /* An 8-bit compare: 250 at 16 MHz, 125 at 8 MHz. A rate whose millisecond is not a whole number
     * of clk/64 periods, or does not fit, gets no tick rather than a wrong one. */
    if (perMs == 0u || perMs > 256u || perMs * TICK_PRESCALE * 1000u != clockCpuHz)
    {
        DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_TICK_DEAD);
        return 0;
    }

    AVR_REG8(AVR_TCCR0B) = 0u;
    AVR_REG8(AVR_TIMSK0) = 0u;
    AVR_REG8(AVR_TCCR0A) = AVR_BIT(AVR_WGM01);
    AVR_REG8(AVR_TCNT0) = 0u;
    AVR_REG8(AVR_OCR0A) = (uint8_t)(perMs - 1u);
    AVR_REG8(AVR_TIFR0) = AVR_BIT(AVR_OCF0A); /* write-1-to-clear */
    AVR_REG8(AVR_TCCR0B) = TICK_CS;

    tickCreditedByPump = 1;
    tickRunning = 1;

    for (n = 0u; n < CLOCK_WAIT_LIMIT; ++n)
    {
        if ((AVR_REG8(AVR_TIFR0) & AVR_BIT(AVR_OCF0A)) != 0u)
        {
            AVR_REG8(AVR_TIFR0) = AVR_BIT(AVR_OCF0A);
            clockCreditPeriod();
            DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_TICK_CORE);
            return 1;
        }

        Wdt_FeedPoll(n);
    }

    AVR_REG8(AVR_TCCR0B) = 0u;
    tickRunning = 0;
    tickCreditedByPump = 0;
    DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_TICK_DEAD);
    return 0;
}

AVR_ISR(AVR_TIMER0_COMPA_vect_num)
{
    HAL_IrqEnter();
    clockCreditPeriod();
    HAL_IrqExit();
}

void Clock_BringupPump(void)
{
    if (!tickRunning || !tickCreditedByPump)
        return;

    if ((AVR_REG8(AVR_TIFR0) & AVR_BIT(AVR_OCF0A)) != 0u)
    {
        AVR_REG8(AVR_TIFR0) = AVR_BIT(AVR_OCF0A);
        clockCreditPeriod();
    }
}

/* The flag is cleared before the vector is enabled, so a period the pump already credited is not
 * counted twice. */
void Clock_TickCountedByIrq(void)
{
    if (!tickRunning)
        return;

    tickCreditedByPump = 0;
    AVR_REG8(AVR_TIFR0) = AVR_BIT(AVR_OCF0A);
    AVR_REG8(AVR_TIMSK0) = AVR_BIT(AVR_OCIE0A);
}

/* Four bytes the vector writes: read with the vector held off (docs/architecture.md §7). */
void Tick_GetMs(uint32_t *ms)
{
    uint8_t sreg = AvrOwn_LockSave();

    *ms = tickMs;
    AvrOwn_LockRestore(sreg);
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

    for (;;)
    {
        Tick_GetMs(&now);

        if ((int32_t)(deadline - now) <= 0)
            break;

        Clock_BringupPump();
        Wdt_FeedPoll(round++);
    }
}

/* ---- the cycle stopwatch ---- */

/* TCNT1 is read low byte first, which latches the high byte into the timer's TEMP register; another
 * 16-bit access to Timer/Counter1 between the two reads would overwrite TEMP, so the pair is read with
 * interrupts held off. */
uint32_t Tick_CycleStamp(void)
{
    uint8_t sreg = AvrOwn_LockSave();
    uint8_t lo = AVR_REG8(AVR_TCNT1L);
    uint8_t hi = AVR_REG8(AVR_TCNT1H);

    AvrOwn_LockRestore(sreg);
    return (uint32_t)((uint16_t)((uint16_t)hi << 8 | lo));
}

uint32_t Tick_CyclesSince(uint32_t stamp)
{
    return (uint32_t)(uint16_t)(Tick_CycleStamp() - stamp);
}

static void clockStartStopwatch(void)
{
    AVR_REG8(AVR_TCCR1B) = 0u;
    AVR_REG8(AVR_TCCR1A) = 0u;
    AVR_REG8(AVR_TCNT1H) = 0u; /* high byte first: it goes to TEMP and lands with the low byte */
    AVR_REG8(AVR_TCNT1L) = 0u;
    AVR_REG8(AVR_TCCR1B) = AVR_BIT(AVR_CS10);
}

#ifdef RCP_TX_TIMING
/* ---- the microsecond clock ---- */

/* The stopwatch's wraps, counted by the vector. Timer/Counter1 keeps running as the stopwatch it is. */
static volatile uint32_t clockWraps;

AVR_ISR(AVR_TIMER1_OVF_vect_num)
{
    HAL_IrqEnter();
    clockWraps++;
    HAL_IrqExit();
}

void Tick_ClockStart(void)
{
    AVR_REG8(AVR_TIFR1) = AVR_BIT(AVR_TOV1); /* write-1-to-clear */
    AVR_REG8(AVR_TIMSK1) = AVR_BIT(AVR_TOIE1);
}

uint32_t Tick_ClockUs(void)
{
    uint8_t sreg = AvrOwn_LockSave();
    uint8_t lo = AVR_REG8(AVR_TCNT1L);
    uint8_t hi = AVR_REG8(AVR_TCNT1H);
    int pending = (AVR_REG8(AVR_TIFR1) & AVR_BIT(AVR_TOV1)) != 0u;
    uint32_t wraps = clockWraps;

    AvrOwn_LockRestore(sreg);
    return AvrClock_Us(wraps, (uint16_t)((uint16_t)hi << 8 | lo), pending);
}
#endif

/* ---- bring-up ---- */

int Clock_Init(void)
{
    uint8_t presc;

    /* The timers are clocked only while their power-reduction bits are clear; the bootloader runs
     * Timer/Counter3 and leaves these as reset left them, which is clear. Cleared anyway, so this image
     * owes nothing to who ran before it. */
    AVR_REG8(AVR_PRR0) = (uint8_t)(AVR_REG8(AVR_PRR0) &
                                   (uint8_t)~(AVR_BIT(AVR_PRTIM0) | AVR_BIT(AVR_PRTIM1)));

    presc = clockClearPrescaler();
    clockCpuHz = AVR_CLOCK_XTAL_HZ >> presc;
    clockRegime = (presc == 0u) ? AVR_CLOCK_FULL : AVR_CLOCK_DIVIDED;
    Debug_Emit(DBG_COMP_CLOCK, presc, DBG_CLOCK_RSN_AVR_PRESCALER);

    clockStartStopwatch();

    if (!clockArmTick())
        return AVR_CLOCK_DOWN;

    Debug_Emit(DBG_COMP_CLOCK, (uint16_t)clockRegime, DBG_CLOCK_RSN_DONE);
    return clockRegime;
}

uint32_t Clock_CpuHz(void)
{
    return clockCpuHz;
}

void Clock_WitnessReport(void)
{
    Debug_Emit(DBG_COMP_WITNESS, AVR_REG8(AVR_CLKPR), DBG_WITNESS_RSN_AVR_CLKPR);
    Debug_Emit(DBG_COMP_WITNESS, AVR_REG8(AVR_MCUCR), DBG_WITNESS_RSN_AVR_MCUCR);
    Debug_Emit(DBG_COMP_WITNESS, AVR_REG8(AVR_SMCR), DBG_WITNESS_RSN_AVR_SMCR);
    Debug_Emit(DBG_COMP_WITNESS, AVR_REG8(AVR_WDTCSR), DBG_WITNESS_RSN_AVR_WDTCSR);
    Debug_Emit(DBG_COMP_WITNESS, AVR_REG8(AVR_TCCR0A), DBG_WITNESS_RSN_AVR_TCCR0A);
    Debug_Emit(DBG_COMP_WITNESS, AVR_REG8(AVR_TCCR0B), DBG_WITNESS_RSN_AVR_TCCR0B);
    Debug_Emit(DBG_COMP_WITNESS, AVR_REG8(AVR_OCR0A), DBG_WITNESS_RSN_AVR_OCR0A);
    Debug_Emit(DBG_COMP_WITNESS, AVR_REG8(AVR_TIMSK0), DBG_WITNESS_RSN_AVR_TIMSK0);
}
