/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* session.c — the RCP's session (src/rcp/session.h, docs/protocol.md §5). */
#include "session.h"

void Rcp_Init(Rcp_Session *s, const Props_Identity *id)
{
    if (s == 0)
        return;

    Props_Init(&s->props, id);

    if (id != 0)
        s->identity = *id;
    else
    {
        s->identity.ncpVersion = 0;
        s->identity.rxSensitivityDbm = 0;
        s->identity.txPowerMaxDbm = 0;
        s->identity.txPowerMaxCh26Dbm = 0;
    }

    s->framesIn = 0u;
    s->malformed = 0u;
    s->unknownCmd = 0u;
    s->unknownProp = 0u;
    s->rejected = 0u;
    s->dropped = 0u;
}

uint16_t Rcp_Reset(Rcp_Session *s, U_BStream *out)
{
    uint16_t n;

    if (s == 0 || out == 0)
        return 0u;
    /* docs/protocol.md §6 power-on state: radio off, channel 0, identity unchanged. A frame still
     * on its way stays in flight - the radio is busy with it, and a transmit request meanwhile is
     * answered BUSY - but its completion carries a TID from before the RESET, which a host that has
     * started over may be waiting on for something else, so it goes unanswered. */
    Props_Init(&s->props, &s->identity);

    if (s->txInFlight)
        s->txOrphan = 1;
    n = Spinel_BuildLastStatus(out, SPINEL_STATUS_RESET_SOFTWARE);

    if (n == 0u)
        s->dropped++;

    return n;
}

static uint16_t rcpStatus(Rcp_Session *s, U_BStream *out, uint8_t tid, uint32_t status)
{
    uint16_t n = Spinel_BuildStatus(out, tid, status);

    if (n == 0u)
        s->dropped++;

    return n;
}

/* The TX TID is reserved for a STREAM_RAW SET, so the host routes its reply to
 * HandleTransmitDone and expects the three-field LAST_STATUS there (docs/protocol.md §7.1).
 * framePending is the received ACK's Frame Pending bit; immediate refusals pass 0 because
 * no transmission happened. */
static uint16_t
rcpTxStatus(Rcp_Session *s, U_BStream *out, uint8_t tid, uint32_t status, int framePending)
{
    uint16_t n = Spinel_BuildTxStatus(out, tid, status, framePending);

    if (n == 0u)
        s->dropped++;

    return n;
}

#ifdef RCP_THREAD
/* What a host that reads TRANSMIT_SEC unpacks (radio_spinel.cpp HandleTransmitDone, ncp_base_radio.cpp
 * LinkRawTransmitDone): status, Frame Pending, headerUpdated; a whole radio frame on success, the
 * Ack, empty here; then a secured frame's key index and frame counter. */
static uint16_t
rcpTxDoneSecured(Rcp_Session *s, U_BStream *out, uint8_t tid, uint32_t status, int framePending)
{
    uint8_t value[RCP_VALUE_MAX];
    U_BStream vs;
    uint16_t n;

    U_bstream_init(&vs, value, sizeof(value));
    {
        uint8_t packed[5];

        n = Spinel_PackedWrite(packed, sizeof(packed), status);
        U_bstream_put_bytes(&vs, packed, n);
    }
    U_bstream_put_u8(&vs, framePending ? 1u : 0u);
    U_bstream_put_u8(&vs, s->txSec.headerUpdated ? 1u : 0u);

    if (status == SPINEL_STATUS_OK)
    {
        static const Radio_Frame noAck = {.crcOk = 1};
        const Radio_Frame *ack = (s->txAck != 0 && s->txAck->len <= RCP_TX_ACK_MAX) ? s->txAck : &noAck;

        if (PropsStream_BuildRawRx(&vs, ack) == 0u)
        {
            s->dropped++;
            return 0u;
        }
    }

    if (s->txSec.secured && s->txSec.headerUpdated)
    {
        U_bstream_put_u8(&vs, s->txSec.keyIndex);
        U_bstream_put_u32_le(&vs, s->txSec.frameCounter);
    }

    n = Spinel_Build(out, tid, SPINEL_CMD_PROP_VALUE_IS, 0u, value, (uint16_t)(sizeof(value) - U_bstream_remaining(&vs)));

    if (n == 0u)
        s->dropped++;

    return n;
}
#endif

/* INSERT and REMOVE answer with VALUE_INSERTED / VALUE_REMOVED carrying the request's own
 * value, which is upstream's shape (ncp_base.cpp WritePropertyValueInsertedRemovedFrame); a
 * refusal is a LAST_STATUS, the same as for a SET (docs/protocol.md §5). */
static uint16_t rcpInsertRemove(Rcp_Session *s, U_BStream *out, const Spinel_Frame *f, int insert)
{
    uint32_t status;

    if (insert)
        status = Props_Insert(&s->props, f->prop, f->value, f->valueLen);
    else
        status = Props_Remove(&s->props, f->prop, f->value, f->valueLen);

    if (status == SPINEL_STATUS_PROP_NOT_FOUND)
        s->unknownProp++;
    else if (status != SPINEL_STATUS_OK)
        s->rejected++;

    if (status == SPINEL_STATUS_OK)
    {
        uint16_t n = Spinel_Build(out,
                                  f->tid,
                                  insert ? SPINEL_CMD_PROP_VALUE_INSERTED : SPINEL_CMD_PROP_VALUE_REMOVED,
                                  f->prop,
                                  f->value,
                                  f->valueLen);

        if (n == 0u)
            s->dropped++;

        return n;
    }

    return rcpStatus(s, out, f->tid, status);
}

static uint16_t
rcpAnswer(Rcp_Session *s, U_BStream *out, uint8_t tid, uint32_t prop, uint32_t status)
{
    const Props_Desc *desc = Props_Find(prop);
    uint16_t n = 0u;

    if (status == SPINEL_STATUS_OK && desc != 0 && (desc->access & PROPS_ACC_READ) != 0u)
    {
        /* docs/protocol.md §5: a successful SET is confirmed with the property echo. */
        uint8_t value[RCP_VALUE_MAX];
        U_BStream vs;

        U_bstream_init(&vs, value, sizeof(value));
        {
            uint32_t got = Props_Get(&s->props, prop, &vs);

            if (got == SPINEL_STATUS_OK)
            {
                uint16_t vlen = (uint16_t)(sizeof(value) - U_bstream_remaining(&vs));

                n = Spinel_Build(out, tid, SPINEL_CMD_PROP_VALUE_IS, prop, value, vlen);
            }
            else if (status == SPINEL_STATUS_OK)
            {
                /* A readable property can still refuse a value (HWADDR with no user row):
                 * the handler's reason is the answer, not NOMEM. */
                status = got;
            }
        }
    }

    if (n == 0u && status == SPINEL_STATUS_OK)
    {
        /* A GET is answered with a value or an error, never OK-with-no-value. */
        status = SPINEL_STATUS_NOMEM;
    }

    if (n == 0u)
        n = rcpStatus(s, out, tid, status);

    return n;
}

/* docs/protocol.md §7.1: the one SET that is not a table write; its answer is deferred
 * to the radio's outcome. */
static uint16_t rcpSetStreamRaw(Rcp_Session *s, U_BStream *out, const Spinel_Frame *f)
{
    PropsStream_TxRequest req;
    uint32_t limitMs = RCP_TX_STUCK_MS;

    if (!s->props.rawStreamEnabled)
    {
        s->rejected++;
        return rcpTxStatus(s, out, f->tid, SPINEL_STATUS_INVALID_STATE, 0);
    }

    if (s->txInFlight)
    {
        s->rejected++;
        return rcpTxStatus(s, out, f->tid, SPINEL_STATUS_BUSY, 0);
    }

    if (!PropsStream_ParseRawTx(f->value, f->valueLen, &req))
    {
        s->rejected++;
        return rcpTxStatus(s, out, f->tid, SPINEL_STATUS_PARSE_ERROR, 0);
    }

#ifdef RCP_THREAD
    if (!MacSec_SecureTx(&s->props.macSec,
                         req.psdu,
                         req.len,
                         s->props.extAddr,
                         req.headerUpdated,
                         req.securityProcessed,
                         &s->txSec))
    {
        s->rejected++;
        return rcpTxStatus(s, out, f->tid, SPINEL_STATUS_INVALID_STATE, 0);
    }

    /* A timed frame is due at base + delay on the radio's clock, both the host's choice, and every
     * back-end's own deadline runs from that moment (radio.h): so does the limit, or a base ahead of
     * the clock would call a healthy frame stuck. A moment already gone completes at once. µs to ms
     * by >> 9: about twice the lead, never less, and no divide on a core without one. */
    if (req.txDelay != 0u)
    {
        int32_t leadUs = (int32_t)(req.txDelayBase + req.txDelay - (uint32_t)s->props.radioTimeUs);

        if (leadUs > 0)
            limitMs += (uint32_t)leadUs >> 9;
    }
#endif

    if (s->txSink == 0 || !s->txSink(s->txCtx, &req, f->tid))
    {
        s->rejected++;
        return rcpTxStatus(s, out, f->tid, SPINEL_STATUS_INVALID_STATE, 0);
    }

    s->txInFlight = 1;
    s->txOrphan = 0;
    s->txTid = f->tid;
    s->txWatched = 0;
    s->txLimitMs = limitMs;
    return 0u;
}

int Rcp_Healthy(Rcp_Session *s, uint32_t nowMs)
{
    if (s == 0 || !s->txInFlight)
        return 1;

    if (!s->txWatched)
    {
        s->txWatched = 1;
        s->txSinceMs = nowMs;
        return 1;
    }

    /* Elapsed time as an unsigned difference, so the tick may wrap in between. */
    return (uint32_t)(nowMs - s->txSinceMs) <= s->txLimitMs ? 1 : 0;
}

void Rcp_SetTxSink(Rcp_Session *s, Rcp_TxSink sink, void *ctx)
{
    if (s == 0)
        return;

    s->txSink = sink;
    s->txCtx = ctx;
    s->txInFlight = 0;
    s->txOrphan = 0;
#ifdef RCP_THREAD
    s->txAck = 0;
#endif
}

#ifdef RCP_THREAD
void Rcp_SetTxAck(Rcp_Session *s, const Radio_Frame *ack)
{
    if (s != 0)
        s->txAck = ack;
}
#endif

uint16_t Rcp_TxDone(Rcp_Session *s, uint8_t tid, int radioStatus, int framePending, U_BStream *out)
{
    uint32_t status;

    if (s == 0 || out == 0 || !s->txInFlight || tid != s->txTid)
        return 0u;

    s->txInFlight = 0;

    if (s->txOrphan)
    {
        s->txOrphan = 0;
        return 0u;
    }

    switch (radioStatus)
    {
        case RADIO_TX_OK:
            status = SPINEL_STATUS_OK;
            break;
        case RADIO_TX_CCA_FAIL:
            status = SPINEL_STATUS_CCA_FAILURE;
            break;
        case RADIO_TX_NO_ACK:
            /* docs/protocol.md §7.1: transmitted but not acknowledged. */
            status = SPINEL_STATUS_NO_ACK;
            break;
        default:
            /* Driver TIMEOUT: no TRX_END, so NO_ACK would name a mechanism that never ran. */
            status = SPINEL_STATUS_FAILURE;
            break;
    }

#ifdef RCP_THREAD
    if (s->props.macSec.hasKeys)
    {
        uint16_t n;

        /* OpenThread's SubMac asserts on any outcome but OK, NO_ACK, CCA_FAILURE or an abort
         * (sub_mac.cpp HandleTransmitDone), and the radio did not get the frame out. */
        if (status == SPINEL_STATUS_FAILURE)
            status = SPINEL_STATUS_CCA_FAILURE;

        n = rcpTxDoneSecured(s, out, tid, status, framePending);

        s->txAck = 0;
        return n;
    }
#endif

    return rcpTxStatus(s, out, tid, status, framePending);
}

uint16_t Rcp_Feed(Rcp_Session *s, const uint8_t *payload, uint16_t len, U_BStream *out)
{
    Spinel_Frame f;
    uint32_t status;
    uint16_t n = 0u;

    if (s == 0 || out == 0)
        return 0u;

    if (!Spinel_Decode(payload, len, &f))
    {
        /* No trustworthy TID, so no answer (docs/protocol.md §5 correlates by TID). */
        s->malformed++;
        return 0u;
    }

    s->framesIn++;

    switch (f.cmd)
    {
        case SPINEL_CMD_NOOP:
            n = 0u;
            break;

        case SPINEL_CMD_RESET:
            n = Rcp_Reset(s, out);
            break;

        case SPINEL_CMD_PROP_VALUE_GET:
            if (Props_Find(f.prop) == 0)
            {
                s->unknownProp++;
                n = rcpStatus(s, out, f.tid, SPINEL_STATUS_PROP_NOT_FOUND);
            }
            else if ((Props_Find(f.prop)->access & PROPS_ACC_READ) == 0u)
            {
                /* A write-only property holds a key: as far as a GET goes, it is not there. */
                s->unknownProp++;
                n = rcpStatus(s, out, f.tid, SPINEL_STATUS_PROP_NOT_FOUND);
            }
            else
                n = rcpAnswer(s, out, f.tid, f.prop, SPINEL_STATUS_OK);

            break;

        case SPINEL_CMD_PROP_VALUE_SET:
            if (f.prop == PROPS_ID_STREAM_RAW)
            {
                n = rcpSetStreamRaw(s, out, &f);
                break;
            }

            status = Props_Set(&s->props, f.prop, f.value, f.valueLen);

            if (status == SPINEL_STATUS_PROP_NOT_FOUND)
                s->unknownProp++;
            else if (status != SPINEL_STATUS_OK)
                s->rejected++;

            /* A write-only property has no echo: upstream answers its SET with LAST_STATUS. */
            if (status == SPINEL_STATUS_OK && (Props_Find(f.prop)->access & PROPS_ACC_READ) == 0u)
                n = rcpStatus(s, out, f.tid, SPINEL_STATUS_OK);
            else
                n = rcpAnswer(s, out, f.tid, f.prop, status);
            break;

        case SPINEL_CMD_PROP_VALUE_INSERT:
        case SPINEL_CMD_PROP_VALUE_REMOVE:
            n = rcpInsertRemove(s, out, &f, f.cmd == SPINEL_CMD_PROP_VALUE_INSERT ? 1 : 0);
            break;

        default:
            s->unknownCmd++;
            n = rcpStatus(s, out, f.tid, SPINEL_STATUS_INVALID_COMMAND);
            break;
    }

    return n;
}
