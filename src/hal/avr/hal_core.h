/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* hal_core.h — the four core instructions (coding-rules.md §1) on the ATmega256RFR2. hal/hal.h includes
 * the copy its target's include path names. */
#ifndef HAL_CORE_H
#define HAL_CORE_H

/* Sleeps in the mode the board's SMCR selects (its SE bit set at init). */
static inline void HAL_Wfi(void)
{
    __asm__ volatile("sleep" ::: "memory");
}

/* The AVR core has no write buffer to drain: a compiler barrier is all a barrier needs to be. */
static inline void HAL_Dsb(void)
{
    __asm__ volatile("" ::: "memory");
}

static inline void HAL_IrqLock(void)
{
    __asm__ volatile("cli" ::: "memory");
}

static inline void HAL_IrqUnlock(void)
{
    __asm__ volatile("sei" ::: "memory");
}

#endif
