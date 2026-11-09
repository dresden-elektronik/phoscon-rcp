/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* hal_core.h — the four core instructions (coding-rules.md §1) on the Cortex-M parts: the SAMR21's M0+
 * and the MG21's M33. hal/hal.h includes the copy its target's include path names. */
#ifndef HAL_CORE_H
#define HAL_CORE_H

static inline void HAL_Wfi(void)
{
    __asm__ volatile("wfi");
}

static inline void HAL_Dsb(void)
{
    __asm__ volatile("dsb 0xf" ::: "memory");
}

static inline void HAL_IrqLock(void)
{
    __asm__ volatile("cpsid i" ::: "memory");
}

static inline void HAL_IrqUnlock(void)
{
    __asm__ volatile("cpsie i" ::: "memory");
}

#endif
