/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* rcp_app.h — the app-side Spinel transport (CB2_MODE=rcp, docs/architecture.md §1). */
#ifndef APP_RCP_APP_H
#define APP_RCP_APP_H

#include <stdint.h>

/* Ring depth: one whole worst-case wire frame (hdlc.h HDLC_FRAME_MAX) with room for the
 * frame the loop is mid-way through; overflow is dropped-oldest and counted
 * (coding-rules.md §4.3). 512 keeps the ISR's modulo a shift. */
#define RCP_APP_RX_RING 512u

void RcpApp_Init(void);

void RcpApp_PushFromIrq(const uint8_t *buf, uint16_t len);

void RcpApp_Poll(void);

uint32_t RcpApp_RxDropped(void);

uint32_t RcpApp_TxDropped(void);

/* The session's health (session.h Rcp_Healthy), with one record where it turns bad. */
int RcpApp_Healthy(uint32_t ms);

/* The radio's step due without an interrupt (radio.h Radio_PollAgain), once the radio is up. */
int RcpApp_PollAgain(void);

#endif
