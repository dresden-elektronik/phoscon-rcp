/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* app_main.h - the entry hooks the shared loop calls; exactly one mode file defines them.
 *
 * main.c is mode-neutral: it brings the board up, watches the link's OUT for the flasher's reset frame and
 * runs the loop. Which image is being built decides what the loop does with the transport, and that
 * is a link-time choice (src/CMakeLists.txt) instead of an #ifdef, so neither image's main carries the
 * other image's code.
 *
 * Exactly one consumer drains the record queue: in probe mode the link's IN takes the records in App_Poll
 * (COM_TxPoll -> Usbc_CdcInPoll), while the RCP session owns that pipe and publishes them in-band as
 * RCP_LOG_CRASH_DUMP (rcp_app.c). Two drains would race for the same queue. */
#ifndef APP_MAIN_H
#define APP_MAIN_H

#include <stdint.h>

/* Once, after HAL_Init() and before the loop. */
void App_Boot(void);

/* Every COM OUT packet, from the link interrupt: hand it to the transport if this mode has one. */
void App_DataOut(const uint8_t *buf, uint16_t len);

/* Once per main-loop pass, while the bring-up succeeded. */
void App_Poll(uint32_t ms);

#endif
