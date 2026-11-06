/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* scan.c — see scan.h. One dwell on one channel: retune, settle, request, confirm, read. */
#include "scan.h"

/* The mask resolves to exactly one usable channel: one bit set, inside 11..26. Upstream's rule
 * (`HasOnly1BitSet` on a mask built from the channel list), and the reason a whole-band default
 * mask cannot start a scan. */
static int singleChannelOfMask(uint32_t mask, uint8_t *channel)
{
    uint8_t bit;

    if (channel == 0 || mask == 0u || (mask & (mask - 1u)) != 0u)
    {
        return 0;
    }

    for (bit = 0u; bit < 32u; ++bit)
    {
        if ((mask & (1uL << bit)) != 0u)
        {
            break;
        }
    }

    if (bit < SCAN_CHAN_MIN || bit > SCAN_CHAN_MAX)
    {
        return 0;
    }

    *channel = bit;
    return 1;
}

/* Samples for a dwell. The machine issues at most one request per main-loop pass and a pass is
 * ~1 ms, so the sample count is the dwell in milliseconds; the hardware floor (135..180 us per
 * measurement) is already below that. Never zero: a dwell asks for at least one measurement. */
/* The count guard, not the timer: how many requests one dwell may make whatever the loop does with
 * its time. The timer is `endMs`. */
static uint16_t sampleCapForDwell(uint16_t dwellMs)
{
    uint32_t cap = (uint32_t)dwellMs / SCAN_SAMPLE_MS;

    if (cap > (uint32_t)SCAN_MAX_SAMPLES)
        cap = (uint32_t)SCAN_MAX_SAMPLES;

    return (cap == 0u) ? 1u : (uint16_t)cap;
}

static void scanResetRun(Scan_State *s)
{
    s->pending = SCAN_ACTION_NONE;
    s->resumePhase = SCAN_PHASE_IDLE;
    s->channel = 0u;
    s->samplesLeft = 0u;
    s->samplesRead = 0u;
    s->samplesStale = 0u;
    s->failStreak = 0u;
    s->waitStreak = 0u;
    s->limitReached = 0u;
    s->maxDbm = (int8_t)SCAN_ED_INVALID;
    s->measured = 0;
    s->edReported = 0;
    s->edFresh = 0;
    s->edDbm = (int8_t)SCAN_ED_INVALID;
    s->result.channel = 0u;
    s->result.dbm = (int8_t)SCAN_ED_INVALID;
    s->resultReady = 0;
    s->deadlineMs = 0u;
}

void Scan_Init(Scan_State *s)
{
    if (s == 0)
    {
        return;
    }

    s->phase = SCAN_PHASE_IDLE;
    s->mask = SCAN_CHAN_MASK_ALL;
    s->periodMs = SCAN_PERIOD_DEFAULT_MS;
    s->dwellMs = SCAN_PERIOD_DEFAULT_MS;
    scanResetRun(s);
}

void Scan_Configure(Scan_State *s, uint32_t mask, uint16_t periodMs)
{
    if (s == 0)
    {
        return;
    }

    s->mask = mask;
    s->periodMs = periodMs;
    s->dwellMs = (periodMs > (uint16_t)SCAN_PERIOD_MAX_MS) ? (uint16_t)SCAN_PERIOD_MAX_MS : periodMs;
}

uint32_t Scan_MaskSingleChannel(uint32_t mask)
{
    uint8_t channel = 0u;

    return singleChannelOfMask(mask, &channel) ? (1uL << channel) : 0u;
}

int Scan_Start(Scan_State *s, uint32_t nowMs)
{
    uint8_t channel = 0u;

    if (s == 0 || s->phase != SCAN_PHASE_IDLE)
    {
        return 0; /* one dwell at a time; a second request leaves the first alone */
    }

    if (!singleChannelOfMask(s->mask, &channel))
    {
        return 0;
    }

    scanResetRun(s);
    s->channel = channel;
    s->samplesLeft = sampleCapForDwell(s->dwellMs);
    s->endMs = nowMs + s->dwellMs; /* a period is wall time, which is what a host waits for */
    s->phase = SCAN_PHASE_TUNE;
    return 1;
}

int Scan_OwnsRadio(const Scan_State *s)
{
    return (s != 0 && s->phase != SCAN_PHASE_IDLE) ? 1 : 0;
}

int Scan_ReportedState(const Scan_State *s)
{
    if (s == 0 || s->phase == SCAN_PHASE_IDLE)
    {
        return 0; /* SPINEL_SCAN_STATE_IDLE */
    }

    return 2; /* SPINEL_SCAN_STATE_ENERGY: all a raw RCP can be doing */
}

/* Hand an action out and start the clock on answering it. */
static Scan_Action handOut(Scan_State *s, Scan_Action action, uint32_t nowMs)
{
    s->pending = action;
    s->pendingDeadlineMs = nowMs + SCAN_PENDING_TIMEOUT_MS;
    return action;
}

Scan_Action Scan_Advance(Scan_State *s, uint32_t nowMs)
{
    if (s == 0)
    {
        return SCAN_ACTION_NONE;
    }

    if (s->pending != SCAN_ACTION_NONE)
    {
        /* An action that is never answered is a glue bug, and it must not cost the host its scan:
         * abandoned as refused, on the same path a refusal takes, and the dwell moves on. */
        if ((int32_t)(nowMs - s->pendingDeadlineMs) < 0)
        {
            return SCAN_ACTION_NONE;
        }
        Scan_ActionDone(s, s->pending, 0, nowMs);
    }

    switch (s->phase)
    {
    case SCAN_PHASE_TUNE:
        return handOut(s, SCAN_ACTION_SET_CHANNEL, nowMs);

    case SCAN_PHASE_SETTLE:
        /* After the retune, never before it: the value in the energy register survives a channel
         * change, so an early read answers with the channel the scan left. */
        if ((int32_t)(nowMs - s->deadlineMs) < 0)
        {
            return SCAN_ACTION_NONE;
        }
        return handOut(s, SCAN_ACTION_START_ED, nowMs);

    case SCAN_PHASE_MEASURE:
        if ((int32_t)(nowMs - s->endMs) >= 0)
        {
            /* The period is used up: ask for no further sample, restore and report. */
            s->phase = SCAN_PHASE_RESTORE;
            return handOut(s, SCAN_ACTION_RESTORE_RX, nowMs);
        }
        if ((int32_t)(nowMs - s->deadlineMs) < 0)
        {
            return SCAN_ACTION_NONE; /* spacing after a refusal, not a retry every pass */
        }
        return handOut(s, SCAN_ACTION_START_ED, nowMs);

    case SCAN_PHASE_READ:
        /* Confirm, then read: a request that has not completed leaves the previous level in the
         * register, which is exactly the value that must not be reported as this channel's. */
        if ((int32_t)(nowMs - s->endMs) >= 0)
        {
            s->phase = SCAN_PHASE_RESTORE;
            return handOut(s, SCAN_ACTION_RESTORE_RX, nowMs);
        }
        if ((int32_t)(nowMs - s->deadlineMs) < 0)
        {
            return SCAN_ACTION_NONE;
        }
        return handOut(s, SCAN_ACTION_READ_ED, nowMs);

    case SCAN_PHASE_RESTORE:
        return handOut(s, SCAN_ACTION_RESTORE_RX, nowMs);

    case SCAN_PHASE_EMIT:
        s->result.channel = s->channel;
        s->result.dbm = (s->measured != 0) ? s->maxDbm : (int8_t)SCAN_ED_INVALID;
        s->resultReady = 1;
        return handOut(s, SCAN_ACTION_EMIT, nowMs);

    case SCAN_PHASE_DONE:
        return handOut(s, SCAN_ACTION_DONE, nowMs);

    case SCAN_PHASE_SUSPEND:
    case SCAN_PHASE_IDLE:
    default:
        return SCAN_ACTION_NONE;
    }
}

/* Account for one read. A reading is usable only when the app vouches for it (`fresh`) and it is
 * not the invalid sentinel — upstream skips unusable samples before taking the maximum, so one
 * bad reading cannot poison a whole dwell. */
static void accountSample(Scan_State *s, int ok, uint32_t nowMs)
{
    int usable;

    usable = (ok != 0 && s->edReported != 0 && s->edFresh != 0 && (int)s->edDbm != SCAN_ED_INVALID)
                 ? 1
                 : 0;

    if (usable != 0)
    {
        if (s->measured == 0 || s->edDbm > s->maxDbm)
        {
            s->maxDbm = s->edDbm;
        }
        s->measured = 1;
        s->samplesRead++;
        s->failStreak = 0u;
        s->waitStreak = 0u;
    }
    else
    {
        s->samplesStale++;
        if (ok == 0)
        {
            s->failStreak++;
        }
    }

    s->edReported = 0;
    s->edFresh = 0;

    if (s->samplesLeft != 0u)
    {
        s->samplesLeft--;
    }

    if (s->failStreak >= SCAN_FAIL_LIMIT)
    {
        s->limitReached = 1u;
    }

    /* Time is what a period means. The request budget only keeps a very fast loop from turning a
     * dwell into a register benchmark. */
    if (s->samplesLeft == 0u || s->limitReached != 0u || (int32_t)(nowMs - s->endMs) >= 0)
    {
        s->phase = SCAN_PHASE_RESTORE;
    }
    else
    {
        s->phase = SCAN_PHASE_MEASURE;
        s->deadlineMs = nowMs + SCAN_SAMPLE_MS; /* the minimum spacing between two requests */
    }
}

void Scan_ActionDone(Scan_State *s, Scan_Action action, int ok, uint32_t nowMs)
{
    if (s == 0 || s->pending != action)
    {
        return; /* an answer to an action that was never handed out changes nothing */
    }

    s->pending = SCAN_ACTION_NONE;

    switch (action)
    {
    case SCAN_ACTION_SET_CHANNEL:
        if (ok == 0)
        {
            /* Refused retune or arming: do not dwell on a channel the radio would not move to.
             * Restore, report nothing measured, finish. */
            s->failStreak = SCAN_FAIL_LIMIT;
            s->limitReached = 1u;
            s->phase = SCAN_PHASE_RESTORE;
            break;
        }
        s->deadlineMs = nowMs + SCAN_SETTLE_MS;
        s->phase = SCAN_PHASE_SETTLE;
        break;

    case SCAN_ACTION_START_ED:
        if (ok != SCAN_ANSWER_DONE)
        {
            /* The state was not one a measurement is legal in, or the request failed. Wait out
             * the retry rather than hammer it, and give up after the limit. */
            s->failStreak++;
            s->samplesStale++; /* a refusal is a sample this dwell did not get: counted, not hidden */
            if (s->failStreak >= SCAN_FAIL_LIMIT)
            {
                s->limitReached = 1u;
                s->phase = SCAN_PHASE_RESTORE;
                break;
            }
            s->phase = SCAN_PHASE_MEASURE;
            s->deadlineMs = nowMs + SCAN_READ_WAIT_MS;
            break;
        }
        s->phase = SCAN_PHASE_READ;
        s->deadlineMs = nowMs + SCAN_READ_WAIT_MS;
        break;

    case SCAN_ACTION_READ_ED:
        if (ok == SCAN_ANSWER_WAITING)
        {
            /* Still measuring: nothing was refused and nothing was learned. Stay in READ and ask
             * again, and drop the report the glue did not make. */
            s->edReported = 0;
            s->edFresh = 0;
            s->waitStreak++;

            if (s->waitStreak >= SCAN_WAIT_LIMIT)
            {
                /* A measurement that never came. Not a refusal - but the dwell stops waiting for it,
                 * and the reading it did not get is counted rather than hidden. */
                s->limitReached = 1u;
                s->samplesStale++;
                s->phase = SCAN_PHASE_RESTORE;
                break;
            }

            s->phase = SCAN_PHASE_READ;
            s->deadlineMs = nowMs + SCAN_READ_WAIT_MS;
            break;
        }
        accountSample(s, (ok == SCAN_ANSWER_DONE) ? 1 : 0, nowMs);
        break;

    case SCAN_ACTION_RESTORE_RX:
        /* A refused restore is recorded and passed over: the dwell is over either way, and the
         * app's own shadow re-applies the host's state on its next pass. */
        s->phase = SCAN_PHASE_EMIT;
        break;

    case SCAN_ACTION_EMIT:
        s->phase = SCAN_PHASE_DONE;
        break;

    case SCAN_ACTION_DONE:
        s->phase = SCAN_PHASE_IDLE;
        s->resultReady = 0;
        s->samplesLeft = 0u;
        break;

    case SCAN_ACTION_NONE:
    default:
        break;
    }
}

void Scan_ReportEd(Scan_State *s, const int8_t dbm, const int fresh)
{
    if (s == 0)
    {
        return;
    }

    s->edDbm = dbm;
    s->edFresh = (fresh != 0) ? 1 : 0;
    s->edReported = 1;
}

void Scan_Suspend(Scan_State *s)
{
    if (s == 0 || s->phase == SCAN_PHASE_IDLE || s->phase == SCAN_PHASE_SUSPEND)
    {
        return;
    }

    s->resumePhase = s->phase;
    s->phase = SCAN_PHASE_SUSPEND;
    s->pending = SCAN_ACTION_NONE;
    s->edReported = 0;
    s->edFresh = 0;
}

void Scan_Resume(Scan_State *s)
{
    if (s == 0 || s->phase != SCAN_PHASE_SUSPEND)
    {
        return;
    }

    /* A finished transmission left the transceiver in the mode its sender asked for, not the mode
     * a measurement needs, and its channel assessment left a level in the register that belongs to
     * that transaction. Re-tune and re-arm; the samples already taken still count. */
    s->phase = SCAN_PHASE_TUNE;
    s->pending = SCAN_ACTION_NONE;
    s->edReported = 0;
    s->edFresh = 0;
}

int Scan_End(Scan_State *s, Scan_Result *out)
{
    int wasRunning;

    if (s == 0)
    {
        return 0;
    }

    wasRunning = (s->phase != SCAN_PHASE_IDLE) ? 1 : 0;

    if (wasRunning != 0)
    {
        if (out != 0)
        {
            out->channel = s->channel;
            out->dbm = (s->measured != 0) ? s->maxDbm : (int8_t)SCAN_ED_INVALID;
        }
        /* The mask and the period survive an interrupted dwell — upstream keeps them too — but the
         * run does not: an aborted scan starts the next one from a clean machine. */
        s->phase = SCAN_PHASE_IDLE;
        scanResetRun(s);
    }

    return wasRunning;
}

int Scan_TakeResult(Scan_State *s, Scan_Result *out)
{
    if (s == 0 || out == 0 || s->resultReady == 0)
    {
        return 0;
    }

    /* Field by field, not `*out = s->result`: GCC turns a struct copy into libc memcpy, and this
     * image is -nostdlib (coding-rules.md §7) - the target link is where that shows up. */
    out->channel = s->result.channel;
    out->dbm = s->result.dbm;
    s->resultReady = 0;
    return 1;
}

void Scan_Counters(const Scan_State *s, uint16_t *samplesRead, uint16_t *samplesStale)
{
    if (s == 0)
    {
        return;
    }

    if (samplesRead != 0)
    {
        *samplesRead = s->samplesRead;
    }
    if (samplesStale != 0)
    {
        *samplesStale = s->samplesStale;
    }
}
