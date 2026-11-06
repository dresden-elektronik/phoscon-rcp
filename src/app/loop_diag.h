/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* loop_diag.h - main-loop instrumentation (DS_LOOP_DIAG builds only).
 *
 * The field symptom is a reply that takes seconds to leave the RCP. This localises it: a
 * periodic record carries the loop iterations per second (a healthy loop is ~1 000/s, one
 * per SysTick), and each loop phase emits a record only when it ran longer than
 * LOOP_DIAG_SLOW_MS, so a stall names itself instead of being inferred.
 *
 * Records use the project's existing (component, line, reason) idiom, but here the line
 * field is a phase id, not __LINE__: LOOP_DIAG_LINE_LOOP carries iterations, every other
 * line carries the elapsed ms. They reach the host exactly like every other record - in
 * RCP mode through PROPS_ID_RCP_LOG_CRASH_DUMP once the host enables it - so the same
 * instrumentation works on hardware and in the emulator.
 *
 * DS_LOOP_DIAG is a build option (src/CMakeLists.txt). When it is off the header provides
 * no-ops and the loop pays nothing; when it is on the probe costs two Tick_GetMs reads per
 * phase plus a few counters.
 */
#ifndef LOOP_DIAG_H
#define LOOP_DIAG_H

#include <stdint.h>

enum
{
    LOOP_DIAG_LINE_LOOP = 1, /* periodic: reason = loop iterations in the last second */
    LOOP_DIAG_LINE_APP_POLL, /* reason = App_Poll elapsed ms when >= the threshold */
    LOOP_DIAG_LINE_COM_NOTIFY,
    LOOP_DIAG_LINE_NVRAM_FLUSH,
    LOOP_DIAG_LINE_NVRAM_PATCH /* periodic: reason = NVRAM row patches in the last second */
};

/* A phase slower than this is worth a record; 20 ms is twenty missed SysTick periods. */
#define LOOP_DIAG_SLOW_MS 20u

#ifdef DS_LOOP_DIAG

/* Once per main-loop pass, with the pass's own ms stamp. */
void LoopDiag_Tick(uint32_t ms);
/* Bracket one phase; End emits when the phase crossed LOOP_DIAG_SLOW_MS. */
void LoopDiag_Begin(void);
void LoopDiag_End(uint16_t line);

#else

static inline void LoopDiag_Tick(uint32_t ms)
{
    (void)ms;
}

static inline void LoopDiag_Begin(void)
{
}

static inline void LoopDiag_End(uint16_t line)
{
    (void)line;
}

#endif

#endif
