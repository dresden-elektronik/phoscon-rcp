/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* rcp_app.c — the app-side Spinel transport (rcp_app.h). */
#include "rcp_app.h"

#include "hal.h"
#include "hal_diag.h"
#include "com.h"
#include "dbg.h"
#include "hdlc.h"
#include "mac.h"
#include "props_stream.h"
#include "scan.h"
#include "session.h"
#include "u_bstream.h"
#include "radio.h"

#include "board.h"
#include "product.h"
#include "props_debug.h"

/* The encode scratch is exactly the wire worst case (hdlc.h HDLC_FRAME_MAX); the ring
 * holds one such reply whole, with room for a second, smaller one. */
#define RCP_APP_ENC_MAX HDLC_FRAME_MAX
/* What may wait for the host link. A board on a slow link names more in product.h: at 115200 baud a frame
 * leaves in 14 ms, and a neighbour's fragments arrive every 5 ms, 2.3 KB for one 1280-octet datagram. */
#ifdef PRODUCT_TX_RING
#define RCP_APP_TX_RING PRODUCT_TX_RING
#else
#define RCP_APP_TX_RING 512u
#endif

/* The decoder buffer and the encode scratch hold the largest frame, the ring holds one
 * whole wire frame, and the value path holds the largest value (session.h RCP_VALUE_MAX):
 * fail the build, not the frame, if a size is ever edited below its payload. */
typedef char hdlcFrameHoldsLargestValue[(HDLC_FRAME_MAX >= (RCP_VALUE_MAX + 3u)) ? 1 : -1];
typedef char rcpAppRxRingHoldsAWireFrame[(RCP_APP_RX_RING >= HDLC_FRAME_MAX) ? 1 : -1];

static uint8_t rcpRxRing[RCP_APP_RX_RING];
static volatile uint16_t rcpRxHead; /* written by the IRQ */
static volatile uint16_t rcpRxTail; /* written by the loop */
static uint32_t rcpRxDropped;
static uint32_t rcpRxFiltered; /* NETWORK mode: frames on another PAN, dropped here */

static Hdlc_Rx rcpHdlc;
static Rcp_Session rcpSession;
/* Outbound replies are HDLC-encoded into this ring and pushed out one COM_TX_MAX
 * packet per pass; a CDC bulk transfer spans packets, so a packet is a transaction, not
 * a transfer ceiling. Full-pool policy: drop-whole and count (coding-rules.md §4.3). */
static uint8_t rcpTxRing[RCP_APP_TX_RING];
static uint16_t rcpTxHead; /* producer: rcpAppQueueReply appends here */
static uint16_t rcpTxTail; /* consumer: RcpApp_Poll sends from here */
static uint16_t rcpTxCount;
static uint8_t rcpTxScratch[RCP_APP_ENC_MAX];
static uint32_t rcpTxDropped;
static uint32_t rcpTxSent;

#ifdef RCP_UART_BITRATE
/* The link's rate (UART_BITRATE, docs/protocol.md §10). The answer to the request that changes it is the
 * last frame at the old rate: rcpLinkMark counts the queued octets still to leave before the switch, and
 * nothing past them goes until the link has moved. A raised rate that hears only framing errors since the
 * last frame falls back to the board's own, which is how a host that starts over, and the flasher, reach
 * the board again. */
#define RCP_LINK_FALLBACK_ERRORS 8u
static uint32_t rcpLinkBitrate; /* what the link runs at */
static uint32_t rcpLinkTarget;  /* where it goes, while rcpLinkPending */
static int rcpLinkPending;
static uint16_t rcpLinkMark;
static uint16_t rcpLinkErrorsAtFrame;
#endif

/* ---- the radio (hal/radio.h) ------------------------------------------ */
/* The extended address the radio's filter view holds, compared the way the sync below decides it: one
 * octet at a time, because a -nostdlib image has no memcmp (docs/coding-rules.md §1). */
static int rcpAppExtAddrSame(const uint8_t *a, const uint8_t *b)
{
    uint8_t i;
    uint8_t diff = 0u;

    /* Reduced rather than short-circuited: an octet loop with an early return is a memcmp to GCC,
     * and a -nostdlib image has none (the same reason props.c clears its address through a volatile
     * pointer). */
    for (i = 0u; i < 8u; ++i)
        diff = (uint8_t)(diff | (uint8_t)(a[i] ^ b[i]));

    return diff == 0u;
}

static void rcpAppQueueReply(const uint8_t *spinel, uint16_t len);

static int rcpRadioReady;
static int rcpRadioRx;
static int rcpRadioFilter = -1; /* -1: nothing asked for yet, so the first pass applies */
static uint8_t rcpRadioChannel;
static int8_t rcpRadioPower = 127; /* 127 = nothing written yet */
static int8_t rcpRadioCca = 127;   /* 127 = nothing written yet: the transceiver's reset threshold */
static uint16_t rcpRadioPanId;
static uint16_t rcpRadioShortAddr;
static uint8_t rcpRadioExtAddr[8];
static int rcpRadioAddrSet;

/* ---- the energy-scan dwell (rcp/scan.h) --------------------------------
 * The machine's state is app-owned and advanced once per pass; only this seam turns a scan property
 * into a Radio_* call (coding-rules.md §4). `rcpScanSaved*` is what the transceiver was doing when a
 * dwell took it over, so a dwell that ends without its own restore action - disarmed or reset - puts
 * that state back instead of leaving the radio tuned to a channel nobody asked for
 * (docs/protocol.md §10.1). */
static Scan_State rcpScan;
static uint8_t rcpScanSavedChannel;
static int rcpScanSavedFilter;
static int rcpScanSavedRx;
static uint32_t rcpScanRxDropped; /* frames suppressed while a dwell held the air */
static uint32_t rcpScanStartMs;   /* when this dwell began, for its own duration record */
static uint32_t rcpScanRefused;   /* dwells the machine would not begin */

static void rcpAppScanNotifyResult(const Scan_Result *res);
static void rcpAppScanNotifyIdle(void);
static void rcpAppScanRestore(void);

/* A received frame becomes an unsolicited VALUE_IS STREAM_RAW (docs/protocol.md §7.2), built in
 * Radio_Poll's context. */
static void rcpAppRxFrame(const Radio_Frame *frame)
{
    static uint8_t rxReply[PROPS_STREAM_RX_MAX + 16u];
    uint8_t value[PROPS_STREAM_RX_MAX];
    U_BStream vs;
    U_BStream out;
    uint16_t vlen;
    uint16_t n;

    /* While a dwell holds the transceiver its own measurements are the only traffic that matters,
     * and the reply ring is 512 B: on a busy channel a scan could push its own result out of the pool
     * with frames the host never asked for (docs/protocol.md §10.1). */
    if (Scan_OwnsRadio(&rcpScan))
    {
        rcpScanRxDropped++;
        return;
    }

    /* NETWORK wants every frame on our PAN, but the chip's promiscuous bit bypasses the
     * whole filter, PAN included, so a foreign-PAN frame is dropped here. */
    if (rcpSession.props.promiscuousMode == 1u)
    {
        uint16_t pan;

        if (Mac_DstPan(frame->psdu, frame->len, &pan) && pan != 0xFFFFu &&
            pan != rcpSession.props.panId)
        {
            rcpRxFiltered++;
            return;
        }
    }

    U_bstream_init(&vs, value, sizeof(value));
    vlen = PropsStream_BuildRawRx(&vs, frame);

    if (vlen == 0u)
    {
        rcpRxDropped++;
        return;
    }

    U_bstream_init(&out, rxReply, sizeof(rxReply));
    /* TID 0: an RX notification is unsolicited (docs/protocol.md §4). */
    n = Spinel_Build(&out, 0u, SPINEL_CMD_PROP_VALUE_IS, PROPS_ID_STREAM_RAW, value, vlen);

    if (n == 0u)
    {
        rcpRxDropped++;
        return;
    }

    rcpAppQueueReply(rxReply, n);
}

/* The records reach the host as PROPS_ID_RCP_LOG_CRASH_DUMP once the host has written to that
 * property (docs/protocol.md §10). Up to PROPS_DEBUG_MAX records are packed into one value, so
 * the stream costs a frame per batch instead of a frame per record, and it runs only while the
 * host asked for it: a full queue drops records, it never delays the session. */
static void rcpAppEmitCounters(void)
{
    static uint32_t nextAtMs;
#if defined(DS_BOARD_DIAG) || defined(DS_RADIO_WITNESS)
    static uint32_t nextRxAtMs;
#endif
    uint32_t now;
    uint16_t lost;

    Tick_GetMs(&now);

#if defined(DS_BOARD_DIAG) || defined(DS_RADIO_WITNESS)
    /* Modular, as every deadline here (docs/architecture.md §7): a plain compare is overdue on every
     * pass of the period before the 49.7-day wrap. */
    if (nextRxAtMs == 0u || (int32_t)(now - nextRxAtMs) >= 0)
    {
        nextRxAtMs = now + 30000u;
        Radio_Witnesses();
#ifdef DS_BOARD_DIAG
        Nvram_WitnessRow();
#endif
    }
#endif

    if (nextAtMs == 0u)
    {
        nextAtMs = now + 5000u;
        return;
    }

    if ((int32_t)(now - nextAtMs) < 0)
        return;

    nextAtMs = now + 5000u;
    lost = rcpRxDropped > 0xFFFFu ? 0xFFFFu : (uint16_t)rcpRxDropped;
    Debug_Emit(DBG_COMP_APP, lost, DBG_APP_RSN_FRAMES_RX);
    lost = rcpRxFiltered > 0xFFFFu ? 0xFFFFu : (uint16_t)rcpRxFiltered;
    Debug_Emit(DBG_COMP_APP, lost, DBG_APP_RSN_FRAMES_FILTERED);
    lost = rcpScanRxDropped > 0xFFFFu ? 0xFFFFu : (uint16_t)rcpScanRxDropped;
    Debug_Emit(DBG_COMP_APP, lost, DBG_APP_RSN_SCAN_RX_DROP);
    lost = rcpScanRefused > 0xFFFFu ? 0xFFFFu : (uint16_t)rcpScanRefused;
    Debug_Emit(DBG_COMP_APP, lost, DBG_APP_RSN_SCAN_REFUSED);
}

static void rcpAppEmitDebug(void)
{
    static uint8_t records[PROPS_DEBUG_MAX * PROPS_DEBUG_RECORD];
    static uint8_t value[PROPS_DEBUG_HEADER + PROPS_DEBUG_MAX * PROPS_DEBUG_RECORD];
    static uint8_t reply[RCP_APP_ENC_MAX];
    uint16_t count;
    uint16_t vlen;
    U_BStream out;
    uint16_t n;

    if (rcpSession.props.logEnabled == 0u)
        return;

    rcpAppEmitCounters();

    count = 0u;

    while (count < PROPS_DEBUG_MAX &&
           Debug_QueueTake(&records[count * PROPS_DEBUG_RECORD], PROPS_DEBUG_RECORD) ==
               PROPS_DEBUG_RECORD)
    {
        count++;
    }

    if (count == 0u)
        return;

    vlen = PropsDebug_Pack(value, (uint16_t)sizeof(value), records, count);

    if (vlen == 0u)
        return;

    U_bstream_init(&out, reply, sizeof(reply));
    n = Spinel_Build(&out, 0u, SPINEL_CMD_PROP_VALUE_IS, PROPS_ID_RCP_LOG_CRASH_DUMP, value, vlen);

    if (n != 0u)
        rcpAppQueueReply(reply, n);
}

/* The TID travels as Radio_Tx's token, handed straight back to the TX-done handler. */
#ifdef RCP_TX_TIMING
/* Where the host wants to listen after a transmission on another channel: a CSL child's, say. */
static uint8_t rcpRxChannelAfterTx;
#endif

static int rcpAppTxSink(void *ctx, const PropsStream_TxRequest *req, uint8_t tid)
{
    int ok;

    (void)ctx;

    /* A transmit outranks a dwell rather than interrupting one: the measurement in progress is
     * abandoned, the machine stops issuing actions until the radio is back, and the resume re-tunes,
     * because a TX leaves the transceiver in the mode its sender asked for rather than the mode a
     * measurement needs. The dwell's budget keeps running, so a scan interrupted for half a second
     * measures for less time rather than for longer (docs/protocol.md §10.1). */
    if (Scan_OwnsRadio(&rcpScan))
    {
        Radio_AbortEd();
        Scan_Suspend(&rcpScan);
    }

    if (req->channel != 0u && !Radio_SetChannel(req->channel))
    {
        if (Scan_OwnsRadio(&rcpScan))
            Scan_Resume(&rcpScan); /* a transmit that never started may not keep the dwell suspended */
        return 0;
    }

    if (req->channel != 0u)
        rcpRadioChannel = req->channel;
#ifdef RCP_TX_TIMING
    rcpRxChannelAfterTx = req->rxChannelAfterTx;

    if (req->txDelay != 0u)
    {
        ok = Radio_TxAt(req->psdu,
                        req->len,
                        Props_TxPowerDbm(&rcpSession.props, rcpRadioChannel),
                        req->csmaCa ? 1 : 0,
                        req->txDelayBase + req->txDelay,
                        (void *)(uintptr_t)tid);

        if (!ok && Scan_OwnsRadio(&rcpScan))
            Scan_Resume(&rcpScan);

        return ok;
    }
#endif
    /* cca = enableCSMACA: 0 means transmit now, ignore backoff (docs/protocol.md §7.1). */
    ok = Radio_Tx(req->psdu,
                    req->len,
                    Props_TxPowerDbm(&rcpSession.props, rcpRadioChannel),
                    req->csmaCa ? 1 : 0,
                    (int)req->maxRetries,
                    (void *)(uintptr_t)tid);

    if (!ok && Scan_OwnsRadio(&rcpScan))
        Scan_Resume(&rcpScan); /* same reason: the radio is not borrowed after a failed submit */

    return ok;
}

static void rcpAppTxDone(void *token, int status, int framePending)
{
    static uint8_t txDoneReply[RCP_TX_DONE_MAX];

    /* The borrow is returned when the radio is: the machine re-tunes from wherever it was interrupted
     * and keeps the samples it already took. */
    if (Scan_OwnsRadio(&rcpScan))
        Scan_Resume(&rcpScan);

#ifdef RCP_TX_TIMING
    if (rcpRxChannelAfterTx != 0u && rcpRxChannelAfterTx != rcpRadioChannel && Radio_SetChannel(rcpRxChannelAfterTx))
        rcpRadioChannel = rcpRxChannelAfterTx;
#endif

    U_BStream out;
    uint16_t n;

#ifdef RCP_TX_TIMING
    Rcp_SetTxAck(&rcpSession, Radio_TxAck());
#endif
    U_bstream_init(&out, txDoneReply, sizeof(txDoneReply));
    n = Rcp_TxDone(&rcpSession, (uint8_t)(uintptr_t)token, status, framePending, &out);

    if (n != 0u)
        rcpAppQueueReply(txDoneReply, n);
}

/* What the host's properties ask the transceiver to be doing, in one place so a dwell's restore
 * asks for exactly what an ordinary pass asks for. */
static uint8_t rcpAppWantedFilter(void)
{
    if (rcpSession.props.promiscuousMode == 1u)
        return RADIO_RX_PAN;
    if (rcpSession.props.promiscuousMode >= 2u)
        return RADIO_RX_ALL;
    return RADIO_RX_FILTERED;
}

static int rcpAppWantedRx(void)
{
    return (rcpSession.props.phyEnabled && rcpSession.props.rawStreamEnabled) ? 1 : 0;
}

#ifdef RCP_THREAD
static void rcpAppMacSecLock(void)
{
    HAL_IrqLock();
}

static void rcpAppMacSecUnlock(void)
{
    HAL_IrqUnlock();
}
#endif

#ifdef RCP_TX_TIMING
/* The Enhanced Ack follows the host's keys, so a host that sets none sees the radio it always had. */
static void rcpAppApplyMacSec(void)
{
    static uint8_t applied;

    if (rcpSession.props.macSec.hasKeys != applied &&
        Radio_SetMacSec(rcpSession.props.macSec.hasKeys ? &rcpSession.props.macSec : 0))
        applied = rcpSession.props.macSec.hasKeys;
}
#endif

static void rcpAppApplyRadio(void)
{
    int wantRx;
    uint8_t channel;
    int8_t power;

    if (!rcpRadioReady)
    {
        if (!Radio_Init())
        {
            return; /* the driver recorded why; retried next pass */
        }

        Radio_SetRxHandler(rcpAppRxFrame);
        Radio_SetTxDoneHandler(rcpAppTxDone);
        rcpRadioReady = 1;
    }

    channel = rcpSession.props.channel;

    /* While a dwell runs it is the only writer of the channel, the filter and the receive state:
     * re-applying the host's values underneath a measurement would retune the radio out from under it
     * and leave these shadows claiming a state the transceiver does not have. The host's properties
     * still change - they are what the dwell restores when it ends. The addresses are not part of
     * that: a dwell measures every frame it can hear, and a scan must not lose the host's PAN or
     * addresses (docs/protocol.md §10.1).
     *
     * The channel follows the host's PHY_CHAN writes, not the property's value: a frame's own channel
     * and rxChannelAfterTx move the radio in between, and must stand until the host writes again
     * (docs/protocol.md §7.1). A write the radio refuses - RAIL during a transmission - stays pending,
     * and so does one with no shadow to compare (a refused restore left it unknown). */
    if (!Scan_OwnsRadio(&rcpScan) && channel >= RADIO_CHAN_MIN && channel <= RADIO_CHAN_MAX &&
        (rcpSession.props.channelWritten || rcpRadioChannel == 0u))
    {
        if (Radio_SetChannel(channel))
        {
            rcpRadioChannel = channel;
            rcpSession.props.channelWritten = 0u;
        }
    }

    /* Capped for the channel the radio is on: the board's limit, lower on channel 26. */
    power = Props_TxPowerDbm(&rcpSession.props, rcpRadioChannel);

    if (power != rcpRadioPower)
    {
        if (Radio_SetPower(power))
            rcpRadioPower = power;
    }

    if (rcpSession.props.ccaThresholdDbm != rcpRadioCca)
    {
        if (Radio_SetCcaThreshold(rcpSession.props.ccaThresholdDbm))
            rcpRadioCca = rcpSession.props.ccaThresholdDbm;
    }

    /* docs/protocol.md §10 MAC_PROMISCUOUS_MODE: 0 = PAN+address, 1 = PAN, 2 = no filter. */
    {
        uint8_t wantFilter = rcpAppWantedFilter();
        /* Addresses first, before a filtered state is entered. 0xFFFF / all-zero / all-ones
         * means "none configured" (the datasheet's permissive default). */
        {
            int extOn = 0;
            uint8_t i;

            for (i = 0u; i < 8u; ++i)
            {
                if (rcpSession.props.extAddr[i] != 0x00u && rcpSession.props.extAddr[i] != 0xFFu)
                    extOn = 1;
            }

            /* The extended address is part of the same view: a write of MAC_15_4_LADDR alone leaves the
             * PAN and the short address as they were, and the filter would keep the "none configured"
             * 0xFF it started with - a filtered stick that then delivers nothing addressed to its own
             * long address, which is where 802.15.4-2024 10.21.5.2's Association Response goes. */
            if (!rcpRadioAddrSet || rcpSession.props.panId != rcpRadioPanId ||
                rcpSession.props.shortAddr != rcpRadioShortAddr ||
                !rcpAppExtAddrSame(rcpSession.props.extAddr, rcpRadioExtAddr))
            {
                if (!Radio_SetPanId(rcpSession.props.panId,
                                    rcpSession.props.panId != 0xFFFFu ? 1 : 0) ||
                    !Radio_SetShortAddr(rcpSession.props.shortAddr,
                                        rcpSession.props.shortAddr != 0xFFFFu ? 1 : 0) ||
                    !Radio_SetExtAddr(rcpSession.props.extAddr, extOn))
                {
                    return; /* the radio recorded why; retried next pass */
                }

                rcpRadioPanId = rcpSession.props.panId;
                rcpRadioShortAddr = rcpSession.props.shortAddr;

                {
                    /* Through a volatile pointer: a plain copy loop becomes a memcpy to GCC, and a
                     * -nostdlib image has none (props.c clears its address the same way). */
                    volatile uint8_t *tracked = rcpRadioExtAddr;

                    for (i = 0u; i < 8u; ++i)
                        tracked[i] = rcpSession.props.extAddr[i];
                }

                rcpRadioAddrSet = 1;
            }
        }

        /* The source-match table follows the property, which says when it changed - a RESET that
         * empties it included - so a pass with no change costs a byte test (docs/protocol.md §10.2). */
        if (rcpSession.props.srcMatch.changed)
        {
            if (!Radio_SetSrcMatch(&rcpSession.props.srcMatch))
            {
                return; /* the radio recorded why; retried next pass */
            }

            rcpSession.props.srcMatch.changed = 0u;
        }

        if (!Scan_OwnsRadio(&rcpScan) && (!rcpRadioReady || wantFilter != (uint8_t)rcpRadioFilter))
        {
            if (Radio_SetRxFilter(wantFilter))
                rcpRadioFilter = wantFilter;
        }
    }

    wantRx = rcpAppWantedRx();

    if (!Scan_OwnsRadio(&rcpScan) && wantRx != rcpRadioRx)
    {
        if (Radio_RxEnable(wantRx))
            rcpRadioRx = wantRx;
    }
}

/* Every pass, whatever the apply pass managed: a frame starts and completes and a received one is
 * handed up here, so a setter the radio keeps refusing - retried next pass - must not stop it. */
static void rcpAppPollRadio(void)
{
    if (rcpRadioReady)
        Radio_Poll();
}

/* The dwell is over: put the transceiver back where it was, or where the host asked for meanwhile,
 * and move the shadows with it, so the next pass is a truthful diff instead of a pass that believes a
 * lie. The channel is the one the dwell found unless the host wrote PHY_CHAN during the dwell; with no
 * channel found either, it is the property's. */
static void rcpAppScanRestore(void)
{
    uint8_t want = rcpScanSavedChannel;
    uint8_t filter = rcpAppWantedFilter();
    int arm = rcpAppWantedRx();
    int ok = 1;

    if (rcpSession.props.channelWritten || want < RADIO_CHAN_MIN || want > RADIO_CHAN_MAX)
        want = rcpSession.props.channel;

    if (want >= RADIO_CHAN_MIN && want <= RADIO_CHAN_MAX && want != rcpRadioChannel)
    {
        if (!Radio_SetChannel(want))
            ok = 0;
        else
            rcpRadioChannel = want;
    }

    if (ok && want == rcpSession.props.channel)
        rcpSession.props.channelWritten = 0u;

    if (filter != (uint8_t)rcpRadioFilter)
    {
        if (!Radio_SetRxFilter(filter))
            ok = 0;
        else
            rcpRadioFilter = filter;
    }

    if (arm != rcpRadioRx)
    {
        if (!Radio_RxEnable(arm))
            ok = 0;
        else
            rcpRadioRx = arm;
    }

    if (ok == 0)
    {
        /* A refused restore is recorded and handed back to the ordinary apply path, which retries
         * from an invalidated shadow next pass rather than trusting these shadows. */
        Debug_Emit(DBG_COMP_APP, 0u, DBG_APP_RSN_SCAN_RESTORE);
        rcpRadioChannel = 0u;
        rcpRadioFilter = -1;
        rcpRadioRx = -1;
    }
}

static void rcpAppScanNotifyResult(const Scan_Result *res)
{
    static uint8_t reply[32];
    static uint8_t value[2];
    U_BStream out;
    uint16_t n;

    value[0] = res->channel;
    value[1] = (uint8_t)res->dbm;

    U_bstream_init(&out, reply, sizeof(reply));
    /* A notification builder, because a scan result is unsolicited: TID 0 is what
     * Spinel_BuildValueIs emits (docs/protocol.md §4). */
    n = Spinel_BuildValueIs(&out, PROPS_ID_MAC_SCAN_RESULT, value, 2u);

    if (n != 0u)
        rcpAppQueueReply(reply, n);
    else
        rcpTxDropped++;
}

static void rcpAppScanNotifyIdle(void)
{
    static uint8_t reply[32];
    static uint8_t value[1];
    U_BStream out;
    uint16_t n;

    value[0] = PROPS_SCAN_STATE_IDLE;

    U_bstream_init(&out, reply, sizeof(reply));
    n = Spinel_BuildValueIs(&out, PROPS_ID_MAC_SCAN_STATE, value, 1u);

    if (n != 0u)
        rcpAppQueueReply(reply, n);
    else
        rcpTxDropped++;
}

/* One action per pass, answered in that pass. The dwell is milliseconds long and a loop pass is a
 * microsecond, so a machine that asked for two things per pass would only make the ordering harder to
 * test (docs/architecture.md §6). */
static void rcpAppApplyScan(void)
{
    Scan_Result res;
    Scan_Action action;
    uint32_t now;
    uint32_t mask;
    uint16_t periodMs;
    int8_t dbm;
    int fresh;
    int got;
    int answer;

    Tick_GetMs(&now);

    /* A host RESET re-initialises the properties under a running dwell - session.c's Rcp_Reset calls
     * Props_Init. The reset frame announced the end already, so this one is silent; the transceiver
     * still has to come back to the fresh defaults, which a dwell that ends without its own restore
     * action does not do by itself (docs/protocol.md §6). */
    if (Scan_OwnsRadio(&rcpScan) && rcpSession.props.scanState == PROPS_SCAN_STATE_IDLE)
    {
        Radio_AbortEd();
        (void)Scan_End(&rcpScan, 0);
        rcpAppScanRestore();
        return;
    }

    if (Props_TakeScanStart(&rcpSession.props, &mask, &periodMs))
    {
        Scan_Configure(&rcpScan, mask, periodMs);
        rcpScanSavedChannel = rcpRadioChannel;
        rcpScanSavedFilter = rcpRadioFilter;
        rcpScanSavedRx = rcpRadioRx;

        rcpScanStartMs = now;

        if (!Scan_Start(&rcpScan, now))
        {
            /* The property layer validated this request, so a refusal here is a disagreement between
             * two core modules rather than a host error; the reported state goes back to idle in the
             * same pass, so no host ever reads a scan that is not happening. */
            Props_SetScanState(&rcpSession.props, PROPS_SCAN_STATE_IDLE);
            rcpScanRefused++;
            Debug_Emit(DBG_COMP_APP, (uint16_t)(mask & 0xFFFFu), DBG_APP_RSN_SCAN_REFUSED);
            return;
        }

        Props_SetScanState(&rcpSession.props, PROPS_SCAN_STATE_ENERGY);
    }

    if (!Scan_OwnsRadio(&rcpScan))
        return;

    /* A disarm ends a dwell that can no longer measure. The host is told - the sentinel level for the
     * channel it was on, then the completion - rather than left waiting in its own timeout. Upstream's
     * disarm drops the callback and never clears its scan state, which is the bug this seam does not
     * copy. */
    if (rcpSession.props.phyEnabled == 0u || rcpSession.props.rawStreamEnabled == 0u)
    {
        Radio_AbortEd();

        if (Scan_End(&rcpScan, &res))
        {
            res.dbm = SCAN_ED_INVALID; /* "nothing usable was measured here" is the only claim to make */
            rcpAppScanNotifyResult(&res);
            rcpAppScanNotifyIdle();
            rcpAppScanRestore();
        }

        Props_SetScanState(&rcpSession.props, PROPS_SCAN_STATE_IDLE);
        return;
    }

    action = Scan_Advance(&rcpScan, now);

    switch (action)
    {
    case SCAN_ACTION_SET_CHANNEL:
    {
        int ok;

        /* The dwell arms the state a measurement is legal in, whatever the host asked the transceiver
         * to be doing: MAC_RAW_STREAM_ENABLED is about delivering frames, not about listening for
         * them (docs/protocol.md §10.1). */
        ok = Radio_SetChannel(rcpScan.channel);
        ok = Radio_SetRxFilter(RADIO_RX_ALL) && ok;
        ok = Radio_RxEnable(1) && ok;

        /* The shadows follow the transceiver, so the restore compares the host's wishes with what the
         * dwell left rather than with what was there before it: a restore that compared against the
         * old shadows found nothing to change and left the radio promiscuous - no Ack for any frame -
         * and on the last scanned channel (measured on a ConBee II and a ConBee I). A refused call
         * leaves its shadow unknown, which the restore then applies unconditionally. */
        rcpRadioChannel = ok ? rcpScan.channel : 0u;
        rcpRadioFilter = ok ? RADIO_RX_ALL : -1;
        rcpRadioRx = ok ? 1 : -1;
        Scan_ActionDone(&rcpScan, action, ok, now);
        break;
    }

    case SCAN_ACTION_START_ED:
        Scan_ActionDone(&rcpScan, action,
                        Radio_StartEd() ? SCAN_ANSWER_DONE : SCAN_ANSWER_REFUSED, now);
        break;

    case SCAN_ACTION_READ_ED:
        dbm = SCAN_ED_INVALID;
        fresh = 0;
        got = Radio_TakeEd(&dbm, &fresh);
        answer = (got == 1) ? SCAN_ANSWER_DONE
                            : (got < 0 ? SCAN_ANSWER_REFUSED : SCAN_ANSWER_WAITING);
        if (got == 1)
            Scan_ReportEd(&rcpScan, dbm, fresh);
        Scan_ActionDone(&rcpScan, action, answer, now);
        break;

    case SCAN_ACTION_RESTORE_RX:
        rcpAppScanRestore();
        Scan_ActionDone(&rcpScan, action, 1, now);
        break;

    case SCAN_ACTION_EMIT:
        if (Scan_TakeResult(&rcpScan, &res))
            rcpAppScanNotifyResult(&res);
        Scan_ActionDone(&rcpScan, action, 1, now);
        break;

    case SCAN_ACTION_DONE:
    {
        uint16_t read = 0u;
        uint16_t stale = 0u;

        Props_SetScanState(&rcpSession.props, PROPS_SCAN_STATE_IDLE);
        rcpAppScanNotifyIdle();
        Scan_ActionDone(&rcpScan, action, 1, now);
        Scan_Counters(&rcpScan, &read, &stale);

        /* A dwell is only interpretable with its own two numbers: how many readings it got and how
         * long it ran on the board's clock. Without them a period is a promise about a clock nobody
         * has measured against anything. */
        Debug_Emit(DBG_COMP_APP, read, DBG_APP_RSN_SCAN_SAMPLES);
        Debug_Emit(DBG_COMP_APP, (uint16_t)(now - rcpScanStartMs), DBG_APP_RSN_SCAN_MS);

        if (stale != 0u)
            Debug_Emit(DBG_COMP_APP, stale, DBG_APP_RSN_SCAN_STALE);
        break;
    }

    case SCAN_ACTION_NONE:
    default:
        break;
    }
}

#ifdef RCP_UART_BITRATE
/* After every frame: a rate the host asked for (or a RESET's return to the board's) is due once what is
 * queued now, its answer last, has left. */
static void rcpAppLinkNote(void)
{
    uint32_t want = rcpSession.props.uartBitrate;

    rcpLinkErrorsAtFrame = COM_RxFrameErrors();

    if (want == (rcpLinkPending ? rcpLinkTarget : rcpLinkBitrate))
        return;

    rcpLinkTarget = want;
    rcpLinkMark = rcpTxCount;
    rcpLinkPending = (want != rcpLinkBitrate) ? 1 : 0;
}

/* Every pass: fall back from a rate the host no longer talks at, and move the link once the octets
 * before the mark are out and the line is idle. 1 while the ring must wait for the switch. */
static int rcpAppLinkHolds(void)
{
    int rc;

    if (!rcpLinkPending && rcpLinkBitrate != rcpSession.props.uartBitrateDefault &&
        (uint16_t)(COM_RxFrameErrors() - rcpLinkErrorsAtFrame) >= RCP_LINK_FALLBACK_ERRORS)
    {
        DBG(DBG_COMP_APP, DBG_APP_RSN_LINK_FALLBACK);
        Props_UartBitrateRevert(&rcpSession.props);
        rcpLinkTarget = rcpSession.props.uartBitrate;
        rcpLinkMark = 0u;
        rcpLinkPending = 1;
    }

    if (!rcpLinkPending || rcpLinkMark != 0u)
        return 0;

    rc = COM_SetBitrate(rcpLinkTarget);

    if (rc == 0)
        return 1;

    if (rc > 0)
    {
        rcpLinkBitrate = rcpLinkTarget;
        Debug_Emit(DBG_COMP_APP, (uint16_t)(rcpLinkBitrate / 100u), DBG_APP_RSN_LINK_RATE);
    }
    else
        rcpSession.props.uartBitrate = rcpLinkBitrate; /* a rate in the list this link cannot run */

    /* Whatever the decoder holds is half a frame at the other rate. */
    Hdlc_RxInit(&rcpHdlc);
    rcpLinkErrorsAtFrame = COM_RxFrameErrors();
    rcpLinkPending = 0;
    return 0;
}
#endif

void RcpApp_Init(void)
{
    static uint8_t factoryEui64[8];
    Props_Identity id;

    id.ncpVersion = Board_Get()->ncpVersion;
    id.rxSensitivityDbm = Board_Get()->rxSensitivityDbm; /* per silicon, docs/protocol.md §10 */
    id.txPowerMaxDbm = Board_Get()->txPowerMaxDbm;       /* the board's caps on PHY_TX_POWER */
    id.txPowerMaxCh26Dbm = Board_Get()->txPowerMaxCh26Dbm;
    id.linkBitrate = Board_Get()->linkBaud;
    /* The session keeps the pointer, not the bytes, so this is static. */
    id.hwAddr = Nvram_FactoryEui64(factoryEui64) ? factoryEui64 : 0;
    Rcp_Init(&rcpSession, &id);
#ifdef RCP_THREAD
    /* After the first init, which no interrupt can race: the radio is not up yet, and the lock is the
     * global mask, which does not nest - taken here, its unlock would enable interrupts that a HAL_Init
     * gone wrong left masked on purpose (hal/mg21/hal_mg21.c, no board clock). */
    MacSec_SetLock(rcpAppMacSecLock, rcpAppMacSecUnlock);
#endif
    /* Without a sink a SET STREAM_RAW answers INVALID_STATE (docs/protocol.md §7.1). */
    Rcp_SetTxSink(&rcpSession, rcpAppTxSink, 0);
    Hdlc_RxInit(&rcpHdlc);
    rcpRxHead = 0u;
    rcpRxTail = 0u;
    rcpRxDropped = 0u;
    rcpTxHead = 0u;
    rcpTxTail = 0u;
    rcpTxCount = 0u;
    rcpTxDropped = 0u;
    rcpTxSent = 0u;
#ifdef RCP_UART_BITRATE
    rcpLinkBitrate = Board_Get()->linkBaud;
    rcpLinkTarget = rcpLinkBitrate;
    rcpLinkPending = 0;
    rcpLinkMark = 0u;
    rcpLinkErrorsAtFrame = COM_RxFrameErrors();
#endif
    Scan_Init(&rcpScan);
    rcpScanSavedChannel = 0u;
    rcpScanSavedFilter = -1;
    rcpScanSavedRx = -1;
    rcpScanRxDropped = 0u;
    rcpScanRefused = 0u;
    /* Bring the transceiver up now rather than at the first host SET: a host may set two
     * properties and transmit in one USB batch, and until the radio is ready those SETs
     * are answered INVALID_STATE and the first frame is lost on air. */
    rcpAppApplyRadio();
    rcpAppPollRadio();
}

uint32_t RcpApp_RxDropped(void)
{
    return rcpRxDropped;
}

uint32_t RcpApp_TxDropped(void)
{
    return rcpTxDropped;
}

int RcpApp_Healthy(uint32_t ms)
{
    static int wasHealthy = 1;
    int healthy = Rcp_Healthy(&rcpSession, ms);

    /* Once, where it turns: the watchdog reset that follows says only that the loop stopped feeding. */
    if (wasHealthy && !healthy)
        DBG(DBG_COMP_APP, DBG_APP_RSN_TX_STUCK);

    wasHealthy = healthy;
    return healthy;
}

int RcpApp_PollAgain(void)
{
    return rcpRadioReady ? Radio_PollAgain() : 0;
}

void RcpApp_PushFromIrq(const uint8_t *buf, uint16_t len)
{
    uint16_t i;

    if (buf == 0)
        return;

    for (i = 0; i < len; ++i)
    {
        uint16_t next = (uint16_t)((rcpRxHead + 1u) % RCP_APP_RX_RING);

        if (next == rcpRxTail)
        {
            /* Full: drop the oldest and count it, never overwrite what the loop is reading. */
            rcpRxTail = (uint16_t)((rcpRxTail + 1u) % RCP_APP_RX_RING);
            rcpRxDropped++;
        }

        rcpRxRing[rcpRxHead] = buf[i];
        rcpRxHead = next;
    }
}

/* A reply that does not fit is dropped whole and counted: a truncated HDLC frame is worse
 * than nothing. */
static void rcpAppQueueReply(const uint8_t *reply, uint16_t len)
{
    U_BStream enc;
    uint16_t n;
    uint16_t first;

    if (len == 0u)
        return;

    U_bstream_init(&enc, rcpTxScratch, sizeof(rcpTxScratch));
    n = Hdlc_Encode(reply, len, &enc);

    if (n == 0u || n > RCP_APP_ENC_MAX)
    {
        rcpTxDropped++;
        return;
    }

    if ((uint32_t)rcpTxCount + n > RCP_APP_TX_RING)
    {
        rcpTxDropped++;
        return;
    }
    /* Through U_BStream: a hand-rolled copy loop is a libc memcpy to GCC, and this image
     * is -nostdlib (coding-rules.md §7). The ring may wrap, so copy its two runs. */
    first = (uint16_t)(RCP_APP_TX_RING - rcpTxHead);

    if (first > n)
        first = n;

    U_bstream_init(&enc, &rcpTxRing[rcpTxHead], first);
    U_bstream_put_bytes(&enc, rcpTxScratch, first);

    if (n > first)
    {
        U_bstream_init(&enc, rcpTxRing, (uint16_t)(n - first));
        U_bstream_put_bytes(&enc, &rcpTxScratch[first], (uint16_t)(n - first));
    }

    rcpTxHead = (uint16_t)((rcpTxHead + n) % RCP_APP_TX_RING);
    rcpTxCount = (uint16_t)(rcpTxCount + n);
}

void RcpApp_Poll(void)
{
    uint8_t reply[RCP_VALUE_MAX + 32u];
    uint16_t guard;

    for (guard = 0u; guard < RCP_APP_RX_RING; ++guard)
    {
        uint8_t byte = 0u;
        int empty;
        Hdlc_RxResult r;

        /* One locked step: the IRQ writes the head and, on a full ring, the tail too, and an
         * 8-bit core reads and writes a 16-bit index a byte at a time - unlocked, the loop could
         * see a head half past 255 or the IRQ a tail half advanced. */
        HAL_IrqLock();
        empty = (rcpRxTail == rcpRxHead);

        if (!empty)
        {
            byte = rcpRxRing[rcpRxTail];
            rcpRxTail = (uint16_t)((rcpRxTail + 1u) % RCP_APP_RX_RING);
        }

        HAL_IrqUnlock();

        if (empty)
            break;

        r = Hdlc_RxByte(&rcpHdlc, byte);

        if (r == HDLC_RX_FRAME)
        {
            U_BStream out;

            U_bstream_init(&out, reply, sizeof(reply));

#ifdef RCP_THREAD
            rcpSession.props.radioTimeUs = Radio_ClockUs();
#endif
            if (Rcp_Feed(&rcpSession, rcpHdlc.buf, rcpHdlc.len, &out) > 0u)
                rcpAppQueueReply(reply, (uint16_t)U_bstream_pos(&out));
#ifdef RCP_UART_BITRATE
            rcpAppLinkNote();
#endif
        }
        else if (r == HDLC_RX_ERROR)
        {
            /* A refused frame has no TID to answer to. */
            rcpSession.malformed++;
        }
    }

    rcpAppApplyRadio();
    rcpAppPollRadio();
#ifdef RCP_TX_TIMING
    rcpAppApplyMacSec();
#endif
    rcpAppApplyScan();

    rcpAppEmitDebug();

#ifdef RCP_UART_BITRATE
    if (rcpAppLinkHolds())
        return;
#endif

    if (rcpTxCount > 0u)
    {
        /* One packet per pass, never crossing the ring wrap; a refused arm is retried
         * next pass, preserving FIFO order. */
        uint16_t first = (uint16_t)(RCP_APP_TX_RING - rcpTxTail);
        uint16_t n = rcpTxCount < first ? rcpTxCount : first;

        if (n > COM_TX_MAX)
            n = COM_TX_MAX;

        /* A USB host completes a bulk read on a short packet, and a full one that ends the reply leaves
         * the reply pending until the next one arrives. So a reply never ends on a full packet: its last
         * octet goes on its own. */
        if (n == COM_TX_MAX && rcpTxCount == COM_TX_MAX)
            n = COM_TX_MAX - 1u;

#ifdef RCP_UART_BITRATE
        if (rcpLinkPending && n > rcpLinkMark)
            n = rcpLinkMark;
#endif

        if (COM_TxSend(&rcpTxRing[rcpTxTail], n))
        {
            rcpTxTail = (uint16_t)((rcpTxTail + n) % RCP_APP_TX_RING);
            rcpTxCount = (uint16_t)(rcpTxCount - n);
            rcpTxSent++;
#ifdef RCP_UART_BITRATE
            if (rcpLinkPending)
                rcpLinkMark = (uint16_t)(rcpLinkMark - n);
#endif
        }
    }
}
