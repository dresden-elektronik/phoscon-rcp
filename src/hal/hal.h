/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* HAL public API: the SoC-services contract (docs/boards.md §4). Board
 * specifics (SAMR21 now, MG21 later) stay behind it; the host link is hal/com.h
 * and the radio hal/radio.h. Status values are int (1 = ok, 0 = fail). */
#ifndef HAL_H
#define HAL_H

#include <stdint.h>

/* ---- Lifecycle ---- */
/* Never returns; called first by the startup, before clocks (.data/.bss). */
void HAL_PreMain(void);
/* Clocks (48 MHz), WDT feeding, the host link (hal/com.h COM_Init). 1 = closed-loop DFLL
 * locked, 2 = USBCRM failover, 0 = CPU left at 8 MHz and the link skipped. */
int HAL_Init(void);

/* ---- System ---- */
void HAL_SystemReset(void);

/* The four core instructions (coding-rules.md §1), one set per instruction set. */
#if defined(__AVR__)

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

#else

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

/* ---- Timers ---- */
/* Bring-up stopwatch wait hook: called by every bounded wait; a no-op until the
 * stopwatch was started (hal_diag.h). */
void Clock_BringupPump(void);

/* Monotonic 1 ms tick since boot; wraps after 2^32 ms (49.7 d), so compare
 * deadlines modularly, never with a plain >= near the wrap. */
void Tick_GetMs(uint32_t *ms);
void Delay_Ms(uint32_t ms);

/* Processor-cycle stopwatch on SysTick's down-counter. A stamp is only comparable with a later
 * one inside a single tick period (one board millisecond); the radio's turnaround, a few
 * hundred microseconds, is what it exists for. Callable from any context. */
uint32_t Tick_CycleStamp(void);
uint32_t Tick_CyclesSince(uint32_t stamp);

/* The radio clock of a board with timed transmit: free-running microseconds counted from the radio's
 * crystal, wrapping at 2^32 with no jump, and readable in any context. Tick_ClockStart is its bring-up,
 * called once the clock tree runs. */
void Tick_ClockStart(void);
uint32_t Tick_ClockUs(void);

/* ---- Watchdog ----
 * Booting, every bounded wait feeds it (Wdt_FeedPoll). Wdt_LoopStart, called once by the main loop
 * before its first pass, ends that: from then on only a completed pass feeds, so the watchdog proves
 * the loop runs, not that some wait is still spinning. No vector ever feeds it (docs/architecture.md §7). */
void Wdt_Feed(void);
void Wdt_LoopStart(void);

/* ---- Radio master clock (hal/samr21/samr21_rf.c) ---- */
/* SPI-wake the AT86RF233 and force CLKM to the vendor 8 MHz. 1 = provably
 * answered (PART_NUM 0x0B, TRX_STATUS held 0x08 over four reads, read-back
 * matched; at86rf233 9.6.4 note 1); 0 = CLKM unproven, GCLKIN unusable. */
int Rf_WakeAndForceClkm(void);

/* ---- Interrupt context (handler-mode tracking) ---- */
/* Every IRQ vector brackets its body with these; a depth, not a flag (handlers
 * preempt). HAL_InIrqContext() is 1 while any handler runs - work that must not
 * run there (flash programming, ~16 ms of stalled AHB) tests it and defers. */
void HAL_IrqEnter(void);
void HAL_IrqExit(void);
int HAL_InIrqContext(void);

/* ---- Device identity (NVRAM / NVM user row) ---- */
/* 1 = mac8 holds the 8-byte MAC (deCONZ order, byte 0 = LSB), from the
 * CRC-checked basic set 0x3FFE0 or, if absent, the chip UID. */
int Nvram_MacValid(uint8_t *mac8);
/* Factory EUI-64, 8 bytes MSB-first (Spinel HWADDR order, reverse of the user
 * row's storage); 1 = programmed. 0 covers a partly-programmed row too: the
 * value must be unicast (I/G clear), which the vendor's all-0/all-0xFF test
 * does not check. The host-set operational address is MAC_15_4_LADDR. */
int Nvram_FactoryEui64(uint8_t *eui8);
/* Production serial (ASCII, user row 0x804012) into buf; returns the byte count,
 * 0 if never programmed. */
uint8_t Nvram_SerialAscii(uint8_t *buf, uint8_t maxLen);
/* The 12-byte chip UID at 0x80400A. */
void Nvram_Uid(uint8_t *uid12);
/* Clear the bootloader's update flag at 0x4FFF: while it is set (0xA3 after a
 * firmware update) the next boot erases the first app flash page 0x5000. Call
 * once at every boot, after the clocks are up. */
void Nvram_ClearUpdateFlag(void);

/* ---- Panic ---- */
/* Locks IRQs, best-effort record, then idles; the WDT resets into the
 * bootloader window. Never returns. */
void Panic_Enter(uint16_t component, uint16_t line, uint16_t reason);

#endif
