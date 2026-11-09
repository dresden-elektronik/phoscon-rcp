/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* fake_radio.c — hal/radio.h for a host suite, keeping the contract's one promise the glue's health
 * rests on: every transmission it accepts completes, in Radio_Poll. Radio_Tx only queues the frame, as
 * the AT86RF233 and ATmega256RFR2 drivers do, and the next Radio_Poll puts it on the air on whatever
 * channel the radio holds then (fakeRadioTxChannel). A frame completes fakeRadioTxMs after it starts;
 * a timed frame starts at its moment on the radio's clock, and one whose moment is already gone
 * completes at once as RADIO_TX_CCA_FAIL. The clock is the suite's millisecond tick. */
#include "fakes.h"

#include "hal.h"
#include "radio.h"

int fakeRadioInitFails;
uint32_t fakeRadioTxMs = 5u;
uint32_t fakeRadioTxCount;
uint32_t fakeRadioLeftOpen;
uint8_t fakeRadioChannel = RADIO_CHAN_MIN;
uint8_t fakeRadioTxChannel;
uint32_t fakeRadioSrcMatchPushes;
int fakeRadioAddrFails;
int fakeRadioPollAgain;
uint8_t fakeRadioSrcMatchEnabled;
uint8_t fakeRadioSrcMatchShorts;

static void (*fakeRxHandler)(const Radio_Frame *frame);
static void (*fakeTxDoneHandler)(void *token, int status, int framePending);
static int fakeTxOpen;
static int fakeTxQueued;
static void *fakeTxToken;
static int fakeTxStatus;
static uint32_t fakeTxDoneAtMs;

void Fake_RadioReset(void)
{
    fakeRadioInitFails = 0;
    fakeRadioTxMs = 5u;
    fakeRadioTxCount = 0u;
    fakeRadioLeftOpen = 0u;
    fakeRadioTxChannel = 0u; /* the channel stays: the glue's shadow of it survives RcpApp_Init too */
    fakeTxOpen = 0;
    fakeTxQueued = 0;
    fakeRadioAddrFails = 0;
    fakeRadioPollAgain = 0;
}

int Radio_Init(void)
{
    return fakeRadioInitFails ? 0 : 1;
}

int Radio_SetChannel(uint8_t channel)
{
    if (channel < RADIO_CHAN_MIN || channel > RADIO_CHAN_MAX)
        return 0;

    fakeRadioChannel = channel;
    return 1;
}

int Radio_GetChannel(uint8_t *channel)
{
    *channel = fakeRadioChannel;
    return 1;
}

int Radio_SetPower(int8_t dbm)
{
    (void)dbm;
    return 1;
}

int Radio_GetPower(int8_t *dbm)
{
    *dbm = 0;
    return 1;
}

int Radio_SetCcaThreshold(int8_t dbm)
{
    (void)dbm;
    return 1;
}

int Radio_SetRxFilter(int mode)
{
    (void)mode;
    return 1;
}

void Radio_Witnesses(void)
{
}

int Radio_RxEnable(int on)
{
    (void)on;
    return 1;
}

int Radio_SetPanId(uint16_t panid, int on)
{
    (void)panid;
    (void)on;
    return fakeRadioAddrFails ? 0 : 1;
}

int Radio_SetShortAddr(uint16_t addr, int on)
{
    (void)addr;
    (void)on;
    return 1;
}

int Radio_SetExtAddr(const uint8_t ext8[8], int on)
{
    (void)ext8;
    (void)on;
    return 1;
}

int Radio_SetSrcMatch(const SrcMatch_Table *table)
{
    fakeRadioSrcMatchPushes++;
    fakeRadioSrcMatchEnabled = table->enabled;
    fakeRadioSrcMatchShorts = table->shortCount;
    return 1;
}

static int fakeTxStart(void *token, uint32_t startMs, int status)
{
    if (fakeTxOpen)
        return 0;

    fakeTxOpen = 1;
    fakeTxQueued = 1;
    fakeTxToken = token;
    fakeTxStatus = status;
    fakeTxDoneAtMs = startMs + fakeRadioTxMs;
    fakeRadioTxCount++;
    return 1;
}

int Radio_Tx(const uint8_t *psdu, uint8_t len, int8_t dbm, int cca, int maxRetries, void *token)
{
    (void)dbm;
    (void)cca;
    (void)maxRetries;

    if (psdu == 0 || len < 3u || len > RADIO_TX_PSDU_MAX)
        return 0;

    return fakeTxStart(token, fakeNowMs, RADIO_TX_OK);
}

uint64_t Radio_ClockUs(void)
{
    return (uint64_t)fakeNowMs * 1000u;
}

uint32_t Radio_Now(void)
{
    return fakeNowMs * 1000u;
}

int Radio_TxAt(const uint8_t *psdu, uint8_t len, int8_t dbm, int cca, uint32_t sfdEndUs, void *token)
{
    int32_t leadUs = (int32_t)(sfdEndUs - (uint32_t)Radio_ClockUs());

    (void)dbm;
    (void)cca;

    if (psdu == 0 || len < 3u || len > RADIO_TX_PSDU_MAX)
        return 0;

    if (leadUs <= 0)
        return fakeTxStart(token, fakeNowMs - fakeRadioTxMs, RADIO_TX_CCA_FAIL);

    return fakeTxStart(token, fakeNowMs + (uint32_t)leadUs / 1000u, RADIO_TX_OK);
}

const Radio_Frame *Radio_TxAck(void)
{
    return 0;
}

int Radio_SetMacSec(MacSec_State *state)
{
    (void)state;
    return 1;
}

int Radio_StartEd(void)
{
    return fakeTxOpen ? 0 : 1;
}

int Radio_TakeEd(int8_t *dbm, int *fresh)
{
    if (dbm != 0)
        *dbm = -90;

    if (fresh != 0)
        *fresh = 1;

    return 1;
}

void Radio_AbortEd(void)
{
}

void Radio_SetRxHandler(void (*fn)(const Radio_Frame *frame))
{
    fakeRxHandler = fn;
}

void Radio_SetTxDoneHandler(void (*fn)(void *token, int status, int framePending))
{
    fakeTxDoneHandler = fn;
}

void Radio_Poll(void)
{
    fakeRadioLeftOpen = (uint32_t)fakeTxOpen;

    if (fakeTxQueued)
    {
        fakeTxQueued = 0;
        fakeRadioTxChannel = fakeRadioChannel;
    }

    if (!fakeTxOpen || (int32_t)(fakeNowMs - fakeTxDoneAtMs) < 0)
        return;

    fakeTxOpen = 0;
    fakeRadioLeftOpen = 0u;

    if (fakeTxDoneHandler != 0)
        fakeTxDoneHandler(fakeTxToken, fakeTxStatus, 0);
}

int Radio_PollAgain(void)
{
    return fakeRadioPollAgain;
}

void Radio_GetCounters(Radio_Counters *out)
{
    uint8_t *p = (uint8_t *)out;
    uint32_t i;

    for (i = 0u; i < sizeof(*out); ++i)
        p[i] = 0u;
}
