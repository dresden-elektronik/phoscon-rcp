/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* esp32_panic.h — the ESP32 fault path's board-private half (hal/hal.h Panic_Enter). */
#ifndef ESP32_PANIC_H
#define ESP32_PANIC_H

#include <stdint.h>

/* HAL_Init calls this once, before the main loop can change anything: a previous run's mark becomes
 * records. */
void Panic_ReportPrevious(void);

/* Called by the trap entry (startup/startup_esp32.S) for every exception; never returns. */
void Esp32Trap_Exception(uint32_t mcause, uint32_t mepc, uint32_t mtval);

#endif /* ESP32_PANIC_H */
