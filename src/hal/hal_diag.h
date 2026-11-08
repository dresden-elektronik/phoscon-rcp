/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* hal_diag.h — this board's diagnostics and post-mortem machinery. Not in hal.h
 * (docs/architecture.md §2): an MG21 back-end has no bring-up stopwatch,
 * register witnesses or 0x3E000 scratch row. Clock_BringupPump() and the NVRAM
 * identity getters stay in hal.h - an MG21 has those too. CB2-only; several entry
 * points exist only in probe builds, so the shipping image carries none of them. */
#ifndef HAL_DIAG_H
#define HAL_DIAG_H

#include <stdint.h>

/* ---- Bring-up stopwatch (component DBG_COMP_TIME, dbg.h) ---- */
/* Counts SysTick's COUNT register - the processor clock, before any GCLK/PM/NVM
 * step exists. A stamp's line field is cycles/4096, saturating at 5.6 s. */
void Clock_BringupClockStart(void);
void Clock_BringupTick(void); /* credits one period; SysTick vector */
uint16_t Clock_StampUnits(void);
void Clock_Stamp(uint16_t milestone);
/* Runs the fixed calibration loop and stamps it (DBG_TIME_RSN_CALIB). */
void Clock_CalibStamp(void);

/* Feed the watchdog once every 64 polls: a WDT.CLEAR costs ~21 000 CPU cycles.
 * Call it with the loop's own round counter. Only until Wdt_LoopStart (hal/hal.h). */
void Wdt_FeedPoll(uint32_t round);

/* Publish TRX_STATUS and XAH_CTRL_1.AACK_PROM_MODE (component DBG_COMP_RF);
 * CB2-only, diagnostic builds. */
void Radio_Witnesses(void);

/* The NVM user row 0x804000..0x80403F as 32 records in order (component
 * DBG_COMP_FLASH), diagnostic builds. */
void Nvram_WitnessRow(void);

/* ---- Register witnesses (component DBG_COMP_WITNESS, dbg.h) ---- */
/* The clock tree read back: OSC8M prescaler, DFLL mode/mul/fine/coarse, GCLK
 * status, the generators and channels for CPU/USB/DFLL-ref, PM.CPUSEL, SysTick
 * LOAD/CTRL. Every "48 MHz" claim in this tree rests on this set. */
void Clock_Witnesses(void);


/* ---- Post-mortem scratch row (samr21_nvram.c) ---- */
/* A flash row at 0x3E000 that survives every app flash: word 0 = bring-up
 * milestone, word 1 = liveness seconds, words 2..4 = the last debug record.
 * Nvram_StatusFlush writes pending state as one row cycle, a no-op when nothing
 * is pending. Handler-mode requests are queued (an IRQ must not program flash);
 * thread-mode ones are written at once. Probe image only (DS_SCRATCH_ROW): a row
 * cycle stalls every flash fetch, vectors included, for up to 16 ms, longer than
 * the UART's receive buffer lasts, and the row is rated for 25k cycles. In an rcp
 * image these calls compile to nothing. */
#define NVRAM_STG_HAL_ENTRY 1u
#define NVRAM_STG_NVRAM_FLAG 2u
#define NVRAM_STG_CLOCKS_GEN 3u
#define NVRAM_STG_CLOCKS_DFLL 4u /* LCKC+LCKF held */
#define NVRAM_STG_CLOCKS_DONE 5u
#define NVRAM_STG_CLOCKS_USBCRM 6u /* 48 MHz via USBCRM, not via a lock */
#define NVRAM_STG_LINK_DONE 7u /* the host link is up (hal/com.h COM_Init) */
#define NVRAM_STG_HAL_DONE 8u        /* ... on the closed-loop clocks */
#define NVRAM_STG_HAL_DONE_USBCRM 9u /* ... on the USBCRM failover */

#ifdef DS_SCRATCH_ROW

void Nvram_StatusStage(uint16_t stage);
void Nvram_StatusTick(uint32_t ms);
void Nvram_StatusDebug(uint16_t component, uint16_t line, uint16_t reason);
void Nvram_StatusFlush(void);
/* While quiet, stages and records queue in RAM only; the caller flushes once afterwards. */
void Nvram_StatusQuiet(int on);

#ifdef DS_LOOP_DIAG
/* Completed NVRAM row patches since boot (loop_diag.h publishes the per-second delta). */
uint32_t Nvram_StatusPatches(void);
#endif

#else

static inline void Nvram_StatusStage(uint16_t stage)
{
    (void)stage;
}

static inline void Nvram_StatusFlush(void)
{
}

static inline void Nvram_StatusQuiet(int on)
{
    (void)on;
}

#endif

/* ---- Diagnostic sink (debug-record staging) ---- */
/* Gate the board's extra debug-record sink, which stages the last record into the post-mortem
 * row at a flash row rewrite per staged batch. The probe image enables it at boot (main_probe.c).
 * 0 (the default) leaves the host-visible record stream and the periodic milestone/liveness marks
 * untouched and stages nothing. Plain int, not bool. */
#ifdef DS_SCRATCH_ROW
void HAL_DiagStage(int on);
#endif

#endif
