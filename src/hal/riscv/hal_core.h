/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* hal_core.h — the four core instructions (coding-rules.md §1) on the RV32 cores of the ESP32 parts.
 * hal/hal.h includes the copy its target's include path names.
 *
 * The lock is mstatus.MIE (bit 3), cleared and set with the CSR immediates, as the Cortex-M copy masks
 * PRIMASK: a plain lock and unlock, not a saved state. The barrier is fence, which orders every
 * memory and I/O access before it against every one after it - the interrupt controller's registers
 * sit on the APB bus, and its own suggested sequence ends a change with one (ESP32-C6 TRM 1.6.3.1). */
#ifndef HAL_CORE_H
#define HAL_CORE_H

static inline void HAL_Wfi(void)
{
    __asm__ volatile("wfi" ::: "memory");
}

static inline void HAL_Dsb(void)
{
    __asm__ volatile("fence" ::: "memory");
}

static inline void HAL_IrqLock(void)
{
    __asm__ volatile("csrci mstatus, 8" ::: "memory");
}

static inline void HAL_IrqUnlock(void)
{
    __asm__ volatile("csrsi mstatus, 8" ::: "memory");
}

#endif
