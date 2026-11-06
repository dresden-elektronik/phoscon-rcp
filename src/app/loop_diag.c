/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* loop_diag.c - main-loop instrumentation (loop_diag.h, DS_LOOP_DIAG builds only). */
#include "loop_diag.h"

#include "hal.h"
#include "hal_diag.h"
#include "dbg.h"

/* Iterations since the last periodic record, and the stamp that opened the window. */
static uint32_t loopDiagIterations;
static uint32_t loopDiagTotal; /* monotonic, for a peek-based probe of the loop rate */
static uint32_t loopDiagWindowMs;
static uint32_t loopDiagPatchBase;

/* The phase start stamp; one phase is timed at a time, so one slot is enough. */
static uint32_t loopDiagPhaseMs;

static uint16_t loopDiagSaturate(uint32_t v)
{
    return v > 0xFFFFu ? 0xFFFFu : (uint16_t)v;
}

/* Row patches completed since boot, for a board that has a scratch row to patch (hal/hal_diag.h). A
 * board without one reports zero, which is exactly what its main loop does to flash. */
static uint32_t loopDiagPatches(void)
{
#ifdef DS_SCRATCH_ROW
    return Nvram_StatusPatches();
#else
    return 0u;
#endif
}

void LoopDiag_Tick(uint32_t ms)
{
    uint32_t patches;
    uint32_t elapsed;

    loopDiagIterations++;
    loopDiagTotal++;

    /* A fresh boot has no baseline: open the window and return. */
    if (loopDiagWindowMs == 0u)
    {
        loopDiagWindowMs = ms + 1000u;
        loopDiagPatchBase = loopDiagPatches();
        return;
    }

    /* Modular compare, correct across the 49.7 d ms wrap. */
    if ((int32_t)(ms - loopDiagWindowMs) < 0)
        return;

    elapsed = loopDiagIterations;
    Debug_Emit(DBG_COMP_APP, LOOP_DIAG_LINE_LOOP, loopDiagSaturate(elapsed));

    patches = loopDiagPatches();
    Debug_Emit(DBG_COMP_APP,
               LOOP_DIAG_LINE_NVRAM_PATCH,
               loopDiagSaturate(patches - loopDiagPatchBase));
    loopDiagPatchBase = patches;

    loopDiagIterations = 0u;
    loopDiagWindowMs = ms + 1000u;
}

void LoopDiag_Begin(void)
{
    Tick_GetMs(&loopDiagPhaseMs);
}

void LoopDiag_End(uint16_t line)
{
    uint32_t now;
    uint32_t elapsed;

    Tick_GetMs(&now);
    elapsed = now - loopDiagPhaseMs;

    if (elapsed >= LOOP_DIAG_SLOW_MS)
        Debug_Emit(DBG_COMP_APP, line, loopDiagSaturate(elapsed));
}
