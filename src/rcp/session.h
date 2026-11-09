/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* session.h — the RCP's session (docs/architecture.md §2). */
#ifndef RCP_SESSION_H
#define RCP_SESSION_H

#include <stdint.h>

#include "props.h"
#include "props_stream.h"
#include "spinel.h"
#include "u_bstream.h"

/* The largest Spinel value on either side of the hop: an inbound SET STREAM_RAW request is
 * 2 + RADIO_PSDU_MAX + PROPS_STREAM_TX_TRAILER = 145 bytes, an outbound STREAM_RAW
 * notification is RADIO_PSDU_MAX + 28 metadata = 155 bytes; PROPS_STREAM_RX_MAX (160) is
 * the outbound ceiling and covers both. */
#define RCP_VALUE_MAX PROPS_STREAM_RX_MAX

/* An Ack passed back in a completion: an Immediate Ack, or an Enhanced Ack with its IEs. Anything longer
 * is not an Ack a Thread parent sees, and the completion carries none. */
#define RCP_TX_ACK_MAX 64u

/* The buffer Rcp_TxDone needs, whichever completion it builds: a four-octet header and property,
 * a packed status of at most five, Frame Pending and headerUpdated, the 28 octets around an Ack frame
 * and the frame, then a key index and a four-octet frame counter. */
#define RCP_TX_DONE_MAX (4u + 5u + 2u + 28u + RCP_TX_ACK_MAX + 5u)

/* How long a transmission may stay in flight, beyond the moment it asked for, before the session calls
 * itself unhealthy. Every back-end clamps the retries to 15 and bounds each attempt by its own
 * deadline (50 ms per attempt on the AT86RF233 boards, one CSMA-CA round under RAIL), so the slowest
 * outcome a congested channel can produce is under a second; a transmission still open after this
 * lost its completion, and every later STREAM_RAW would be answered BUSY. */
#define RCP_TX_STUCK_MS 2000u

typedef int (*Rcp_TxSink)(void *ctx, const PropsStream_TxRequest *req, uint8_t tid);

typedef struct
{
    Props_State props;
    Props_Identity identity;

    uint32_t framesIn;
    uint32_t malformed;
    uint32_t unknownCmd;
    uint32_t unknownProp;
    uint32_t rejected;
    uint32_t dropped;

    Rcp_TxSink txSink;
    void *txCtx;
    int txInFlight;
    uint8_t txTid;
    /* Rcp_Healthy's clock on the transmission in flight: cleared at submit, started on its first look. */
    int txWatched;
    uint32_t txSinceMs;
    uint32_t txLimitMs;
#ifdef RCP_THREAD
    MacSec_TxResult txSec;
    const Radio_Frame *txAck;
#endif
} Rcp_Session;

void Rcp_Init(Rcp_Session *s, const Props_Identity *id);

uint16_t Rcp_Reset(Rcp_Session *s, U_BStream *out);

uint16_t Rcp_Feed(Rcp_Session *s, const uint8_t *payload, uint16_t len, U_BStream *out);

void Rcp_SetTxSink(Rcp_Session *s, Rcp_TxSink sink, void *ctx);

/* framePending is the received ACK's Frame Pending bit (0/1); the session stays pure, so it
 * crosses from the radio as a plain int. */
#ifdef RCP_THREAD
/* The Ack the next Rcp_TxDone passes back, 0 for none. */
void Rcp_SetTxAck(Rcp_Session *s, const Radio_Frame *ack);
#endif

uint16_t Rcp_TxDone(Rcp_Session *s, uint8_t tid, int radioStatus, int framePending, U_BStream *out);

/* 0 once a transmission has been in flight longer than RCP_TX_STUCK_MS plus its own delay, else 1.
 * Called once per main-loop pass, which gates the watchdog feed on it (docs/architecture.md §7); the
 * first call that sees a transmission starts its clock. */
int Rcp_Healthy(Rcp_Session *s, uint32_t nowMs);

#endif
