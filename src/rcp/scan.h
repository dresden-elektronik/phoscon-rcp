/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* scan.h — the dwell machine for one energy scan of one channel.
 *
 * A pure core module (docs/coding-rules.md §4, docs/architecture.md §2): it owns no clock, reads no register
 * and calls nothing outside itself. Time arrives as `nowMs` from the caller, its state is
 * caller-owned, and what it produces is a request for **one action per main-loop pass** plus the
 * number it wants read back. `src/app/rcp_app.c` is the only place allowed to touch `Radio_*`.
 *
 * The contract is upstream's link-raw energy scan: one channel per request, a dwell of
 * `MAC_SCAN_PERIOD` milliseconds, one result reported as `MAC_ENERGY_SCAN_RESULT`, and a completion
 * that puts the host's own receive state back before anything is emitted. The measurements behind
 * the ordering are in `docs/hardware_samr21.md` §6 — a measurement is *requested* by a write access to the
 * energy register, completes with an interrupt, takes 135..180 us, and its value **survives a
 * retune**, so a level is never read before a request on this channel has been confirmed.
 *
 *   IDLE ─Start─► TUNE ─► SETTLE ─► MEASURE ─► READ ─┬─ samples left ─► MEASURE
 *                          ▲                          │
 *                          └── Resume after a TX     └─ none left ─► RESTORE ─► EMIT ─► DONE ─► IDLE
 *
 * TUNE retunes and arms the state a measurement is legal in; SETTLE is the retune's settle time,
 * not politeness. A transmission borrows the transceiver: SUSPEND stops issuing actions, RESUME
 * re-runs TUNE, because a TX leaves the chip in the mode its sender asked for rather than the mode
 * a measurement needs. A refused action is counted and worked around — the scan always restores,
 * emits and finishes, so nothing can wedge the machine the way upstream's disarm does. */
#ifndef SCAN_H
#define SCAN_H

#include <stdint.h>

/* A channel mask is 32 bits with bit N = channel N — upstream's internal encoding. Only 11..26
 * mean anything on this radio; `MAC_SCAN_MASK` carries a *list of channel numbers* and the props
 * layer builds the mask. */
#define SCAN_CHAN_MIN 11u
#define SCAN_CHAN_MAX 26u
#define SCAN_CHAN_MASK_ALL 0x007FFF800u /* bits 11..26 set: upstream's fresh default, whole band */
#define SCAN_CHAN_COUNT 16u

/* Upstream's fresh-coprocessor defaults, copied deliberately: a host that writes nothing but the
 * scan state meets the same refusal here that a stock OpenThread RCP gives it. */
#define SCAN_PERIOD_DEFAULT_MS 200u

/* `MAC_SCAN_PERIOD` is stored verbatim; the dwell is clamped to this. The reason is host timeouts
 * and how long one channel may hold the transceiver, not silicon — the machine never blocks, so
 * the watchdog is not what bounds it. GET reports what was written, and this is the bound the dwell
 * itself obeys. */
#define SCAN_PERIOD_MAX_MS 1000u

/* Retune settle, and the wait between a confirmed request and the read. Both are one pass, which is
 * already several times the hardware's own 180 us measurement (docs/hardware_samr21.md §6). They stay
 * separate names because they guard different things: a stale channel, and a measurement still in
 * progress. */
#define SCAN_SETTLE_MS 1u
#define SCAN_READ_WAIT_MS 1u

/* The dwell is bounded by **time**: `MAC_SCAN_PERIOD` is how long a channel is measured, so what ends
 * a dwell is `nowMs` reaching the period the host asked for. `SCAN_SAMPLE_MS` is the minimum spacing
 * between two requests, so a quick loop cannot hammer the radio, and `SCAN_MAX_SAMPLES` is how many
 * requests one dwell may make at all - the guard that keeps a dwell from becoming a register benchmark
 * on a loop faster than a millisecond. Measured on the board before this was fixed: one request costs
 * about five main-loop passes, so a dwell counted in samples ran roughly five times as long as the
 * period its host asked for. */
#define SCAN_SAMPLE_MS 1u
#define SCAN_MAX_SAMPLES 2000u

/* "Nothing usable was measured", in dBm: upstream's `OT_RADIO_RSSI_INVALID`. It is never mixed
 * into the maximum, so a single bad reading cannot poison a dwell. */
#define SCAN_ED_INVALID 127

/* Consecutive refusals before the dwell gives up. Without it, a radio that refuses the request
 * would be asked again every millisecond for the whole dwell. */
#define SCAN_FAIL_LIMIT 3u

/* Consecutive "not ready yet" answers before the dwell gives up on that reading. A wait is not a
 * failure, but a radio that only ever says "not ready" must not hold a dwell open for longer than the
 * period asked for either. Eight is a whole confirmation window at the HAL's own bound. */
#define SCAN_WAIT_LIMIT 8u

/* An action the glue never answers. Handing an action out and getting no reply is a glue bug, and
 * the machine must survive it: the action is abandoned as refused rather than waited for forever.
 * 8 ms is generous against a register access of a few microseconds. */
#define SCAN_PENDING_TIMEOUT_MS 8u

typedef enum
{
    SCAN_PHASE_IDLE = 0,
    SCAN_PHASE_TUNE,
    SCAN_PHASE_SETTLE,
    SCAN_PHASE_MEASURE,
    SCAN_PHASE_READ,
    SCAN_PHASE_RESTORE,
    SCAN_PHASE_EMIT,
    SCAN_PHASE_DONE,
    SCAN_PHASE_SUSPEND
} Scan_Phase;

typedef enum
{
    SCAN_ACTION_NONE = 0,
    SCAN_ACTION_SET_CHANNEL, /* retune to the scan channel and arm the state a measurement needs */
    SCAN_ACTION_START_ED,    /* request one measurement, on the channel it is already holding */
    SCAN_ACTION_READ_ED,     /* read the level back, after that request was confirmed */
    SCAN_ACTION_RESTORE_RX,  /* put the host's channel and receive state back */
    SCAN_ACTION_EMIT,        /* the result is ready for MAC_ENERGY_SCAN_RESULT */
    SCAN_ACTION_DONE         /* the scan is over: MAC_SCAN_STATE reads idle */
} Scan_Action;

typedef struct
{
    uint8_t channel; /* the channel this level belongs to */
    int8_t dbm;      /* the maximum of the usable samples, or SCAN_ED_INVALID */
} Scan_Result;

typedef struct
{
    Scan_Phase phase;
    Scan_Phase resumePhase;  /* where a suspension was interrupted; unused until it is needed */
    Scan_Action pending;     /* the action handed out and not yet answered */
    uint32_t pendingDeadlineMs; /* after this, an unanswered action is abandoned as refused */

    uint32_t mask;
    uint16_t periodMs;   /* as written by the host */
    uint16_t dwellMs;    /* as this scan actually dwells */
    uint8_t channel;     /* the one channel a started scan measures */

    uint32_t endMs;        /* the dwell's wall limit, modular only: (int32_t)(nowMs - endMs) >= 0 */
    uint16_t samplesLeft;  /* requests this dwell may still make - the count guard, not the timer */
    uint16_t samplesRead;  /* readings that reached the maximum */
    uint16_t samplesStale; /* refused, invalid or unconfirmed readings, counted not believed */
    uint8_t failStreak;
    uint8_t waitStreak;    /* consecutive answers of "not ready yet" */
    uint8_t limitReached;  /* 1 when refusals ended the dwell early */

    int8_t maxDbm;
    int measured;

    int edReported; /* a reading is waiting to be accounted for */
    int edFresh;    /* the app says it came from the confirmed request, not from the last frame */
    int8_t edDbm;

    Scan_Result result;
    int resultReady;

    uint32_t deadlineMs; /* modular only: (int32_t)(nowMs - deadlineMs) >= 0 */
} Scan_State;

/* Fresh state, carrying upstream's defaults. Also the reset of a running scan. */
void Scan_Init(Scan_State *s);

/* Store what the host wrote to `MAC_SCAN_MASK` (already a mask) and `MAC_SCAN_PERIOD`. Storing is
 * not starting: a whole-band mask stores fine and is refused only when a scan is asked to begin. */
void Scan_Configure(Scan_State *s, uint32_t mask, uint16_t periodMs);

/* The mask as a startable one: the mask itself when it resolves to exactly one channel inside
 * 11..26, otherwise 0. */
uint32_t Scan_MaskSingleChannel(uint32_t mask);

/* Begin one dwell. It runs until `nowMs` has advanced the stored period from this call - bounded by
 * SCAN_PERIOD_MAX_MS, by SCAN_MAX_SAMPLES requests and by the machine's own refusal limit - so a
 * host's period is the time it waits. 1 = started; 0 = refused, because a scan is already running or
 * the mask is not a single usable channel. */
int Scan_Start(Scan_State *s, uint32_t nowMs);

/* 1 while the machine holds the transceiver: the app must not re-arm it underneath. */
int Scan_OwnsRadio(const Scan_State *s);

/* What `MAC_SCAN_STATE` reads right now: 0 idle, 2 energy — the only two values a raw RCP shows. */
int Scan_ReportedState(const Scan_State *s);

/* One main-loop pass. Returns the action to perform, or SCAN_ACTION_NONE. An unanswered action is
 * never repeated and nothing overtakes it. */
Scan_Action Scan_Advance(Scan_State *s, uint32_t nowMs);

/* The outcome of the action handed out, and the machine reacts to the difference:
 *   SCAN_ANSWER_DONE     the radio did it.
 *   SCAN_ANSWER_REFUSED  the radio said no. Counted as a sample this dwell did not get, and three
 *                        of them end the dwell rather than retrying for the whole period.
 *   SCAN_ANSWER_WAITING  the radio has not produced the thing yet and nothing went wrong - a
 *                        measurement still in flight. Re-ask next pass, without charging a failure
 *                        for the wait and without spending a sample. Measured on the board: a
 *                        confirmation that was answered as a refusal ended dwells three readings in,
 *                        on a channel with nothing wrong with it.
 * `SCAN_READ_WAIT_MS` is the re-ask interval. */
#define SCAN_ANSWER_REFUSED 0
#define SCAN_ANSWER_DONE 1
#define SCAN_ANSWER_WAITING 2

void Scan_ActionDone(Scan_State *s, Scan_Action action, int ok, uint32_t nowMs);

/* The level a read produced. `fresh` = 0 when it may be a leftover — the request was not confirmed,
 * or the transceiver was mid-reception, where an energy value stops being refreshed
 * (docs/hardware_samr21.md §6). Such a reading never reaches the maximum. */
void Scan_ReportEd(Scan_State *s, const int8_t dbm, const int fresh);

/* A transmission took the transceiver; issue nothing until it is back. */
void Scan_Suspend(Scan_State *s);
/* It is back: re-tune and re-arm, keeping the samples already taken. */
void Scan_Resume(Scan_State *s);

/* End a dwell early — a disarm or a reset while scanning. Fills `out` with what the scan had
 * (SCAN_ED_INVALID when nothing usable was measured) and reports 1 if a scan really was running.
 * Whether that result is emitted is the app's decision, not this module's. */
int Scan_End(Scan_State *s, Scan_Result *out);

/* Hand the finished result to the app. 1 = taken; a result is emitted once per scan. */
int Scan_TakeResult(Scan_State *s, Scan_Result *out);

/* Accounting, for the diagnostic side: usable readings and refused/unusable readings. */
void Scan_Counters(const Scan_State *s, uint16_t *samplesRead, uint16_t *samplesStale);

#endif
