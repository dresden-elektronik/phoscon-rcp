/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* main.c — bring up the board's link to the host; on the host flasher's reset frame, reset into the
 * bootloader window. Mode-neutral: the image supplies the loop hooks (app_main.h). */
#include "hal.h"
#include "hal_diag.h"
#include "com.h"
#include "dbg.h"

#include "flasher_trap.h"

#include "app_main.h"
#include "loop_diag.h"


/* Liveness cadence: a mark is a full flash row erase+rewrite, so the step is
 * minutes, not 1 Hz (endurance note in samr21_nvram.c). */
#define STATUS_FIRST_MS 1000u
#define STATUS_STEP_MS 60000u

#ifdef DS_FLASHER_ACK
/* Set by the link interrupt when the trap arms, cleared by the loop once the reply is on its way. */
static volatile uint8_t flasherAckPending;
#endif

static void onComDataOut(const uint8_t *buf, uint16_t len)
{
    /* The host flasher asks for a reset on this same byte stream (flasher_trap.h). */
    if (FlasherTrap_Feed(buf, len))
    {
        DBG(DBG_COMP_APP, DBG_APP_RSN_FLASHER_RESET);
#ifdef DS_FLASHER_ACK
        flasherAckPending = 1u;
#endif
    }

    App_DataOut(buf, len);
}

/* Replay window: minutes, because the harness needs ~12 s from flash to tty
 * (dbg.h Debug_ShadowReplay). */
#define REPLAY_FIRST_MS 3000u
#define REPLAY_STEP_MS 2500u
#define REPLAY_LIMIT 40u


int main(void)
{
    int ok;
    uint32_t nextReplayMs;
    uint32_t replays;

    Debug_ShadowHold(1);
    ok = HAL_Init();
    DBG(DBG_COMP_APP, DBG_APP_RSN_HELLO);
    /* Read the bring-up state back before anything else can change it (dbg.h DBG_COMP_WITNESS).
     * Which read-backs exist is a board capability: the stopwatch, the clock-tree witnesses and the
     * post-mortem scratch row are the SAMR21's (hal/hal_diag.h), and a board that has none reports
     * its own through its HAL files and through COM_Witnesses (hal/uart_drv.h). */
#ifdef DS_BOARD_DIAG
    Clock_Witnesses();
#endif
    COM_Witnesses();
    COM_SetRxHandler(onComDataOut);
    App_Boot();

    /* From here on a completed pass is the only thing that feeds the WDT (hal/hal.h),
     * and only when HAL_Init reported the bring-up good: a failed one ends in the
     * watchdog reset. Deadline arithmetic is modular, so it is correct across the
     * 49.7 d tick wrap. */
#ifdef DS_SCRATCH_ROW
    uint32_t nextStatusMs = STATUS_FIRST_MS;
#endif

#ifdef DS_BOARD_DIAG
    Clock_Stamp(DBG_TIME_RSN_MAIN_FIRST);
#endif
    nextReplayMs = REPLAY_FIRST_MS;
    replays = 0u;
    Wdt_LoopStart();

    for (;;)
    {
        uint32_t ms;
        Tick_GetMs(&ms);
        LoopDiag_Tick(ms);

#ifdef DS_SCRATCH_ROW
        if ((int32_t)(ms - nextStatusMs) >= 0)
        {
            Nvram_StatusTick(ms);
            nextStatusMs = ms + STATUS_STEP_MS;
        }
#endif

        if (replays < REPLAY_LIMIT && (int32_t)(ms - nextReplayMs) >= 0)
        {
            /* Only once configured, and only into the queue - never through the sink
             * (dbg.h). */
            if (COM_Present())
                Debug_ShadowReplay();

            replays++;
            nextReplayMs = ms + REPLAY_STEP_MS;
        }

#ifdef DS_FLASHER_ACK
        /* A board whose flasher waits for the reply (flasher_trap.h) gets it before the reset is due;
         * a link still busy with a chunk takes it on a later pass. */
        if (flasherAckPending != 0u && COM_TxSend(FlasherTrap_Ack(), FLASHER_TRAP_ACK_LEN))
            flasherAckPending = 0u;
#endif

        if (FlasherTrap_Due(ms))
        {
            DBG(DBG_COMP_APP, DBG_APP_RSN_FLASHER_RESET);
            HAL_SystemReset();
        }

        if (ok)
        {
            LoopDiag_Begin();
            App_Poll(ms);
            LoopDiag_End(LOOP_DIAG_LINE_APP_POLL);
            LoopDiag_Begin();
            COM_NotifyPoll();
            LoopDiag_End(LOOP_DIAG_LINE_COM_NOTIFY);

            if (App_Healthy(ms))
                Wdt_Feed();
        }
#ifdef DS_SCRATCH_ROW
        /* The only place an ISR-queued record can reach flash: Nvram_StatusStage and
         * Nvram_StatusDebug defer in handler mode (samr21_nvram.c). */
        LoopDiag_Begin();
        Nvram_StatusFlush();
        LoopDiag_End(LOOP_DIAG_LINE_NVRAM_FLUSH);
#endif
        HAL_Wfi();
    }
}
