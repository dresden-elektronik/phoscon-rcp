/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mg21_panic.h — the ConBee III fault path's board-private entry (hal/hal.h carries Panic_Enter). */
#ifndef MG21_PANIC_H
#define MG21_PANIC_H

/* Report the fault a previous run left in SRAM, as records, and clear the mark. Called once from
 * HAL_PreMain, before the main loop can produce anything that would be confused with it. */
void Panic_ReportPrevious(void);

#endif /* MG21_PANIC_H */
