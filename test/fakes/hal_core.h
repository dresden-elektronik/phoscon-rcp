/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* hal_core.h — the four core instructions for a host suite: there is no core to sleep or mask, so a
 * suite that calls them defines them (test/fakes/). */
#ifndef HAL_CORE_H
#define HAL_CORE_H

void HAL_Wfi(void);
void HAL_Dsb(void);
void HAL_IrqLock(void);
void HAL_IrqUnlock(void);

#endif
