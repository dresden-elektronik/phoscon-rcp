/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* avr_panic.h — the ConBee I / RaspBee I fault path's board-private half. Panic_Enter itself is in
 * hal/hal.h. */
#ifndef AVR_PANIC_H
#define AVR_PANIC_H

/* Report the mark a previous run's panic left in .noinit, once, before the main loop runs. */
void Panic_ReportPrevious(void);

#endif /* AVR_PANIC_H */
