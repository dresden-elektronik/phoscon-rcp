/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* Board glue for a SAMR21 target: startup, HAL lifecycle (hal.h). The host link is
 * the board's (hal/com.h, chosen by targets/<board>/board.cmake), so this file names no
 * link at all. */
#include <stdint.h>

#include "hal.h"
#include "hal_diag.h"
#include "board.h"
#include "com.h"
#include "dbg.h"
#include "samr21_regs.h"

/* Linker symbols (targets/cb2/atsamr21.ld). */
extern uint32_t _sdata[];
extern uint32_t _edata[];
extern uint32_t _sbss[];
extern uint32_t _ebss[];
extern uint32_t _data_load;

/* Board bring-up (samr21_clock.c, and the host link this board's target names). */
extern int Clock_Init(void);


extern int main(void);

/* Handler-mode depth: SysTick masks only while it runs, so a handler can preempt
 * a handler. */
static uint8_t halIrqDepth;

void HAL_IrqEnter(void)
{
    halIrqDepth++;
}

void HAL_IrqExit(void)
{
    if (halIrqDepth != 0u)
        halIrqDepth--;
}

int HAL_InIrqContext(void)
{
    return halIrqDepth != 0u;
}

#ifdef DS_SCRATCH_ROW
/* Every Debug_Emit lands in the flash status row's words 2..4; in handler mode it
 * only queues - the scratch row cannot be programmed from an IRQ. Staging is opt-in
 * (HAL_DiagStage): a board that never hears from the host writes no rows here, and
 * the host-visible record stream is a separate path. The periodic milestone and
 * liveness marks never pass through this sink. */
static int boardDiagStage; /* plain int, 0/1; default 0 = staging off */

static void boardDbgSink(uint16_t component, uint16_t line, uint16_t reason)
{
    if (boardDiagStage == 0)
        return;

    Nvram_StatusDebug(component, line, reason);
}

void HAL_DiagStage(int on)
{
    boardDiagStage = (on != 0) ? 1 : 0;
}
#endif

/* After a bare-POR boot the core still uses VTOR 0x0, where erased flash returns
 * 0xFFFFFFFF, so the first exception faults into a void. Point VTOR at our table
 * (the board's image origin). */
#define SCB_VTOR_ADDR 0xE000ED08u

void HAL_PreMain(void)
{
    uint32_t *dst;
    const uint32_t *src;

    *(volatile uint32_t *)SCB_VTOR_ADDR = Board_Get()->appOrigin;
    HAL_Dsb();

    /* The bootloader jumps with its link live and that vector possibly pending; mask
   * all peripheral IRQs until the board's own link bring-up re-enables its one on
   * clean hardware. PRIMASK stays set anyway until HAL_Init unlocks it last. */
    NVIC_ICER0 = 0xFFFFFFFFu;
    NVIC_ICER1 = 0xFFFFFFFFu;

    dst = _sbss;

    while (dst < _ebss)
        *dst++ = 0;

    dst = _sdata;
    src = (const uint32_t *)&_data_load;

    while (dst < _edata)
        *dst++ = *src++;

#ifdef DS_SCRATCH_ROW
    Debug_SetSink(boardDbgSink);
#endif

    DBG(DBG_COMP_STARTUP, DBG_STARTUP_RSN_BOOT);
    /* The reset cause as the bootloader left it: its jump is not a reset, so a watchdog reset of
     * this image still reads WDT here. Read-only, and latched until the next reset. */
    Debug_Emit(DBG_COMP_WITNESS, SAMR21_PM->RCAUSE, DBG_WITNESS_RSN_SAMR21_RCAUSE);
    main();
}

int HAL_Init(void)
{
    int ok;

    /* Start the stopwatch from here so every DBG_COMP_TIME milestone shares one zero
   * (hal/samr21/samr21_clock.c). */
    Clock_BringupClockStart();
    Nvram_StatusStage(NVRAM_STG_HAL_ENTRY);

    /* Clear the bootloader update flag first: while byte 0x4FFF is set, the V2_07
   * bootloader erases app page 0 at the next boot (eternal window). */
    Nvram_ClearUpdateFlag();
    Nvram_StatusStage(NVRAM_STG_NVRAM_FLAG);

    /* Vendor parity: no flash programming during bring-up. */
    Nvram_StatusQuiet(1);
    ok = Clock_Init(); /* 1 = 48 MHz closed loop, 2 = 48 MHz USBCRM
                        * failover, 0 = still at 8 MHz (no USB possible) */
    if (ok)
    {
        Clock_Stamp(DBG_TIME_RSN_LINK_ENTER);
        Clock_CalibStamp();
        if (!COM_Init())
            DBG(DBG_COMP_APP, DBG_APP_RSN_LINK_INIT_FAIL);
        Clock_Stamp(DBG_TIME_RSN_LINK_EXIT);

        Nvram_StatusStage(NVRAM_STG_LINK_DONE);
    }

    /* The one and only global unlock, last: the SysTick IRQ, plus the link's vector when
   * ok. The terminal mark names the regime (8 = closed loop, 9 = USBCRM failover). */
    Nvram_StatusQuiet(0);
    Nvram_StatusFlush();

    HAL_IrqUnlock();
    Clock_Stamp(DBG_TIME_RSN_HAL_EXIT);
    Nvram_StatusStage(ok == 2 ? NVRAM_STG_HAL_DONE_USBCRM : NVRAM_STG_HAL_DONE);


    return ok;
}

/* Cortex-M0+ core SCB (SAMR21 datasheet, "Cortex-M0+ core" chapter):
 * VECTKEY 0x5FA in [31:16] unlocks AIRCR, SYSRESETREQ (bit 2) requests the reset. */
#define SCB_AIRCR_ADDR 0xE000ED0Cu
#define SCB_AIRCR_RESET 0x05FA0004u

void HAL_SystemReset(void)
{
    /* Write the queued scratch record before the system ends. */
    Nvram_StatusFlush();

    /* DSB: the AIRCR store must commit before the reset event. */
    HAL_Dsb();
    *(volatile uint32_t *)SCB_AIRCR_ADDR = SCB_AIRCR_RESET;
    HAL_Dsb();

    for (;;)
    {
    }
}
