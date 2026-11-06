/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mg21_clock.c — ConBee III clock bring-up, the board's millisecond, and the cycle stopwatch.

Glue (docs/coding-rules.md §4): register writes in the order the part wants them, and the arithmetic
that decides those values lives in tick_plan.c where the host suite can pin it. Every number the rest
of the image depends on — SysTick's reload, the USART's divisor — is produced from the clock this
bring-up actually ended in, not from the one the board is supposed to have.

Three things are different from the SAMR21 side, and they are why this file is a separate
implementation rather than a translation:

  - The part comes up on FSRCO at 20 MHz, already running: no 8 MHz idle state to climb out of and no
    DFLL to lock (reference manual 8.4, 9.2, 9.7). HFXO is the upgrade, not the requirement.
  - The crystal's trimming is the board's, in the manufacturing token (docs/hardware_mg21.md §10.1),
    and nothing has applied it when this image starts: the bootloader jumps here from its
    SystemInit2, straight after a reset and before any oscillator of its own is configured, so HFXO is
    at its reset XTALCFG/XTALCTRL (CTUNE 0x8C). That is close enough for the USART; the radio will
    need the token's CTUNE. Nothing here writes XTALCFG: it holds the oscillator on and waits for
    RDY.
  - SysTick counts cycles, and a millisecond of this board's core clock is 38 400 of them, which fits
    the 24-bit reload exactly. LFRCO is the opposite case: 32.768 kHz gives 32.768 cycles per
    millisecond, so it is not a millisecond source at all and the shortest whole period it has is
    125 ms. The tick here is the core clock's, and the exactness rule is arithmetic in tick_plan.h
    under its own suite rather than a claim in a comment. */
#include <stdint.h>

#include "dbg.h"
#include "hal.h"
#include "mg21_clock.h"
#include "mg21_regs.h"
#include "mg21_wdt.h"
#include "tick_plan.h"

/* A bounded bring-up wait: 262144 polls - a few tenths of a second at the 20 MHz the part comes up
 * on - far above the microsecond-scale start-up the manual describes, and still far below the 8 s
 * the bootloader armed. */
#define CLOCK_WAIT_LIMIT 0x40000u

/* The panic hold: about a tenth of a second, long enough that the reset which follows is this
 * image's decision rather than an unexplained timeout. */
#define CLOCK_FAULT_HOLD_CYCLES 4000000u
#define CLOCK_FAULT_HOLD_ROUNDS 250000u

/* The core's own registers. CMSIS names them through struct pointers; this tree reaches the same
 * addresses by the names mg21_regs.h holds, which the mg21_regs suite checks against CMSIS. */
#define CORE(addr) (*(volatile uint32_t *)(addr))

typedef enum
{
    CLOCK_STEP_CMU = 0, /* open the CMU's protected writes */
    CLOCK_STEP_HFXO,    /* hold the crystal on and wait for it to say it is ready */
    CLOCK_STEP_GROUPS,  /* name the system clock and the two peripheral group clocks */
    CLOCK_STEP_SYSTICK, /* arm the millisecond and prove one period passed */
    CLOCK_STEP_CYCLES,  /* prove or disprove the core's cycle counter */
    CLOCK_STEP_DONE
} Clock_Step;

static volatile Clock_Step clockStep;
static int clockRegime;
static uint32_t clockSysclkHz;
static uint32_t clockLinkHz;

/* The millisecond. Bring-up runs with interrupts masked, so during it a period is credited by
 * reading the counter; afterwards the SysTick vector credits it. */
static Tick_Plan tickPlan;
static uint32_t tickCtrl;
static volatile uint32_t tickMs;
static int tickRunning;
static int tickCreditedByPump;

static int cyclesRunning;

static void clockCreditPeriod(void);

/* ---- steps ---- */

static int clockOpenCmu(void)
{
    MG21_CMU->LOCK = MG21_CMU_LOCKKEY_UNLOCK;
    return 1;
}

/* Hold the oscillator on regardless of who asks (CTRL.FORCEEN) and do not switch it off when the
 * last request goes away (CTRL.DISONDEMAND, 9.2). RDY is the crystal's own statement that it is
 * running; ENS only says something asked, so waiting on the wrong one reports a crystal that was
 * never proved. */
static int clockBringHfxo(void)
{
    uint32_t n;

    MG21_HFXO0->LOCK = MG21_HFXO_LOCK_LOCKKEY_UNLOCK;
    MG21_HFXO0->CTRL |= MG21_HFXO_CTRL_FORCEEN | MG21_HFXO_CTRL_DISONDEMAND;

    for (n = 0; n < CLOCK_WAIT_LIMIT; ++n)
    {
        if ((MG21_HFXO0->STATUS & MG21_HFXO_STATUS_RDY) != 0u)
        {
            DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_HFXO_ON);
            return 1;
        }

        Clock_BringupPump();
        Wdt_FeedPoll(n);
    }

    DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_HFXO_NO_RDY);
    return 0;
}

/* Pick the system clock and read both bus prescalers back rather than assuming the bootloader left
 * them undivided. The USART is clocked from HCLK / (PCLKPRESC+1), and HCLK is SYSCLK / (HCLKPRESC+1)
 * - the same chain the vendor's CMU_ClockFreqGet(cmuClock_USART0) walks (SystemHCLKGet divided by the
 * PCLK divider), so the link reference is SYSCLK / ((HCLKPRESC+1) x (PCLKPRESC+1)). That is the rate
 * the USART divisor is computed against (hal/mg21/mg21_uart.c). Both prescalers are read, not assumed:
 * the bootloader leaves them undivided, but a neighbour that sets HCLKPRESC would otherwise leave the
 * divisor aimed at a clock the USART does not see. EM01GRPACLK is a different select, and the group
 * that comes out of reset naming HFRCODPLL - an oscillator this board does not run. The millisecond is
 * a separate premise: the tick (clockArmTick) counts the core clock, which equals clockSysclkHz only
 * while HCLKPRESC is 0 - the board's design point, which the SYSCLKCTRL witness makes visible.
 */
static int clockSelectClocks(int useHfxo)
{
    uint32_t ctrl;
    uint32_t sel = (useHfxo != 0) ? MG21_CMU_SYSCLKCTRL_CLKSEL_HFXO
                                  : MG21_CMU_SYSCLKCTRL_CLKSEL_FSRCO;

    MG21_CMU->SYSCLKCTRL = (MG21_CMU->SYSCLKCTRL & ~MG21_CMU_SYSCLKCTRL_CLKSEL_MASK) | sel;

    MG21_CMU->EM01GRPACLKCTRL = (useHfxo != 0) ? MG21_CMU_EM01GRPACLKCTRL_CLKSEL_HFXO
                                               : MG21_CMU_EM01GRPACLKCTRL_CLKSEL_FSRCO;
    MG21_CMU->EM23GRPACLKCTRL = MG21_CMU_EM23GRPACLKCTRL_CLKSEL_LFRCO;

    ctrl = MG21_CMU->SYSCLKCTRL;
    sel = ctrl & MG21_CMU_SYSCLKCTRL_CLKSEL_MASK;

    clockRegime = (sel == MG21_CMU_SYSCLKCTRL_CLKSEL_HFXO) ? MG21_CLOCK_HFXO : MG21_CLOCK_FSRCO;
    clockSysclkHz = (clockRegime == MG21_CLOCK_HFXO) ? MG21_BOARD_HFXO_HZ : MG21_FSRCO_HZ;
    {
        uint32_t hclkPresc = (ctrl & MG21_CMU_SYSCLKCTRL_HCLKPRESC_MASK)
                             >> MG21_CMU_SYSCLKCTRL_HCLKPRESC_SHIFT;
        uint32_t pclkPresc = (ctrl & MG21_CMU_SYSCLKCTRL_PCLKPRESC_MASK)
                             >> MG21_CMU_SYSCLKCTRL_PCLKPRESC_SHIFT;

        clockLinkHz = clockSysclkHz / MG21_CMU_PRESC_DIV(hclkPresc)
                                       / MG21_CMU_PRESC_DIV(pclkPresc);

        /* The millisecond assumes the core runs at SYSCLK, which holds only while HCLKPRESC is 0.
         * If a neighbour sets it, say so: the line field carries the value and the tick is wrong. */
        if (hclkPresc != 0u)
            Debug_Emit(DBG_COMP_CLOCK, (uint16_t)hclkPresc, DBG_CLOCK_RSN_HCLK_PRESC);
    }

    DBG(DBG_COMP_CLOCK,
        (clockRegime == MG21_CLOCK_HFXO) ? DBG_CLOCK_RSN_SYSCLK_HFXO : DBG_CLOCK_RSN_SYSCLK_FSRCO);
    DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_GROUP_CLK);
    return 1;
}

/* One SysTick period, observed without interrupts: reading CTRL takes COUNTFLAG out and the counter
 * sets it again at every wrap. The loop credits what it sees, so a bring-up that took three
 * milliseconds is reported as three milliseconds rather than as none. */
static int clockWaitTickPeriod(void)
{
    uint32_t n;

    (void)CORE(MG21_SYSTICK_CTRL);

    for (n = 0; n < CLOCK_WAIT_LIMIT; ++n)
    {
        if ((CORE(MG21_SYSTICK_CTRL) & MG21_SYSTICK_CTRL_COUNTFLAG) != 0u)
        {
            clockCreditPeriod();
            return 1;
        }

        Wdt_FeedPoll(n);
    }

    return 0;
}

static int clockArmTick(void)
{
    /* A millisecond first, and if a clock ever stops being a multiple of 1000, the shortest exact
     * whole-millisecond period it does have - said out loud in the witnesses rather than absorbed as
     * a rounding error. */
    if (!TickPlan_Period(clockSysclkHz, 1u, &tickPlan) &&
        !TickPlan_Shortest(clockSysclkHz, TICK_PLAN_MAX_MS, &tickPlan))
    {
        DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_TICK_DEAD);
        clockStep = CLOCK_STEP_DONE;
        return 0;
    }

    tickCtrl = MG21_SYSTICK_CTRL_ENABLE | MG21_SYSTICK_CTRL_CLKSOURCE;
    tickCreditedByPump = 1;
    CORE(MG21_SYSTICK_LOAD) = tickPlan.load;
    CORE(MG21_SYSTICK_CTRL) = tickCtrl;

    if (!clockWaitTickPeriod())
    {
        CORE(MG21_SYSTICK_CTRL) = 0u;
        tickCreditedByPump = 0;
        DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_TICK_DEAD);
        clockStep = CLOCK_STEP_DONE;
        return 0;
    }

    tickRunning = 1;
    DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_TICK_CORE);
    clockStep = CLOCK_STEP_CYCLES;
    return 1;
}

/* The cycle stopwatch a radio turnaround needs. DWT's counter exists only if the core implements it
 * and the trace block is enabled, so it is proved by watching it move rather than assumed from the
 * core's generation. */
static int clockProbeCycles(void)
{
    uint32_t before;
    uint32_t after;
    uint32_t n;

    CORE(MG21_SCB_DEMCR) |= MG21_DEMCR_TRCENA;
    CORE(MG21_DWT_CYCCNT) = 0u;
    CORE(MG21_DWT_CTRL) = MG21_DWT_CTRL_CYCCNTENA;

    before = CORE(MG21_DWT_CYCCNT);

    for (n = 0; n < 16u; ++n)
        after = CORE(MG21_DWT_CYCCNT);

    if (after != before)
        return 1;

    DBG(DBG_COMP_CLOCK, DBG_CLOCK_RSN_CYCLES_OFF);
    return 0;
}

/* ---- the machine ---- */

int Clock_Init(void)
{
    clockStep = CLOCK_STEP_CMU;

    while (clockStep != CLOCK_STEP_DONE)
    {
        switch (clockStep)
        {
            case CLOCK_STEP_CMU:
                (void)clockOpenCmu();
                clockStep = CLOCK_STEP_HFXO;
                break;

            case CLOCK_STEP_HFXO:
                (void)clockSelectClocks(clockBringHfxo());
                clockStep = CLOCK_STEP_GROUPS;
                break;

            case CLOCK_STEP_GROUPS:
                clockStep = CLOCK_STEP_SYSTICK;
                break;

            case CLOCK_STEP_SYSTICK:
                (void)clockArmTick();
                break;

            case CLOCK_STEP_CYCLES:
                cyclesRunning = clockProbeCycles();
                clockStep = CLOCK_STEP_DONE;
                break;

            default:
                clockStep = CLOCK_STEP_DONE;
                break;
        }
    }

    if (!tickRunning)
        return MG21_CLOCK_DOWN;

    Debug_Emit(DBG_COMP_CLOCK, (uint16_t)clockRegime, DBG_CLOCK_RSN_DONE);
    return clockRegime;
}

/* ---- the millisecond ---- */

static void clockCreditPeriod(void)
{
    tickMs += tickPlan.msPerPeriod;
}

void SysTick_Handler(void)
{
    HAL_IrqEnter();
    clockCreditPeriod();
    HAL_IrqExit();
}

/* During bring-up the pump is what keeps Tick_GetMs() moving through a long wait; once the vector
 * credits periods the pump stops, or every period counts twice. Writing CTRL reloads the counter,
 * which is the moment the tick switches from polled to vectored. */
void Clock_BringupPump(void)
{
    if (!tickRunning || !tickCreditedByPump)
        return;

    if ((CORE(MG21_SYSTICK_CTRL) & MG21_SYSTICK_CTRL_COUNTFLAG) != 0u)
        clockCreditPeriod();
}

void Clock_TickCountedByIrq(int on)
{
    if (!tickRunning)
        return;

    tickCreditedByPump = (on == 0) ? 1 : 0;
    CORE(MG21_SYSTICK_CTRL) = tickCtrl | ((on != 0) ? MG21_SYSTICK_CTRL_TICKINT : 0u);
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

int Clock_TickRunning(void)
{
    return tickRunning;
}

/* ---- the cycle stopwatch ---- */

uint32_t Tick_CycleStamp(void)
{
    return cyclesRunning ? CORE(MG21_DWT_CYCCNT) : 0u;
}

uint32_t Tick_CyclesSince(uint32_t stamp)
{
    return cyclesRunning ? (CORE(MG21_DWT_CYCCNT) - stamp) : 0u;
}

int Clock_CyclesRunning(void)
{
    return cyclesRunning;
}

/* ---- read-backs ---- */

static void clockEmitPair(volatile uint32_t *reg, uint16_t rsnLow, uint16_t rsnHigh)
{
    uint32_t v = *reg;

    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)v, rsnLow);

    if (rsnHigh != 0u)
        Debug_Emit(DBG_COMP_WITNESS, (uint16_t)(v >> 16), rsnHigh);
}

void Clock_WitnessReport(void)
{
    clockEmitPair(&MG21_CMU->SYSCLKCTRL, DBG_WITNESS_RSN_MG21_SYSCLKCTRL, 0u);
    clockEmitPair(&MG21_CMU->EM01GRPACLKCTRL, DBG_WITNESS_RSN_MG21_EM01GRPACLK, 0u);
    clockEmitPair(&MG21_CMU->EM23GRPACLKCTRL, DBG_WITNESS_RSN_MG21_EM23GRPACLK, 0u);
    clockEmitPair(&MG21_CMU->WDOG0CLKCTRL, DBG_WITNESS_RSN_MG21_WDOG_CLKSEL, 0u);
    clockEmitPair(&MG21_HFXO0->STATUS, DBG_WITNESS_RSN_MG21_HFXO_STATUS,
                  DBG_WITNESS_RSN_MG21_HFXO_STATUS_HI);
    clockEmitPair(&MG21_HFXO0->CTRL, DBG_WITNESS_RSN_MG21_HFXO_CTRL, 0u);
    clockEmitPair(&MG21_LFRCO->STATUS, DBG_WITNESS_RSN_MG21_LFRCO_STATUS,
                  DBG_WITNESS_RSN_MG21_LFRCO_STATUS_HI);
    clockEmitPair(&MG21_WDOG0->CFG, DBG_WITNESS_RSN_MG21_WDOG_CFG, DBG_WITNESS_RSN_MG21_WDOG_CFG_HI);
    clockEmitPair(&MG21_WDOG0->EN, DBG_WITNESS_RSN_MG21_WDOG_EN, 0u);
    clockEmitPair(&CORE(MG21_SYSTICK_CTRL), DBG_WITNESS_RSN_MG21_SYSTICK_CTRL, 0u);
    clockEmitPair(&MG21_SYSCFG->CFGSYSTIC, DBG_WITNESS_RSN_MG21_SYSTIC_CFG, 0u);
    clockEmitPair(&MG21_SYSCFG->CHIPREV, DBG_WITNESS_RSN_MG21_CHIPREV, 0u);

    /* The core-clock reload does not fit 16 bits, so the reload travels as two records with the
     * same reason and the periods-per-millisecond count travels beside them. */
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)CORE(MG21_SYSTICK_LOAD),
               DBG_WITNESS_RSN_MG21_SYSTICK_LOAD);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)(CORE(MG21_SYSTICK_LOAD) >> 16),
               DBG_WITNESS_RSN_MG21_SYSTICK_LOAD);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)tickPlan.msPerPeriod, DBG_WITNESS_RSN_MG21_TICK_PERIOD);

    if (cyclesRunning)
        Debug_Emit(DBG_COMP_WITNESS, (uint16_t)CORE(MG21_DWT_CYCCNT), DBG_WITNESS_RSN_MG21_CYCCNT);
}

/* ---- regime answers ---- */

uint32_t Clock_SysclkHz(void)
{
    return clockSysclkHz;
}

uint32_t Clock_LinkHz(void)
{
    return clockLinkHz;
}

int Clock_Regime(void)
{
    return clockRegime;
}

/* ---- hand-back ---- */

/* The bootloader is entered straight after a reset and assumes the reset state: its clock query
 * (util_getClockFreq in the shipped ELF) believes HFXO whenever SYSCLKCTRL names it and the HFRCO
 * band otherwise, and its delay timer and its UART are timed from that answer. A soft reset need not
 * restore everything this file changed - the debug domain, at least, survives it - and on the bench
 * the stick answered nothing after any soft reset this image issued until a power cycle
 * (docs/hardware_mg21.md §10.7). So every change is undone here, in the reverse order of Clock_Init. */
void Clock_Release(void)
{
    CORE(MG21_SYSTICK_CTRL) = 0u;
    tickRunning = 0;

    CORE(MG21_DWT_CTRL) &= ~MG21_DWT_CTRL_CYCCNTENA;
    CORE(MG21_SCB_DEMCR) &= ~MG21_DEMCR_TRCENA;
    cyclesRunning = 0;

    MG21_CMU->LOCK = MG21_CMU_LOCKKEY_UNLOCK;
    MG21_CMU->SYSCLKCTRL = (MG21_CMU->SYSCLKCTRL & ~MG21_CMU_SYSCLKCTRL_CLKSEL_MASK)
                           | MG21_CMU_SYSCLKCTRL_CLKSEL_FSRCO;
    MG21_CMU->EM01GRPACLKCTRL = MG21_CMU_EM01GRPACLKCTRL_CLKSEL_HFRCODPLL;
    MG21_CMU->EM23GRPACLKCTRL = MG21_CMU_EM23GRPACLKCTRL_CLKSEL_LFRCO;

    MG21_HFXO0->LOCK = MG21_HFXO_LOCK_LOCKKEY_UNLOCK;
    MG21_HFXO0->CTRL = MG21_HFXO_CTRL_DISONDEMAND;
}

/* ---- terminal path ---- */

/* Hold, feeding, then leave: the caller resets (hal/mg21/hal_mg21.c), so the reset that ends a fault
 * is this image's, at a known time, instead of a watchdog timeout whose reason the bootloader reads
 * as an unexplained one. With no cycle counter the hold is counted in rounds, which is approximate
 * and says so by being bounded. */
void Clock_FaultReport(void)
{
    uint32_t start;
    uint32_t round;

    if (cyclesRunning)
    {
        start = CORE(MG21_DWT_CYCCNT);

        for (round = 0u; round < CLOCK_FAULT_HOLD_ROUNDS; ++round)
        {
            if ((CORE(MG21_DWT_CYCCNT) - start) >= CLOCK_FAULT_HOLD_CYCLES)
                break;

            Wdt_Feed();
        }

        return;
    }

    for (round = 0u; round < CLOCK_FAULT_HOLD_ROUNDS; ++round)
        Wdt_Feed();
}
