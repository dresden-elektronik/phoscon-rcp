/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mg21_clock.h — what the ConBee III clock module offers its own board glue (docs/hardware_mg21.md
 * §10.5). Not in hal/hal.h: these are this silicon's facts, and a second MG21 file may want them.
 *
 * The regime the bring-up ended in is the answer everything else depends on. The part runs from its
 * 20 MHz FSRCO out of reset; this board's clock is the 38.4 MHz HFXO; and if the crystal does not
 * come up the image stays on FSRCO and says so, because every divisor on the board — SysTick's
 * reload, the USART's CLKDIV — is computed from whichever clock is really running. */
#ifndef MG21_CLOCK_H
#define MG21_CLOCK_H

#include <stdint.h>

/* What Clock_Init returns: the regime the board is in. */
#define MG21_CLOCK_DOWN 0 /* nothing ticks: the image cannot keep time and must not open a link */
#define MG21_CLOCK_HFXO 1 /* the crystal: 38.4 MHz */
#define MG21_CLOCK_FSRCO 2 /* the fallback: the part's own 20 MHz fast-start RC */

/* 1 = the system clock and the low-energy group clock both came up as asked. */
int Clock_Init(void);

/* The processor clock the regime produced, and the clock USART0 and GPIO run on (the EM0/EM1 group
 * A clock, which is a select of its own in CMU and not the processor clock by definition). */
uint32_t Clock_SysclkHz(void);
uint32_t Clock_LinkHz(void);

/* 1 = SysTick is running, which is what Tick_GetMs() counts. */
int Clock_TickRunning(void);

/* 1 = DWT's CYCCNT counts, which is what Tick_CycleStamp() reads. */
int Clock_CyclesRunning(void);

/* Which regime the bring-up ended in: MG21_CLOCK_HFXO or MG21_CLOCK_FSRCO. */
int Clock_Regime(void);

/* Hand the millisecond from the polled counter (bring-up, interrupts masked) to the SysTick vector,
 * or back. Called once, as HAL_Init unlocks interrupts. */
void Clock_TickCountedByIrq(int on);

/* The bring-up read-backs, as records (DBG_COMP_WITNESS). Called once, after bring-up, before the
 * host link's own witnesses. */
void Clock_WitnessReport(void);

/* Hand the clock tree back as reset left it, before a reset into the bootloader: SysTick off, the
 * system clock and the EM0/EM1 group A clock back on their reset sources, HFXO back to its reset
 * CTRL, and the cycle counter and DEMCR.TRCENA cleared - the last two sit in the debug domain, which
 * only a power-on resets. Called with interrupts masked; Tick_GetMs() stops counting. */
void Clock_Release(void);

/* The terminal path hal/mg21/mg21_panic.c uses: hold for a bounded time, feeding the watchdog, so
 * the reset that follows is the board's own rather than a timeout it cannot explain. */
void Clock_FaultReport(void);

#endif /* MG21_CLOCK_H */
