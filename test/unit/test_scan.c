/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* test_scan.c — Scan_* suite (docs/testing.md §2).
 *
 * The pure core needs no fake (docs/testing.md §2): this module's whole world is `nowMs` in, one
 * action out, and the two calls the glue makes when an action has an answer. What the suite pins is
 * the ordering the silicon demands — the energy register keeps the previous measurement's value
 * across a retune (docs/hardware_samr21.md §6, measured on this board), so a level is never read before a
 * request on *this* channel has been confirmed — the refusal rules the wire contract promises, and
 * the property that a dwell can never wedge: every path, refused action included, restores, emits
 * once and finishes. */
#include "scan.h"
#include "tu.h"

#include <stdint.h>

#define DRIVE_LIMIT 4000u /* covers a clamped dwell; a runaway machine fails here rather than
                           * spinning the suite forever */

typedef struct
{
    uint32_t steps;
    uint32_t setChannel;
    uint32_t startEd;
    uint32_t readEd;
    uint32_t restore;
    uint32_t emit;
    uint32_t done;
    uint32_t none;
    uint32_t restoreStep; /* the pass on which the restore ran, versus the emit's */
    uint32_t emitStep;
    Scan_Result result;
    int gotResult;
} DriveLog;

typedef struct
{
    int8_t levels[8]; /* fed to each read, cycling */
    int levelCount;
    int fresh;   /* 1 = the glue vouches that this level came from the confirmed request */
    int armOk;   /* the retune/arming accepted */
    int startOk; /* the measurement request accepted */
    int readOk;  /* the read succeeded */
} DriveSpec;

static void specAll(DriveSpec *spec, int8_t level)
{
    int i;

    for (i = 0; i < 8; ++i)
    {
        spec->levels[i] = level;
    }
    spec->levelCount = 1;
    spec->fresh = 1;
    spec->armOk = 1;
    spec->startOk = 1;
    spec->readOk = 1;
}

/* Drive one scan to completion, one pass per step, answering every action as rcp_app would —
 * including taking the result when the emit action arrives, which is the only way a result leaves
 * this module. */
static void driveScan(Scan_State *s, DriveLog *log, uint32_t now, const DriveSpec *spec)
{
    Scan_Action action;
    uint32_t i;
    int sample = 0;

    log->steps = 0u;
    log->setChannel = 0u;
    log->startEd = 0u;
    log->readEd = 0u;
    log->restore = 0u;
    log->emit = 0u;
    log->done = 0u;
    log->none = 0u;
    log->restoreStep = 0u;
    log->emitStep = 0u;
    log->gotResult = 0;
    log->result.channel = 0u;
    log->result.dbm = 0;

    for (i = 0u; i < DRIVE_LIMIT && Scan_OwnsRadio(s) != 0; ++i)
    {
        action = Scan_Advance(s, now);
        now++;
        log->steps++;

        switch (action)
        {
        case SCAN_ACTION_SET_CHANNEL:
            log->setChannel++;
            Scan_ActionDone(s, action, spec->armOk, now);
            break;
        case SCAN_ACTION_START_ED:
            log->startEd++;
            Scan_ActionDone(s, action, spec->startOk, now);
            break;
        case SCAN_ACTION_READ_ED:
            log->readEd++;
            Scan_ReportEd(s, spec->levels[sample % (spec->levelCount > 0 ? spec->levelCount : 1)],
                          spec->fresh);
            sample++;
            Scan_ActionDone(s, action, spec->readOk, now);
            break;
        case SCAN_ACTION_RESTORE_RX:
            log->restore++;
            log->restoreStep = log->steps;
            Scan_ActionDone(s, action, 1, now);
            break;
        case SCAN_ACTION_EMIT:
            log->emit++;
            log->emitStep = log->steps;
            log->gotResult = Scan_TakeResult(s, &log->result);
            Scan_ActionDone(s, action, 1, now);
            break;
        case SCAN_ACTION_DONE:
            log->done++;
            Scan_ActionDone(s, action, 1, now);
            break;
        case SCAN_ACTION_NONE:
        default:
            log->none++;
            break;
        }
    }
}

static void startOne(Scan_State *s, uint32_t mask, uint16_t periodMs, uint32_t now)
{
    Scan_Init(s);
    Scan_Configure(s, mask, periodMs);
    TU_CHECK(Scan_Start(s, now) == 1);
}

TU_TEST(only_a_single_usable_channel_can_start_a_scan)
{
    uint32_t twoChannels;
    Scan_State s;
    DriveLog log;
    DriveSpec spec;

    /* Upstream's rule (`HasOnly1BitSet`): the host iterates the channels, the coprocessor dwells
     * one. */
    TU_EQ(Scan_MaskSingleChannel(SCAN_CHAN_MASK_ALL), 0u);
    TU_EQ(Scan_MaskSingleChannel(0u), 0u);
    twoChannels = (uint32_t)((1u << 20) | (1u << 21));
    TU_EQ(Scan_MaskSingleChannel(twoChannels), 0u);
    TU_EQ(Scan_MaskSingleChannel(1u << 11), (uint32_t)(1u << 11));
    TU_EQ(Scan_MaskSingleChannel(1u << 26), (uint32_t)(1u << 26));
    /* Inside the mask's 32 bits but outside this radio's band: storable, not selectable. */
    TU_EQ(Scan_MaskSingleChannel(1u << 31), 0u);
    TU_EQ(Scan_MaskSingleChannel(1u << 10), 0u);

    specAll(&spec, -80);
    s.pending = SCAN_ACTION_NONE;
    Scan_Init(&s);
    Scan_Configure(&s, twoChannels, 64u);
    TU_EQ(Scan_Start(&s, 0u), 0);
    driveScan(&s, &log, 0u, &spec);
    TU_EQ(log.steps, 0u); /* refused: and nothing was left half-running */
    TU_EQ(Scan_OwnsRadio(&s), 0);

    Scan_Configure(&s, 1u << 25, 64u);
    TU_EQ(Scan_Start(&s, 0u), 1);
    Scan_End(&s, 0);
}

TU_TEST(one_dwell_at_a_time_and_a_second_request_leaves_the_first_alone)
{
    Scan_State s;
    DriveLog log;
    DriveSpec spec;

    specAll(&spec, -80);
    startOne(&s, 1u << 20, 64u, 0u);
    TU_EQ(Scan_Start(&s, 1u), 0);
    TU_EQ(Scan_ReportedState(&s), 2); /* MAC_SCAN_STATE reads ENERGY while a dwell runs */
    driveScan(&s, &log, 0u, &spec);

    TU_EQ(Scan_ReportedState(&s), 0);
    TU_EQ(s.phase, SCAN_PHASE_IDLE);
    TU_EQ(Scan_Start(&s, 100u), 1); /* a finished dwell is startable again */
    Scan_End(&s, 0);
}

TU_TEST(a_level_is_never_read_before_its_request_is_confirmed)
{
    Scan_State s;
    Scan_Action action;
    uint32_t now = 0u;

    startOne(&s, 1u << 15, 10u, now);

    action = Scan_Advance(&s, now);
    TU_EQ(action, SCAN_ACTION_SET_CHANNEL);
    Scan_ActionDone(&s, action, 1, now);

    /* The same pass, immediately after the retune: nothing may be read yet. */
    TU_EQ(Scan_Advance(&s, now), SCAN_ACTION_NONE);

    now++;
    action = Scan_Advance(&s, now);
    TU_EQ(action, SCAN_ACTION_START_ED);
    Scan_ActionDone(&s, action, 1, now);

    /* The request is confirmed, but its measurement is still running: the read waits. */
    TU_EQ(Scan_Advance(&s, now), SCAN_ACTION_NONE);
    now++;
    action = Scan_Advance(&s, now);
    TU_EQ(action, SCAN_ACTION_READ_ED);
    Scan_End(&s, 0);
}

TU_TEST(one_action_is_not_repeated_and_nothing_overtakes_it)
{
    Scan_State s;
    Scan_Action action;

    startOne(&s, 1u << 11, 4u, 0u);

    action = Scan_Advance(&s, 0u);
    TU_EQ(action, SCAN_ACTION_SET_CHANNEL);
    TU_EQ(Scan_Advance(&s, 1u), SCAN_ACTION_NONE);
    TU_EQ(Scan_Advance(&s, 2u), SCAN_ACTION_NONE);

    /* An answer to an action that was never handed out changes nothing. */
    Scan_ActionDone(&s, SCAN_ACTION_READ_ED, 1, 3u);
    TU_EQ(s.phase, SCAN_PHASE_TUNE);
    TU_EQ(s.pending, SCAN_ACTION_SET_CHANNEL);

    Scan_ActionDone(&s, action, 1, 3u);
    TU_EQ(s.phase, SCAN_PHASE_SETTLE);
    Scan_End(&s, 0);
}

TU_TEST(samples_are_the_dwell_and_an_over_long_period_is_clamped_not_refused)
{
    Scan_State s;

    Scan_Init(&s);

    Scan_Configure(&s, 1u << 20, 1u);
    TU_CHECK(Scan_Start(&s, 0u) == 1);
    TU_EQ(s.samplesLeft, 1u);
    Scan_End(&s, 0);

    Scan_Configure(&s, 1u << 20, 128u);
    TU_CHECK(Scan_Start(&s, 0u) == 1);
    TU_EQ(s.samplesLeft, 128u);
    Scan_End(&s, 0);

    Scan_Configure(&s, 1u << 20, 300u);
    TU_CHECK(Scan_Start(&s, 0u) == 1);
    TU_EQ(s.samplesLeft, 300u);
    Scan_End(&s, 0);

    /* Decision D7: stored verbatim for GET, dwelled at the limit, never refused. */
    Scan_Configure(&s, 1u << 20, 60000u);
    TU_EQ(s.periodMs, 60000u);
    TU_EQ(s.dwellMs, (uint16_t)SCAN_PERIOD_MAX_MS);
    TU_CHECK(Scan_Start(&s, 0u) == 1);
    TU_EQ(s.samplesLeft, (uint16_t)SCAN_PERIOD_MAX_MS);
    Scan_End(&s, 0);

    /* A dwell of zero still measures once, so a zero result cannot come from asking nothing. */
    Scan_Configure(&s, 1u << 20, 0u);
    TU_CHECK(Scan_Start(&s, 0u) == 1);
    TU_EQ(s.samplesLeft, 1u);
    Scan_End(&s, 0);
}

TU_TEST(a_dwell_restores_then_emits_then_finishes_once)
{
    Scan_State s;
    DriveLog log;
    DriveSpec spec;
    Scan_Result again;
    specAll(&spec, -80);
    startOne(&s, 1u << 20, 28u, 0u);
    driveScan(&s, &log, 0u, &spec);

    /* A sample cycle costs four passes - request, one pass of confirmation wait, read, one pass of
     * spacing - so a 28 ms dwell makes six of them and one retune for the whole dwell. */
    TU_EQ(log.setChannel, 1u);
    TU_EQ(log.startEd, 7u);
    TU_EQ(log.readEd, 6u);
    TU_EQ(log.restore, 1u);
    TU_EQ(log.emit, 1u);
    TU_EQ(log.done, 1u);
    TU_CHECK(log.restoreStep < log.emitStep); /* restore ran before the result left (D4) */
    TU_EQ(log.gotResult, 1);
    TU_EQ(log.result.channel, 20u);
    TU_EQ(log.result.dbm, -80);
    TU_EQ(Scan_TakeResult(&s, &again), 0); /* a result is emitted once per dwell */
    TU_EQ(s.phase, SCAN_PHASE_IDLE);
}

TU_TEST(invalid_and_unvouched_readings_never_poison_the_maximum)
{
    Scan_State s;
    DriveLog log;
    DriveSpec spec;

    /* Every reading unusable: one result carrying the sentinel, no invented number. */
    specAll(&spec, (int8_t)SCAN_ED_INVALID);
    startOne(&s, 1u << 20, 28u, 0u);
    driveScan(&s, &log, 0u, &spec);
    TU_EQ(log.readEd, 6u);
    TU_EQ(log.result.dbm, SCAN_ED_INVALID);

    /* One usable reading among unusable ones: upstream skips the unusable before taking the max,
   * so one bad reading cannot poison a dwell. */
    spec.levelCount = 4;
    spec.levels[0] = (int8_t)SCAN_ED_INVALID;
    spec.levels[1] = -91;
    spec.levels[2] = (int8_t)SCAN_ED_INVALID;
    spec.levels[3] = -73;
    startOne(&s, 1u << 20, 28u, 0u);
    driveScan(&s, &log, 0u, &spec);
    TU_EQ(log.result.dbm, -73);

    /* A reading the glue cannot vouch for — request unconfirmed, or the transceiver mid-reception,
     * where an energy value stops being refreshed — is counted and discarded. */
    specAll(&spec, -40);
    spec.fresh = 0;
    startOne(&s, 1u << 20, 28u, 0u);
    driveScan(&s, &log, 0u, &spec);
    TU_EQ(log.result.dbm, SCAN_ED_INVALID);
}

TU_TEST(the_maximum_belongs_to_the_channel_that_was_selected)
{
    Scan_State s;
    DriveLog log;
    DriveSpec spec;

    specAll(&spec, -88);
    spec.levelCount = 4;
    spec.levels[0] = -88;
    spec.levels[1] = -73;
    spec.levels[2] = -91;
    spec.levels[3] = -80;

    startOne(&s, 1u << 13, 28u, 0u);
    driveScan(&s, &log, 0u, &spec);

    TU_EQ(log.result.dbm, -73);
    TU_EQ(log.result.channel, 13u);
    TU_EQ(log.setChannel, 1u); /* retuned once, not per sample */
}

TU_TEST(a_refused_action_ends_the_dwell_without_wedging_anything)
{
    Scan_State s;
    DriveLog log;
    DriveSpec spec;
    uint16_t read = 0u;
    uint16_t stale = 0u;

    /* The radio refuses every measurement request. The dwell gives up, restores, emits the
     * sentinel and finishes — and is startable again straight away, which is precisely what
     * upstream's own disarm path does not manage. */
    specAll(&spec, -80);
    spec.startOk = 0;
    startOne(&s, 1u << 20, 20u, 0u);
    driveScan(&s, &log, 0u, &spec);

    TU_CHECK(log.startEd <= (uint32_t)SCAN_FAIL_LIMIT);
    TU_EQ(log.restore, 1u);
    TU_EQ(log.emit, 1u);
    TU_EQ(log.done, 1u);
    TU_EQ(s.phase, SCAN_PHASE_IDLE);
    TU_EQ(log.result.dbm, SCAN_ED_INVALID);
    Scan_Counters(&s, &read, &stale);
    TU_EQ(read, 0u);
    TU_CHECK(stale > 0u); /* the refusals are counted, not hidden */
    TU_CHECK(s.limitReached == 1);
    TU_EQ(Scan_Start(&s, 100u), 1);

    /* A refused retune never dwells at all. */
    specAll(&spec, -80);
    spec.armOk = 0;
    startOne(&s, 1u << 20, 20u, 0u);
    driveScan(&s, &log, 0u, &spec);
    TU_EQ(log.startEd, 0u);
    TU_EQ(log.readEd, 0u);
    TU_EQ(log.restore, 1u);
    TU_EQ(log.result.dbm, SCAN_ED_INVALID);

    /* A read that fails is a lost sample, not a stopped scan - until the refusal limit is
   * reached, and then the dwell ends early and says which of the two happened. */
    specAll(&spec, -70);
    spec.readOk = 0;
    startOne(&s, 1u << 20, 20u, 0u);
    driveScan(&s, &log, 0u, &spec);
    TU_EQ(log.readEd, (uint32_t)SCAN_FAIL_LIMIT);
    TU_CHECK(s.limitReached == 1);
    TU_EQ(log.result.dbm, SCAN_ED_INVALID);
    Scan_Counters(&s, &read, &stale);
    TU_EQ(read, 0u);
    TU_EQ(stale, (uint16_t)SCAN_FAIL_LIMIT);
}

/* The period a host wrote is what ends a dwell - in time. Before this was the rule, the dwell counted
 * requests, and because a request costs about five main-loop passes the scan ran roughly five times as
 * long as the period it was asked for. The board measured it: 50 ms became 276 ms, 200 ms became
 * 1034 ms, 800 ms became 4042 ms. */
TU_TEST(the_period_bounds_the_dwell_in_time)
{
    Scan_State s;
    DriveLog log;
    DriveSpec spec;

    specAll(&spec, -80);
    startOne(&s, 1u << 20, 20u, 0u);
    driveScan(&s, &log, 0u, &spec);

    /* One millisecond per pass, twenty milliseconds of period: four or five samples, and the pass
     * budget still unspent when the dwell ended. */
    TU_CHECK(log.readEd >= 4u && log.readEd <= 6u);
    /* The request budget (one per millisecond, so twenty) was nowhere near used up: the period is what
     * stopped it. Scan_ActionDone clears the budget when the dwell finishes, so the reading that
     * proves this is the sample count against the period, not the counter itself. */
    TU_CHECK(log.readEd < s.periodMs);
    TU_CHECK(log.steps <= 24u);   /* and the dwell did not overrun its period */
    TU_CHECK(log.none > 0u);      /* the machine idles between requests rather than hammering */
    TU_CHECK(log.setChannel == 1u && log.restore == 1u && log.emit == 1u && log.done == 1u);
}

/* Charged as a refusal, three "not ready yet" answers ended a dwell early on a channel with nothing
 * wrong with it - the board said so, in its own records. A wait is not a failure; and a dwell that
 * only ever waits still ends, on its own terms rather than the radio's. */
TU_TEST(a_wait_is_not_a_refusal_and_a_dwell_that_only_waits_still_ends)
{
    Scan_State s;
    DriveLog log;
    DriveSpec spec;
    uint16_t read = 0u;
    uint16_t stale = 0u;

    /* A radio that answers "not ready" every time. Charged as a refusal, this would have stopped after
     * SCAN_FAIL_LIMIT = three asks; as waits it goes the whole SCAN_WAIT_LIMIT, and then ends the
     * dwell because waiting is not the same as waiting forever. */
    specAll(&spec, -80);
    spec.readOk = SCAN_ANSWER_WAITING;
    startOne(&s, 1u << 20, 40u, 0u);
    driveScan(&s, &log, 0u, &spec);

    TU_CHECK(log.readEd >= (uint32_t)SCAN_FAIL_LIMIT + 1u); /* a wait is not counted as a refusal */
    TU_CHECK(log.readEd <= (uint32_t)SCAN_WAIT_LIMIT);      /* and the wait has its own bound */
    TU_EQ(s.limitReached, 1);
    TU_CHECK(log.done == 1u && log.emit == 1u && log.restore == 1u); /* finished, never wedged */

    Scan_Counters(&s, &read, &stale);
    TU_EQ(read, 0u);                     /* nothing was measured */
    TU_CHECK(stale >= 1u);               /* and the readings it did not get are counted, not hidden */
    TU_EQ(log.result.dbm, SCAN_ED_INVALID);
}

TU_TEST(requests_are_spaced_and_a_dwell_has_a_request_cap)
{
    Scan_State s;

    Scan_Init(&s);
    Scan_Configure(&s, 1u << 20, (uint16_t)SCAN_PERIOD_MAX_MS);
    TU_CHECK(Scan_Start(&s, 0u) == 1);
    TU_CHECK(s.samplesLeft <= (uint16_t)SCAN_MAX_SAMPLES);
    TU_CHECK(s.samplesLeft == (uint16_t)SCAN_PERIOD_MAX_MS); /* one per millisecond, no more */
    TU_CHECK(s.endMs == (uint32_t)SCAN_PERIOD_MAX_MS);
    Scan_End(&s, 0);

    /* A period above the bound is clamped, and the clamp is what the dwell and its cap both use. */
    Scan_Init(&s);
    Scan_Configure(&s, 1u << 20, 60000u);
    TU_CHECK(Scan_Start(&s, 100u) == 1);
    TU_EQ(s.dwellMs, (uint16_t)SCAN_PERIOD_MAX_MS);
    TU_EQ(s.endMs, 100u + (uint32_t)SCAN_PERIOD_MAX_MS);
    Scan_End(&s, 0);
}

TU_TEST(a_transmission_preempts_the_dwell_and_resuming_re_arms)
{
    Scan_State s;
    DriveLog log;
    DriveSpec spec;
    Scan_Action action;
    uint32_t now = 0u;
    uint16_t before = 0u;
    uint16_t staleBefore = 0u;
    uint16_t after = 0u;
    uint16_t staleAfter = 0u;

    startOne(&s, 1u << 20, 40u, now);

    /* Bank exactly one sample. */
    for (;;)
    {
        action = Scan_Advance(&s, now);
        if (action == SCAN_ACTION_NONE)
        {
            now++;
            continue;
        }
        if (action == SCAN_ACTION_READ_ED)
        {
            Scan_ReportEd(&s, -85, 1);
        }
        Scan_ActionDone(&s, action, 1, now);
        now++;
        if (s.phase == SCAN_PHASE_MEASURE)
        {
            break;
        }
    }
    Scan_Counters(&s, &before, &staleBefore);
    TU_EQ(before, 1u);
    TU_EQ(s.samplesLeft, 39u);

    /* A SET STREAM_RAW takes the transceiver: the dwell issues nothing while it is gone. */
    Scan_Suspend(&s);
    TU_EQ(Scan_Advance(&s, now), SCAN_ACTION_NONE);
    TU_EQ(Scan_Advance(&s, now + 1u), SCAN_ACTION_NONE);
    TU_EQ(s.phase, SCAN_PHASE_SUSPEND);

    /* A finished transmission left the chip in the mode its sender asked for, and left its own
     * channel assessment in the energy register: resuming re-tunes rather than continuing. */
    Scan_Resume(&s);
    TU_EQ(s.phase, SCAN_PHASE_TUNE);

    specAll(&spec, -77);
    log.steps = 0u;
    while (Scan_OwnsRadio(&s) != 0)
    {
        action = Scan_Advance(&s, now);
        if (action == SCAN_ACTION_NONE)
        {
            now++;
            continue;
        }
        if (log.setChannel == 0u && log.steps == 1u)
        {
            TU_EQ(action, SCAN_ACTION_SET_CHANNEL); /* the re-arm, and not a continuation */
            log.setChannel = 1u;
        }
        if (action == SCAN_ACTION_READ_ED)
        {
            Scan_ReportEd(&s, -77, 1);
        }
        if (action == SCAN_ACTION_EMIT)
        {
            TU_CHECK(Scan_TakeResult(&s, &log.result) == 1);
        }
        Scan_ActionDone(&s, action, 1, now);
        now++;
    }

    Scan_Counters(&s, &after, &staleAfter);
    TU_CHECK(after > before);            /* the samples already banked survived the interruption */
    TU_CHECK(after >= 4u);
    TU_EQ(log.result.dbm, -77);
}

TU_TEST(deadline_arithmetic_survives_the_clock_wrapping)
{
    Scan_State s;
    Scan_Action action;
    uint32_t now = 0xFFFFFFFEu;

    startOne(&s, 1u << 20, 8u, now);

    action = Scan_Advance(&s, now);
    TU_EQ(action, SCAN_ACTION_SET_CHANNEL);
    Scan_ActionDone(&s, action, 1, now); /* settle deadline lands on 0xFFFFFFFF */

    now = 0xFFFFFFFFu;
    action = Scan_Advance(&s, now);
    TU_EQ(action, SCAN_ACTION_START_ED);
    Scan_ActionDone(&s, action, 1, now); /* the read deadline wraps past zero */

    TU_EQ(Scan_Advance(&s, now), SCAN_ACTION_NONE);
    action = Scan_Advance(&s, 0u);
    TU_EQ(action, SCAN_ACTION_READ_ED);
    Scan_ReportEd(&s, -60, 1);
    Scan_ActionDone(&s, action, 1, 0u);

    now = 1u;
    while (Scan_OwnsRadio(&s) != 0)
    {
        action = Scan_Advance(&s, now);
        if (action != SCAN_ACTION_NONE)
        {
            if (action == SCAN_ACTION_READ_ED)
            {
                Scan_ReportEd(&s, -55, 1);
            }
            Scan_ActionDone(&s, action, 1, now);
        }
        now++;
    }
    TU_EQ(s.phase, SCAN_PHASE_IDLE);
}

TU_TEST(an_action_that_is_never_answered_is_abandoned_not_waited_for)
{
    Scan_State s;
    DriveLog log;
    DriveSpec spec;
    Scan_Action action;
    uint32_t now = 0u;

    startOne(&s, 1u << 20, 2u, now);
    action = Scan_Advance(&s, now);
    TU_EQ(action, SCAN_ACTION_SET_CHANNEL);

    /* The glue forgets to answer. A machine that waits for that answer forever is a scan that never
     * ends and a transceiver the host can no longer use, so an unanswered action is abandoned as
     * refused and the dwell takes its refusal path: restore, report that nothing was measured,
     * finish. */
    now += SCAN_PENDING_TIMEOUT_MS;
    action = Scan_Advance(&s, now);
    TU_CHECK(action != SCAN_ACTION_SET_CHANNEL);
    TU_CHECK(s.pending != SCAN_ACTION_SET_CHANNEL);
    TU_CHECK(s.limitReached == 1);

    specAll(&spec, -70);
    while (Scan_OwnsRadio(&s) != 0)
    {
        action = Scan_Advance(&s, now);
        if (action == SCAN_ACTION_EMIT)
        {
            TU_CHECK(Scan_TakeResult(&s, &log.result) == 1);
        }
        if (action != SCAN_ACTION_NONE)
        {
            Scan_ActionDone(&s, action, 1, now);
        }
        now++;
    }

    TU_EQ(log.result.dbm, SCAN_ED_INVALID); /* it reports honestly that it measured nothing */
    TU_EQ(s.phase, SCAN_PHASE_IDLE);
    TU_CHECK(Scan_Start(&s, now) == 1);     /* and it is usable again */
    Scan_End(&s, 0);
}

TU_TEST(an_interrupted_dwell_reports_and_starts_clean)
{
    Scan_State s;
    Scan_Result r;
    Scan_Action action;
    uint32_t now = 0u;

    startOne(&s, 1u << 20, 200u, now);
    action = Scan_Advance(&s, now);
    Scan_ActionDone(&s, action, 1, now);

    /* A disarm or a host RESET while the dwell runs: it ends, and it says it had been running. */
    TU_EQ(Scan_End(&s, &r), 1);
    TU_EQ(r.channel, 20u);
    TU_EQ(r.dbm, SCAN_ED_INVALID); /* nothing usable was measured, so nothing is invented */
    TU_EQ(s.phase, SCAN_PHASE_IDLE);
    TU_EQ(Scan_OwnsRadio(&s), 0);

    /* What the host configured survives; the half-taken dwell does not. */
    TU_EQ(s.mask, (uint32_t)(1u << 20));
    TU_EQ(s.periodMs, 200u);
    TU_EQ(s.samplesLeft, 0u);
    TU_EQ(Scan_End(&s, &r), 0); /* nothing was running */
    TU_CHECK(Scan_Start(&s, 10u) == 1);
    Scan_End(&s, 0);
}

TU_TEST(a_fresh_state_carries_upstreams_defaults_and_holds_nothing)
{
    Scan_State s;

    Scan_Init(&s);
    TU_EQ(s.phase, SCAN_PHASE_IDLE);
    TU_EQ(s.mask, SCAN_CHAN_MASK_ALL);
    TU_EQ(s.periodMs, (uint16_t)SCAN_PERIOD_DEFAULT_MS);
    TU_EQ(Scan_OwnsRadio(&s), 0);
    TU_EQ(Scan_ReportedState(&s), 0);
    TU_EQ(Scan_Advance(&s, 0u), SCAN_ACTION_NONE);
    /* The fresh defaults cannot start a scan on their own, which is upstream's behaviour too: the
     * host names a channel or it gets the refusal. */
    TU_EQ(Scan_MaskSingleChannel(s.mask), 0u);
    TU_EQ(Scan_Start(&s, 0u), 0);
}

TU_TEST(a_null_state_is_not_a_crash)
{
    Scan_Result r;
    uint16_t n = 0u;

    Scan_Init(0);
    Scan_Configure(0, 1u << 20, 8u);
    TU_EQ(Scan_Start(0, 0u), 0);
    TU_EQ(Scan_Advance(0, 0u), SCAN_ACTION_NONE);
    Scan_ActionDone(0, SCAN_ACTION_START_ED, 1, 0u);
    Scan_ReportEd(0, -70, 1);
    Scan_Suspend(0);
    Scan_Resume(0);
    TU_EQ(Scan_End(0, &r), 0);
    TU_EQ(Scan_TakeResult(0, &r), 0);
    TU_EQ(Scan_TakeResult(0, 0), 0);
    Scan_Counters(0, &n, 0);
    TU_EQ(Scan_OwnsRadio(0), 0);
    TU_EQ(Scan_ReportedState(0), 0);
    TU_EQ(Scan_MaskSingleChannel(0xFFFFFFFFu), 0u);
}

int main(void)
{
    static const Tu_Test tests[] = {
        {"only_a_single_usable_channel_can_start_a_scan", only_a_single_usable_channel_can_start_a_scan},
        {"one_dwell_at_a_time_and_a_second_request_leaves_the_first_alone",
         one_dwell_at_a_time_and_a_second_request_leaves_the_first_alone},
        {"a_level_is_never_read_before_its_request_is_confirmed",
         a_level_is_never_read_before_its_request_is_confirmed},
        {"one_action_is_not_repeated_and_nothing_overtakes_it",
         one_action_is_not_repeated_and_nothing_overtakes_it},
        {"samples_are_the_dwell_and_an_over_long_period_is_clamped_not_refused",
         samples_are_the_dwell_and_an_over_long_period_is_clamped_not_refused},
        {"a_dwell_restores_then_emits_then_finishes_once",
         a_dwell_restores_then_emits_then_finishes_once},
        {"invalid_and_unvouched_readings_never_poison_the_maximum",
         invalid_and_unvouched_readings_never_poison_the_maximum},
        {"the_maximum_belongs_to_the_channel_that_was_selected",
         the_maximum_belongs_to_the_channel_that_was_selected},
        {"the_period_bounds_the_dwell_in_time", the_period_bounds_the_dwell_in_time},
        {"requests_are_spaced_and_a_dwell_has_a_request_cap",
         requests_are_spaced_and_a_dwell_has_a_request_cap},
        {"a_wait_is_not_a_refusal_and_a_dwell_that_only_waits_still_ends",
         a_wait_is_not_a_refusal_and_a_dwell_that_only_waits_still_ends},
        {"a_refused_action_ends_the_dwell_without_wedging_anything",
         a_refused_action_ends_the_dwell_without_wedging_anything},
        {"a_transmission_preempts_the_dwell_and_resuming_re_arms",
         a_transmission_preempts_the_dwell_and_resuming_re_arms},
        {"deadline_arithmetic_survives_the_clock_wrapping", deadline_arithmetic_survives_the_clock_wrapping},
        {"an_action_that_is_never_answered_is_abandoned_not_waited_for",
         an_action_that_is_never_answered_is_abandoned_not_waited_for},
        {"an_interrupted_dwell_reports_and_starts_clean", an_interrupted_dwell_reports_and_starts_clean},
        {"a_fresh_state_carries_upstreams_defaults_and_holds_nothing",
         a_fresh_state_carries_upstreams_defaults_and_holds_nothing},
        {"a_null_state_is_not_a_crash", a_null_state_is_not_a_crash}};

    Tu_Run("scan", tests, (int)(sizeof(tests) / sizeof(tests[0])));
    return 0;
}
