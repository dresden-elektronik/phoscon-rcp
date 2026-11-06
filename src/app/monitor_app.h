/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* monitor_app.h — the bench register monitor in a probe image (DS_MONITOR, docs/hardware_mg21.md
 * §10.17).
 *
 * The link's received bytes are fed in from the link's interrupt; one HDLC frame at a time becomes a
 * pending command, which the main loop executes and answers as DBG_COMP_MON records (hal/dbg.h). A
 * command that arrives while one is pending is dropped and counted, never queued. */
#ifndef MONITOR_APP_H
#define MONITOR_APP_H

#include <stdint.h>

/* Received link bytes, in the link's interrupt context. */
void Monitor_Feed(const uint8_t *buf, uint16_t len);

/* Execute and answer a pending command, from the main loop. */
void Monitor_Poll(void);

#endif /* MONITOR_APP_H */
