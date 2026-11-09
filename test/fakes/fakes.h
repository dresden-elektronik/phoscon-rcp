/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* fakes.h — the board edge for a host suite that runs app/ glue (test/fakes/): the four core
 * instructions, the tick, the board descriptor, the host link and a radio that keeps hal/radio.h's
 * contract. The suite drives time and reads what reached the link. */
#ifndef FAKES_H
#define FAKES_H

#include <stdint.h>

/* ---- time and context ---- */
extern uint32_t fakeNowMs;
extern int fakeIrqDepth;     /* HAL_IrqLock nesting: never below 0, back to 0 after a pass */
extern int fakeIrqUnbalanced; /* an unlock without its lock */

/* ---- the host link: every accepted write, in order ---- */
#define FAKE_LINK_OUT_MAX 16384u
extern uint8_t fakeLinkOut[FAKE_LINK_OUT_MAX];
extern uint32_t fakeLinkOutLen;
extern int fakeLinkRefuse; /* 1: COM_TxSend refuses, as a link with a write still in flight does */
void Fake_LinkReset(void);

/* ---- the radio ---- */
extern int fakeRadioInitFails;
extern uint32_t fakeRadioTxMs;   /* how long a transmission takes before its completion */
extern uint32_t fakeRadioTxCount;
extern uint32_t fakeRadioLeftOpen; /* a transmission still waiting for its completion */
extern uint8_t fakeRadioChannel;   /* the channel the radio holds */
extern uint8_t fakeRadioTxChannel; /* the channel the last frame went out on */
extern uint32_t fakeRadioSrcMatchPushes; /* source-match tables the glue handed down */
extern int fakeRadioAddrFails;           /* 1: Radio_SetPanId refuses, as a radio that went away does */
extern uint8_t fakeRadioSrcMatchEnabled; /* and what the radio holds from the last one */
extern uint8_t fakeRadioSrcMatchShorts;
void Fake_RadioReset(void);

#endif
