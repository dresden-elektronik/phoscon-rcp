/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* radio.h — the PHY/MAC contract the RCP core consumes
 * (docs/architecture.md §2). One implementation per silicon: samr21_radio.c
 * (AT86RF233 over SERCOM4) now, an MG21 back-end later; everything above is
 * portable C, drivable by the host fake (docs/testing.md §2).
 *
 * Ownership and context are part of the contract:
 *   - The layer owns one RX ring of RADIO_RX_SLOTS frames; the frame pointer
 *     handed to the RX handler is valid only for that call. Ring full -> newest
 *     dropped and counted, never blocking.
 *   - Radio_Tx copies the PSDU into a TX slot and returns immediately; the
 *     caller's buffer is never retained.
 *   - rssi/lqi/channel/timestamp/crcOk are always filled, even when promiscuous
 *     mode is off: the sniffer is a mode of the same RX path. */
#ifndef RADIO_H
#define RADIO_H

#include <stdint.h>

#include "srcmatch.h"
#ifdef RCP_THREAD
#include "mac_sec.h"
#endif

/* 802.15.4 PSDU, 0..127 bytes with FCS included; the RF233 PHR counts the two
 * FCS bytes as well (at86rf233.txt:10984-10989). */
#define RADIO_PSDU_MAX 127u
/* The largest PSDU a caller may hand to Radio_Tx: it carries its own two FCS octets, which the
 * transceiver replaces with the checksum it computes, so the whole PSDU is the bound. */
#define RADIO_TX_PSDU_MAX RADIO_PSDU_MAX

#define RADIO_RX_SLOTS 8u /* bursts during a scan; drop-newest when full */
#define RADIO_TX_SLOTS 2u /* one in flight, one queued */

/* IEEE 802.15.4 2.4 GHz band (at86rf233.txt channel table). */
#define RADIO_CHAN_MIN 11u
#define RADIO_CHAN_MAX 26u

typedef struct
{
    uint8_t psdu[RADIO_PSDU_MAX];
    uint8_t len;        /* bytes in psdu; the FCS is the last two when crcOk */
    int8_t rssi;        /* dBm: -94 + the frame's ED byte from the Frame Buffer (at86rf233 8.5) */
    uint8_t lqi;        /* 0..255, as delivered in the frame buffer's LQI byte */
    uint8_t channel;    /* 11..26 the frame was received on */
    /* Microseconds. On a board with RCP_TX_TIMING: the radio clock (Radio_ClockUs) at the end of the
     * frame's SFD, which is what a host schedules against. Elsewhere: Radio_Now at reception. */
    uint64_t timestamp;
    int crcOk;          /* 0 = FCS failed and was delivered anyway (promiscuous) */
    /* 1 when the Ack this firmware composed for the frame carried Frame Pending set. The
     * transceiver does not acknowledge in the receive mode this firmware uses, so this bit is
     * the only statement of what the peer was told (docs/protocol.md §7.2). */
    int ackedFp;
    /* 1 when that Ack was a secured Enhanced Ack, under this key index and frame counter, which a host
     * must count as used (docs/protocol.md §7.2). */
    int ackedSec;
    uint8_t ackKeyIndex;
    uint32_t ackFrameCounter;
} Radio_Frame;

typedef struct
{
    uint32_t rxFrames;     /* frames delivered to the handler */
    uint32_t rxDropped;    /* frames dropped because the RX ring was full */
    uint32_t rxCrcBad;     /* of the delivered frames, how many had an invalid FCS */
    uint32_t rxUnfiltered; /* bad-FCS frames dropped because the mode filters */
    uint32_t txFrames;
    uint32_t txFailed; /* TX never reached TRX_END within the bound */
    uint32_t edRequests;
    /* Reception moved into the radio interrupt: a frame that found the ring full was left in
     * the transceiver's buffer, and a handler pass that found the IRQ pin high with an empty
     * status register is counted rather than acted on. */
    uint32_t rxRingFull;
    uint32_t isrSpurious;
    /* Acks this firmware composed: how many, how many claimed pending, and how many were
     * abandoned because the turnaround had already passed (docs/ack-pending-frame.md). */
    uint32_t ackSent;
    uint32_t ackFp;
    uint32_t ackLate;
    uint32_t ackNotPllOn; /* the transceiver was not in PLL_ON when the Ack was due */
    uint32_t rxFiltered;  /* dropped by the software third level filter */
} Radio_Counters;

/* ---- lifecycle ---- */
/* Bring the transceiver up over its own SPI (pins, SERCOM, reset, identity
 * proof, TX auto-CRC); leaves it in TRX_OFF. 1 = ok. */
int Radio_Init(void);

/* ---- channel and power ---- */
int Radio_SetChannel(uint8_t channel); /* 11..26 */
int Radio_GetChannel(uint8_t *channel);
/* dBm, snapped down to the RF233's 16 steps (at86rf233.txt Table 9-9:
 * 0x0 = +4 dBm ... 0xF = -17 dBm). */
int Radio_SetPower(int8_t dbm);
int Radio_GetPower(int8_t *dbm);
/* The energy level above which a clear-channel assessment reports the channel busy, in dBm at the
 * transceiver (PHY_CCA_THRESHOLD, docs/protocol.md §10). The RF233 snaps it down to its 2 dB steps
 * from -94 to -64 dBm (CCA_ED_THRES, at86rf233.txt Table 8-23). 1 = applied. */
int Radio_SetCcaThreshold(int8_t dbm);

/* ---- receive ---- */
/* The RF233's three receive configurations, not Spinel's modes (the app maps
 * MAC_PROMISCUOUS_MODE onto them, docs/protocol.md §10). Auto-ACK is
 * off in PAN and ALL, on in FILTERED (a promiscuous receiver "shall not be
 * acknowledged, even if requested", at86rf233.txt:6695-6708). */
enum
{
    RADIO_RX_FILTERED = 0, /* RX_AACK_ON + address filter; bad-FCS not delivered */
    RADIO_RX_PAN = 1,      /* RX_AACK_ON, filter bypassed (AACK_PROM_MODE); bad-FCS
                            * delivered, this driver drops them (rxUnfiltered) */
    RADIO_RX_ALL = 2       /* RX_ON, no filter/AACK: every frame, crcOk relaxed -
                            * the sniffer */
};
/* Applies immediately unless a transmission is in flight, then at its end; the
 * mode is never lost across a TX. */
int Radio_SetRxFilter(int mode);
/* Publish TRX_STATUS and AACK_PROM_MODE as debug records (hal_diag.h). */
void Radio_Witnesses(void);
int Radio_RxEnable(int on);
/* The RF233's address filter (RX_AACK states): three value+enable pairs, so the
 * core owns the policy (docs/architecture.md §2). A pair that is off gets the
 * datasheet's permissive default (PAN and short 0xFFFF, extended all-ones). ext8
 * is EUI-64 wire order (MSB first, as Spinel's 'E' carries it); this layer
 * reverses it into IEEE_ADDR_0..7 (LSB first). */
int Radio_SetPanId(uint16_t panid, int on);
int Radio_SetShortAddr(uint16_t addr, int on);
int Radio_SetExtAddr(const uint8_t ext8[8], int on);

/* The source-match table, which is what the Ack this layer composes reads: an address in
 * the table means the peer has something queued and the Ack says so. The layer copies the
 * table into the copy its interrupt reads, so the caller's buffer is never retained and an
 * update never delays the radio. 1 = stored. */
int Radio_SetSrcMatch(const SrcMatch_Table *table);

/* ---- transmit ---- */
/* cca != 0 requests clear-channel assessment. token comes back to the TX-done
 * handler untouched; psdu is copied; 1 = queued (not sent). maxRetries is docs/protocol.md §7.1's
 * field, clamped to the register's four bits.
 *
 * An acknowledgment-requesting frame (FCF bit 5, rcp/mac.c) uses the hardware
 * ARET engine (CSMA-CA, ACK wait, TRAC_STATUS - the only way to tell OK from
 * NO_ACK); ARET always does one CCA even with enableCSMACA = 0, the datasheet
 * having no CSMA-less mode. A non-ACK frame keeps PLL_ON with a manual CCA. */
int Radio_Tx(const uint8_t *psdu, uint8_t len, int8_t dbm, int cca, int maxRetries, void *token);

/* The clock a host reads through RCP_TIMESTAMP: monotonic microseconds, never wrapping in practice.
 * Keep calling Radio_Poll, which keeps it extended. */
uint64_t Radio_ClockUs(void);

#ifdef RCP_TX_TIMING
/* Radio_Tx at a chosen moment, sfdEndUs being the low half of Radio_ClockUs when the end of the SFD
 * is at the antenna. The frame gets one attempt, because a retry would miss the window: a window
 * already gone, or a channel found busy, completes as RADIO_TX_CCA_FAIL, the one failure a host's CSL
 * scheduler retries rather than asserts on (csl_tx_scheduler.cpp HandleFrameTxDone). */
int Radio_TxAt(const uint8_t *psdu, uint8_t len, int8_t dbm, int cca, uint32_t sfdEndUs, void *token);

/* The Ack of the transmission just completed, valid inside the TX-done handler; 0 when there is none. */
const Radio_Frame *Radio_TxAck(void);

/* From here on, answer a version 2 frame with an Enhanced Ack, secured from this state; 0 stops it. The
 * state stays the caller's and is read in the radio interrupt (rcp/mac_sec.h). */
int Radio_SetMacSec(MacSec_State *state);
#endif

/* ---- energy detection ----
 * One measurement at a time, on the channel the transceiver already holds. A measurement is
 * *requested* by a write access to PHY_ED_LEVEL and completes with IRQ_4 (CCA_ED_DONE); the
 * result maps as dBm = -94 + level in 1 dB steps, and the request is only legal in RX_ON or
 * BUSY_RX. Trigger, completion, the sentinel that proves a measurement ran, and the trap below
 * are measured on the board (docs/hardware_samr21.md §6): the level in the register survives a retune,
 * so a read belongs only to the channel whose own request completed. */
enum
{
    RADIO_ED_IDLE = 0,
    RADIO_ED_MEASURING,
    RADIO_ED_READY,
    RADIO_ED_FAILED /* refused, or a completion with no measurement behind it */
};

/* Request one measurement. 0 = refused: the transceiver is not in a state a manual measurement
 * is legal in, or it is transmitting - a scan yields to a transmit, it does not interrupt one.
 * There is no channel argument on purpose: the request measures the air the transceiver is on. */
int Radio_StartEd(void);

/* Take the completed measurement and consume it. Three answers, because "not yet" and "no" are
 * different facts and a caller must not react to them the same way:
 *   1  *dbm is this request's result. *fresh = 0 when the completion landed while the transceiver was
 *      mid-reception, where an energy value stops being refreshed and the number belongs to the frame
 *      in the air rather than to this request.
 *   0  the measurement is still in flight: ask again later, nothing went wrong.
 *  -1  there is no result to take - the request was refused, or it completed with nothing behind it.
 *      The reason is on the record (docs/hardware_samr21.md §6). */
int Radio_TakeEd(int8_t *dbm, int *fresh);

/* Give up a measurement in flight: the transceiver is needed for something else. */
void Radio_AbortEd(void);
/* ---- housekeeping (thread context) ---- */
void Radio_SetRxHandler(void (*fn)(const Radio_Frame *frame));
/* status is a RADIO_TX_* value. framePending is the received ACK's Frame Pending subfield
 * (0/1): 1 means the acknowledging peer has data waiting, which a host uses to decide
 * whether a sleepy child must be polled. It is 0 whenever no acknowledgment was received
 * (NO_ACK, CCA fail, timeout, or a frame that did not request one). */
void Radio_SetTxDoneHandler(void (*fn)(void *token, int status, int framePending));
/* Read a finished RX frame into the ring and deliver it, report TX completion;
 * once per main-loop pass. */
void Radio_Poll(void);
void Radio_GetCounters(Radio_Counters *out);

/* Monotonic microseconds, 1 ms resolution (from the millisecond tick;
 * docs/protocol.md §7.2). */
uint32_t Radio_Now(void);

/* TX-done status values handed to the TX-done handler. */
enum
{
    RADIO_TX_OK = 0,
    RADIO_TX_TIMEOUT = 1,  /* never reached TRX_END inside the bound */
    RADIO_TX_CCA_FAIL = 2, /* CCA reported a busy channel (cca was requested) */
    RADIO_TX_NO_ACK = 3    /* ARET: the frame, after its retries, was not acknowledged */
};

#endif
