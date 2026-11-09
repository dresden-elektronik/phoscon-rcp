/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* props.c — the Spinel property table (src/rcp/props.h, docs/protocol.md §10). */
#include "props.h"

#include "props_debug.h"
#include "spinel.h"
#include "srcmatch.h"

#define PROPS_CHAN_MIN 11u
#define PROPS_CHAN_MAX 26u

#define PROPS_RO PROPS_ACC_READ
#define PROPS_RW (PROPS_ACC_READ | PROPS_ACC_WRITE)
#define PROPS_WO PROPS_ACC_WRITE

static const Props_Desc propsTable[] = {{PROPS_ID_HWADDR, PROPS_T_BE64, PROPS_RO},
                                        {PROPS_ID_LAST_STATUS, PROPS_T_U8, PROPS_RO},
                                        {PROPS_ID_CAPS, PROPS_T_CAP_LIST, PROPS_RO},
                                        {PROPS_ID_PROTOCOL_VERSION, PROPS_T_PACKED2, PROPS_RO},
                                        {PROPS_ID_NCP_VERSION, PROPS_T_STR, PROPS_RO},
                                        {PROPS_ID_INTERFACE_TYPE, PROPS_T_U8, PROPS_RO},
                                        {PROPS_ID_RCP_API_VERSION, PROPS_T_U8, PROPS_RO},
                                        {PROPS_ID_RCP_MIN_HOST_API, PROPS_T_U8, PROPS_RO},
                                        {PROPS_ID_PHY_ENABLED, PROPS_T_BOOL, PROPS_RW},
                                        {PROPS_ID_PHY_CHAN, PROPS_T_U8, PROPS_RW},
                                        {PROPS_ID_PHY_CHAN_SUPPORTED, PROPS_T_CHAN_BAND, PROPS_RO},
                                        {PROPS_ID_PHY_CHAN_PREFERRED, PROPS_T_CHAN_BAND, PROPS_RO},
                                        {PROPS_ID_PHY_CCA_THRESHOLD, PROPS_T_I8, PROPS_RW},
                                        {PROPS_ID_PHY_TX_POWER, PROPS_T_I8, PROPS_RW},
                                        {PROPS_ID_PHY_RSSI, PROPS_T_I8, PROPS_RO},
                                        {PROPS_ID_PHY_RX_SENSITIVITY, PROPS_T_I8, PROPS_RO},
                                        {PROPS_ID_RADIO_CAPS, PROPS_T_PACKED1, PROPS_RO},
                                        {PROPS_ID_MAC_PANID, PROPS_T_U16, PROPS_RW},
                                        {PROPS_ID_MAC_SADDR, PROPS_T_U16, PROPS_RW},
                                        {PROPS_ID_MAC_LADDR, PROPS_T_BE64, PROPS_RW},
                                        {PROPS_ID_MAC_RAW_STREAM, PROPS_T_BOOL, PROPS_RW},
                                        {PROPS_ID_MAC_RX_ON_WHEN_IDLE, PROPS_T_BOOL, PROPS_RW},
                                        {PROPS_ID_MAC_SCAN_STATE, PROPS_T_U8, PROPS_RW},
                                        {PROPS_ID_MAC_SCAN_MASK, PROPS_T_CHAN_LIST, PROPS_RW},
                                        {PROPS_ID_MAC_SCAN_PERIOD, PROPS_T_U16, PROPS_RW},
                                        {PROPS_ID_MAC_PROMISCUOUS, PROPS_T_U8, PROPS_RW},
                                        {PROPS_ID_MAC_SRC_MATCH_ENABLED, PROPS_T_BOOL, PROPS_RW},
                                        {PROPS_ID_MAC_SRC_MATCH_SHORT, PROPS_T_ADDR_LIST_SHORT,
                                         PROPS_RW},
                                        {PROPS_ID_MAC_SRC_MATCH_EXT, PROPS_T_ADDR_LIST_EXT,
                                         PROPS_RW},
#ifdef RCP_THREAD
                                        {PROPS_ID_RCP_MAC_KEY, PROPS_T_MAC_SEC, PROPS_WO},
                                        {PROPS_ID_RCP_MAC_FRAME_COUNTER, PROPS_T_MAC_SEC, PROPS_WO},
                                        {PROPS_ID_RCP_TIMESTAMP, PROPS_T_U64, PROPS_RO},
#ifdef RCP_TX_TIMING
                                        {PROPS_ID_RCP_ENH_ACK_PROBING, PROPS_T_MAC_SEC, PROPS_WO},
                                        {PROPS_ID_RCP_CSL_ACCURACY, PROPS_T_U8, PROPS_RO},
                                        {PROPS_ID_RCP_CSL_UNCERTAINTY, PROPS_T_U8, PROPS_RO},
#endif
#endif
                                        {PROPS_ID_RCP_LOG_CRASH_DUMP, PROPS_T_U8, PROPS_RW}};

/* docs/protocol.md §10: PROTOCOL_VERSION 4.3 is what zoh reads. */
#define PROPS_PROTOCOL_MAJOR 4u
#define PROPS_PROTOCOL_MINOR 3u
#define PROPS_INTERFACE_SPINEL 3u
#define PROPS_RCP_API_VERSION 4u
#define PROPS_RCP_MIN_HOST_API 4u
#define PROPS_CCA_DEFAULT_DBM (-71) /* the captured PHY_CCA_THRESHOLD of a Silabs RCP */
#define PROPS_TX_POWER_NO_CAP 127   /* a session without an identity caps nothing */
#define PROPS_CHANNEL_26 26u
#define PROPS_RSSI_UNKNOWN (-128)

/* The CAPS list, in the order OpenThread is happy to see it. Every entry is true of this
 * firmware: it is an RCP that streams raw frames, and it implements the three RCP-channel
 * properties. Nothing else is promised (no scan, no allowlist, no network/log caps). */
static const uint32_t propsCaps[] = {PROPS_CAP_CONFIG_RADIO,
                                     PROPS_CAP_RCP_API_VERSION,
                                     PROPS_CAP_RCP_MIN_HOST_API_VERSION,
                                     PROPS_CAP_RCP_LOG_CRASH_DUMP,
                                     PROPS_CAP_MAC_RAW};

const Props_Desc *Props_Table(uint16_t *count)
{
    if (count != 0)
        *count = (uint16_t)(sizeof(propsTable) / sizeof(propsTable[0]));

    return propsTable;
}

const Props_Desc *Props_Find(uint32_t id)
{
    uint16_t i;

    for (i = 0; i < (uint16_t)(sizeof(propsTable) / sizeof(propsTable[0])); ++i)
    {
        if (propsTable[i].id == (uint16_t)id)
            return &propsTable[i];
    }

    return 0;
}

void Props_Init(Props_State *s, const Props_Identity *id)
{
    if (s == 0)
        return;

    s->phyEnabled = 0u; /* docs/protocol.md §10 */
    s->channel = 0u;
    s->channelWritten = 0u;
    s->ccaThresholdDbm = (int8_t)PROPS_CCA_DEFAULT_DBM;
    s->txPowerDbm = 0;
    s->txPowerMaxDbm = (id != 0) ? id->txPowerMaxDbm : (int8_t)PROPS_TX_POWER_NO_CAP;
    s->txPowerMaxCh26Dbm = (id != 0) ? id->txPowerMaxCh26Dbm : (int8_t)PROPS_TX_POWER_NO_CAP;
    s->rssiDbm = (int8_t)PROPS_RSSI_UNKNOWN;
    s->rxSensitivityDbm = (id != 0) ? id->rxSensitivityDbm : (int8_t)-96;
    s->panId = 0xFFFFu;
    s->shortAddr = 0xFFFEu;
    {
        /* Through a volatile pointer: a plain zero loop becomes a libc memset to GCC,
         * and a -nostdlib image has none. */
        volatile uint8_t *p = s->extAddr;
        uint8_t i;

        for (i = 0; i < 8u; ++i)
            p[i] = 0u;
    }
#ifdef RCP_THREAD
    MacSec_Init(&s->macSec);
    s->radioTimeUs = 0u;
#endif
    s->rawStreamEnabled = 0u;
    s->rxOnWhenIdle = 0u;
    s->promiscuousMode = 0u;
    /* The scan family defaults to what upstream's RCP build reports, so a host that never writes a
     * scan property still GETs the list and dwell it expects (docs/protocol.md §10.1). */
    s->scanState = PROPS_SCAN_STATE_IDLE;
    s->scanMask = PROPS_SCAN_MASK_DEFAULT;
    s->scanPeriodMs = PROPS_SCAN_PERIOD_DEFAULT;
    s->scanStart = 0u;
    /* An empty, disabled table: a host that writes nothing gets the standard's fallback, which
     * is "pending", not "nothing queued" (docs/protocol.md §10.2). */
    SrcMatch_Init(&s->srcMatch);
    s->ncpVersion = (id != 0 && id->ncpVersion != 0) ? id->ncpVersion : "dspinelless";
    s->hwAddr = (id != 0) ? id->hwAddr : 0;
}

void Props_SetRssi(Props_State *s, int8_t dbm)
{
    if (s != 0)
        s->rssiDbm = dbm;
}

int8_t Props_TxPowerDbm(const Props_State *s, uint8_t channel)
{
    int8_t cap;

    if (s == 0)
        return 0;

    cap = (channel == PROPS_CHANNEL_26) ? s->txPowerMaxCh26Dbm : s->txPowerMaxDbm;
    return (s->txPowerDbm > cap) ? cap : s->txPowerDbm;
}

/* What a dwell needs from the mask: one channel, and a channel this silicon has. Upstream checks
 * the first half at the same point and leaves the second to its radio driver (link_raw.cpp:221
 * EnergyScan -> the platform's channel validation); both are answered here so a SET that will
 * never measure is refused on the spot rather than confirmed and then abandoned. */
static int propsMaskIsOneBandChannel(uint32_t mask)
{
    uint8_t ch;

    if (mask == 0u || (mask & (mask - 1u)) != 0u)
        return 0; /* empty, or more than one channel */

    for (ch = PROPS_CHAN_MIN; ch <= PROPS_CHAN_MAX; ++ch)
    {
        if (mask == (1uL << ch))
            return 1;
    }

    return 0;
}

int Props_TakeScanStart(Props_State *s, uint32_t *mask, uint16_t *dwellMs)
{
    if (s == 0 || s->scanStart == 0u)
        return 0;

    s->scanStart = 0u;

    if (mask != 0)
        *mask = s->scanMask;
    if (dwellMs != 0)
        *dwellMs = s->scanPeriodMs;

    return 1;
}

void Props_SetScanState(Props_State *s, uint8_t state)
{
    if (s != 0)
        s->scanState = state;
}

uint32_t Props_Get(const Props_State *s, uint32_t id, U_BStream *dst)
{
    uint8_t packed[5];
    uint16_t n;
    const char *str;
    uint8_t i;

    if (s == 0 || dst == 0)
    {
        return SPINEL_STATUS_INVALID_ARG;
    }

    if (Props_Find(id) == 0)
    {
        return SPINEL_STATUS_PROP_NOT_FOUND;
    }

    switch (id)
    {
#ifdef RCP_THREAD
        case PROPS_ID_RCP_TIMESTAMP:
            U_bstream_put_u64_le(dst, s->radioTimeUs);
            break;
#endif

#ifdef RCP_TX_TIMING
        case PROPS_ID_RCP_CSL_ACCURACY:
            U_bstream_put_u8(dst, RCP_CSL_ACCURACY_PPM);
            break;

        case PROPS_ID_RCP_CSL_UNCERTAINTY:
            U_bstream_put_u8(dst, RCP_CSL_UNCERTAINTY_10US);
            break;
#endif

        case PROPS_ID_PROTOCOL_VERSION:
            /* 'ii': two packed uints, exactly the captured `04 03` (docs/protocol.md §8). */
            n = Spinel_PackedWrite(packed, sizeof(packed), PROPS_PROTOCOL_MAJOR);
            U_bstream_put_bytes(dst, packed, n);
            n = Spinel_PackedWrite(packed, sizeof(packed), PROPS_PROTOCOL_MINOR);
            U_bstream_put_bytes(dst, packed, n);
            break;

        case PROPS_ID_NCP_VERSION:
            /* 'U': the terminator is part of the value. */
            str = s->ncpVersion;

            while (*str != '\0')
                U_bstream_put_u8(dst, (uint8_t)*str++);

            U_bstream_put_u8(dst, 0u);
            break;

        case PROPS_ID_INTERFACE_TYPE:
            U_bstream_put_u8(dst, PROPS_INTERFACE_SPINEL);
            break;

        case PROPS_ID_RCP_API_VERSION:
            U_bstream_put_u8(dst, PROPS_RCP_API_VERSION);
            break;

        case PROPS_ID_RCP_MIN_HOST_API:
            U_bstream_put_u8(dst, PROPS_RCP_MIN_HOST_API);
            break;

        case PROPS_ID_CAPS:
            /* A list of packed uints, one capability number each: OpenThread pushes every
             * element and later tests membership (spinel_driver.cpp:416-426,
             * spinel_driver.hpp:206), so a bitmask would advertise nothing. */
            for (i = 0; i < (uint8_t)(sizeof(propsCaps) / sizeof(propsCaps[0])); ++i)
            {
                n = Spinel_PackedWrite(packed, sizeof(packed), propsCaps[i]);
                U_bstream_put_bytes(dst, packed, n);
            }
            break;

        case PROPS_ID_RADIO_CAPS:
            n = Spinel_PackedWrite(packed, sizeof(packed), PROPS_RADIO_CAPS_VALUE);
            U_bstream_put_bytes(dst, packed, n);
            break;

        case PROPS_ID_LAST_STATUS:
            U_bstream_put_u8(dst, SPINEL_STATUS_OK);
            break;

        case PROPS_ID_PHY_ENABLED:
            U_bstream_put_u8(dst, s->phyEnabled);
            break;
        case PROPS_ID_PHY_CHAN_SUPPORTED:
        case PROPS_ID_PHY_CHAN_PREFERRED:
            /* Upstream's shape (ncp_base.cpp EncodeChannelMask): the numbers and nothing else, which
             * RadioSpinel reads to the end of the value. Every board has the whole band. */
            for (i = PROPS_CHAN_MIN; i <= PROPS_CHAN_MAX; ++i)
                U_bstream_put_u8(dst, i);
            break;

        case PROPS_ID_PHY_CHAN:
            U_bstream_put_u8(dst, s->channel);
            break;
        case PROPS_ID_PHY_CCA_THRESHOLD:
            U_bstream_put_u8(dst, (uint8_t)s->ccaThresholdDbm);
            break;
        case PROPS_ID_PHY_TX_POWER:
            U_bstream_put_u8(dst, (uint8_t)Props_TxPowerDbm(s, s->channel));
            break;
        case PROPS_ID_PHY_RSSI:
            U_bstream_put_u8(dst, (uint8_t)s->rssiDbm);
            break;
        case PROPS_ID_PHY_RX_SENSITIVITY:
            U_bstream_put_u8(dst, (uint8_t)s->rxSensitivityDbm);
            break;

        case PROPS_ID_MAC_PANID:
            U_bstream_put_u8(dst, (uint8_t)(s->panId & 0xFFu));
            U_bstream_put_u8(dst, (uint8_t)(s->panId >> 8));
            break;
        case PROPS_ID_MAC_SADDR:
            U_bstream_put_u8(dst, (uint8_t)(s->shortAddr & 0xFFu));
            U_bstream_put_u8(dst, (uint8_t)(s->shortAddr >> 8));
            break;
        case PROPS_ID_HWADDR:
            /* All or nothing: a part with no programmed user row has no factory address. */
            if (s->hwAddr == 0)
            {
                return SPINEL_STATUS_INVALID_STATE;
            }

            for (i = 0; i < 8u; ++i)
                U_bstream_put_u8(dst, s->hwAddr[i]);

            break;
        case PROPS_ID_MAC_LADDR:
            /* 'E': big-endian (docs/protocol.md §4). */
            for (i = 0; i < 8u; ++i)
                U_bstream_put_u8(dst, s->extAddr[i]);

            break;
        case PROPS_ID_MAC_RAW_STREAM:
            U_bstream_put_u8(dst, s->rawStreamEnabled);
            break;
        case PROPS_ID_MAC_RX_ON_WHEN_IDLE:
            U_bstream_put_u8(dst, s->rxOnWhenIdle);
            break;
        case PROPS_ID_RCP_LOG_CRASH_DUMP:
            U_bstream_put_u8(dst, s->logEnabled);
            break;

        case PROPS_ID_MAC_PROMISCUOUS:
            U_bstream_put_u8(dst, s->promiscuousMode);
            break;

        case PROPS_ID_MAC_SRC_MATCH_ENABLED:
            U_bstream_put_u8(dst, s->srcMatch.enabled);
            break;

        case PROPS_ID_MAC_SRC_MATCH_SHORT:
            /* 'A(S)': the stored list, little endian, no count prefix. Upstream has no GET
             * handler for this id, so a host reads it back only from this firmware; the SET echo
             * below is what a conformant host sees either way. */
            if (SrcMatch_EncodeShort(&s->srcMatch, dst) == 0u && s->srcMatch.shortCount != 0u)
            {
                return SPINEL_STATUS_NOMEM;
            }

            break;

        case PROPS_ID_MAC_SRC_MATCH_EXT:
            if (SrcMatch_EncodeExt(&s->srcMatch, dst) == 0u && s->srcMatch.extCount != 0u)
            {
                return SPINEL_STATUS_NOMEM;
            }

            break;

        case PROPS_ID_MAC_SCAN_STATE:
            U_bstream_put_u8(dst, s->scanState);
            break;

        case PROPS_ID_MAC_SCAN_MASK:
        {
            /* 'A(C)': one count byte, then that many channel numbers, ascending. The list is a
             * convenience for stating which channels a host cares about; it is not a claim that one
             * SET measures all of them (docs/protocol.md §10.1). */
            uint8_t list[PROPS_SCAN_MAX_LIST];
            uint8_t count = 0u;
            uint8_t ch;

            /* Every bit the host stored, in order - including a channel this silicon does not have.
             * Dropping it here would make a SET and its own echo disagree, and the refusal for such a
             * channel belongs to the dwell that would try to measure it. */
            for (ch = 0u; ch < 32u; ++ch)
            {
                if ((s->scanMask & (1uL << ch)) != 0u)
                    list[count++] = ch;
            }

            U_bstream_put_u8(dst, count);

            for (i = 0u; i < count; ++i)
                U_bstream_put_u8(dst, list[i]);

            break;
        }

        case PROPS_ID_MAC_SCAN_PERIOD:
            U_bstream_put_u8(dst, (uint8_t)(s->scanPeriodMs & 0xFFu));
            U_bstream_put_u8(dst, (uint8_t)(s->scanPeriodMs >> 8));
            break;

        default:
            return SPINEL_STATUS_PROP_NOT_FOUND;
    }

    return SPINEL_STATUS_OK;
}

uint32_t Props_Set(Props_State *s, uint32_t id, const uint8_t *value, uint16_t len)
{
    const Props_Desc *desc;
    uint16_t v16;
    uint8_t i;

    if (s == 0)
    {
        return SPINEL_STATUS_INVALID_ARG;
    }

    desc = Props_Find(id);

    if (desc == 0)
    {
        return SPINEL_STATUS_PROP_NOT_FOUND;
    }

    if ((desc->access & PROPS_ACC_WRITE) == 0u)
    {
        return SPINEL_STATUS_UNIMPLEMENTED;
    }

#ifdef RCP_THREAD
    if (id == PROPS_ID_RCP_MAC_KEY)
        return MacSec_SetKeys(&s->macSec, value, len) ? SPINEL_STATUS_OK : SPINEL_STATUS_INVALID_ARG;

    if (id == PROPS_ID_RCP_MAC_FRAME_COUNTER)
        return MacSec_SetFrameCounter(&s->macSec, value, len) ? SPINEL_STATUS_OK : SPINEL_STATUS_INVALID_ARG;

    if (id == PROPS_ID_RCP_ENH_ACK_PROBING)
        return MacSec_SetProbe(&s->macSec, value, len) ? SPINEL_STATUS_OK : SPINEL_STATUS_INVALID_ARG;
#endif

    /* A u8 payload sets the record stream. An empty write is OpenThread's request for a crash dump, made
     * on every start (radio_spinel.cpp): there is none, and it must not start a stream at a host that did
     * not ask for one. So it is answered before the fixed-size type check below. */
    if (id == PROPS_ID_RCP_LOG_CRASH_DUMP)
    {
        uint8_t on = s->logEnabled;

        if (!PropsDebug_EnableValue(value, len, &on))
        {
            return SPINEL_STATUS_INVALID_ARG;
        }

        s->logEnabled = on;
        return SPINEL_STATUS_OK;
    }

    /* MAC_SCAN_MASK is the one writable value whose length is not fixed, so it is decoded before the
     * fixed-size rule below. Two shapes arrive: upstream's bare channel numbers (ncp_base.cpp
     * DecodeChannelMask, what RadioSpinel and zigbee-on-host send), and this firmware's count byte
     * followed by the numbers, which Phoscon Zigbee Stack sends. A first octet equal to the count
     * of what follows is the counted form; otherwise a first octet in the band is the bare form. A
     * bare list of twelve channels from 11 would read as counted, and no host sends one: RadioSpinel
     * writes one channel at a time. The channel bound is upstream's: a number above 31 is refused. */
    if (id == PROPS_ID_MAC_SCAN_MASK)
    {
        uint16_t k;
        uint16_t first = 0u;
        uint32_t mask = 0u;

        if (value == 0 && len != 0u)
        {
            return SPINEL_STATUS_INVALID_ARG;
        }

        if (len > 0u)
        {
            if ((uint16_t)value[0] + 1u == len)
                first = 1u;
            else if (value[0] < PROPS_CHAN_MIN || value[0] > PROPS_CHAN_MAX)
                return SPINEL_STATUS_INVALID_ARG; /* neither shape: a count that does not match */
        }

        if ((uint16_t)(len - first) > PROPS_SCAN_MAX_LIST)
        {
            return SPINEL_STATUS_INVALID_ARG;
        }

        for (k = first; k < len; ++k)
        {
            if (value[k] > 31u)
            {
                return SPINEL_STATUS_INVALID_ARG; /* a channel number the wire format cannot carry */
            }

            mask |= (1uL << value[k]);
        }

        s->scanMask = mask;
        return SPINEL_STATUS_OK;
    }

    /* The two address lists are length-decoded like MAC_SCAN_MASK: 'A(S)' and 'A(E)' carry no
     * count, so the length is the only statement of how many entries there are. */
    if (id == PROPS_ID_MAC_SRC_MATCH_SHORT || id == PROPS_ID_MAC_SRC_MATCH_EXT)
    {
        int rc;

        if (value == 0 && len != 0u)
        {
            return SPINEL_STATUS_INVALID_ARG;
        }

        if (id == PROPS_ID_MAC_SRC_MATCH_SHORT)
            rc = SrcMatch_SetShortList(&s->srcMatch, value, len);
        else
            rc = SrcMatch_SetExtList(&s->srcMatch, value, len);

        if (rc == SRCMATCH_BAD_LEN)
        {
            return SPINEL_STATUS_INVALID_ARG;
        }

        if (rc == SRCMATCH_FULL)
        {
            return SPINEL_STATUS_NOMEM;
        }

        return SPINEL_STATUS_OK;
    }

    /* Writable properties are fixed-size scalars ('b'/'C'/'c'/'S'/'E'). */
    switch (desc->type)
    {
        case PROPS_T_BOOL:
        case PROPS_T_U8:
        case PROPS_T_I8:
            if (len != 1u || value == 0)
            {
                return SPINEL_STATUS_INVALID_ARG;
            }

            break;
        case PROPS_T_U16:
            if (len != 2u || value == 0)
            {
                return SPINEL_STATUS_INVALID_ARG;
            }

            break;
        case PROPS_T_BE64:
            if (len != 8u || value == 0)
            {
                return SPINEL_STATUS_INVALID_ARG;
            }

            break;
        default:
            return SPINEL_STATUS_UNIMPLEMENTED;
    }

    switch (id)
    {
        case PROPS_ID_PHY_ENABLED:
            s->phyEnabled = (value[0] != 0u) ? 1u : 0u;
            break;

        case PROPS_ID_PHY_CHAN:
            /* docs/protocol.md §10: Zigbee channels 11..26. */
            if (value[0] < PROPS_CHAN_MIN || value[0] > PROPS_CHAN_MAX)
            {
                return SPINEL_STATUS_INVALID_ARG;
            }

            s->channel = value[0];
            s->channelWritten = 1u;
            break;

        case PROPS_ID_PHY_CCA_THRESHOLD:
            s->ccaThresholdDbm = (int8_t)value[0];
            break;
        case PROPS_ID_PHY_TX_POWER:
            s->txPowerDbm = (int8_t)value[0];
            break;

        case PROPS_ID_MAC_PANID:
            s->panId = (uint16_t)((uint16_t)value[0] | ((uint16_t)value[1] << 8));
            break;
        case PROPS_ID_MAC_SADDR:
            s->shortAddr = (uint16_t)((uint16_t)value[0] | ((uint16_t)value[1] << 8));
            break;
        case PROPS_ID_MAC_LADDR:
            for (i = 0; i < 8u; ++i)
                s->extAddr[i] = value[i];

            break;

        case PROPS_ID_MAC_RAW_STREAM:
            s->rawStreamEnabled = (value[0] != 0u) ? 1u : 0u;
            break;
        case PROPS_ID_MAC_RX_ON_WHEN_IDLE:
            s->rxOnWhenIdle = (value[0] != 0u) ? 1u : 0u;
            break;

        case PROPS_ID_MAC_SRC_MATCH_ENABLED:
            /* Off is not "no source match": it is the standard's fallback, every Ack to a Data
             * Request claiming pending (docs/protocol.md §10.2). */
            s->srcMatch.enabled = (value[0] != 0u) ? 1u : 0u;
            s->srcMatch.changed = 1u;
            break;

        case PROPS_ID_MAC_PROMISCUOUS:
            /* docs/protocol.md §10: 0 off, 1 network, 2 full. */
            if (value[0] > 2u)
            {
                return SPINEL_STATUS_INVALID_ARG;
            }

            s->promiscuousMode = value[0];
            break;

        case PROPS_ID_MAC_SCAN_STATE:
            /* The arms are upstream's, in upstream's order (link_raw.cpp:181-219): the kinds an RCP
             * build does not implement answer UNIMPLEMENTED, a cancel is accepted and does nothing,
             * and a start is refused for a state that cannot hold one or a mask it cannot dwell on
             * (docs/protocol.md §10.1). */
            if (value[0] == PROPS_SCAN_STATE_BEACON || value[0] == PROPS_SCAN_STATE_DISCOVER ||
                value[0] > PROPS_SCAN_STATE_DISCOVER)
            {
                return SPINEL_STATUS_UNIMPLEMENTED;
            }

            if (value[0] == PROPS_SCAN_STATE_IDLE)
                return SPINEL_STATUS_OK;

            if (s->phyEnabled == 0u || s->scanState == PROPS_SCAN_STATE_ENERGY)
                return SPINEL_STATUS_INVALID_STATE;

            if (propsMaskIsOneBandChannel(s->scanMask) == 0)
                return SPINEL_STATUS_INVALID_ARG;

            s->scanStart = 1u;
            s->scanState = PROPS_SCAN_STATE_ENERGY;
            return SPINEL_STATUS_OK;

        case PROPS_ID_MAC_SCAN_PERIOD:
            /* Stored verbatim, as upstream stores it (ncp_base.cpp:1795 ReadUint16 with no range
             * check): what GET reports is what the host wrote, and the bound belongs to the dwell
             * that uses it (rcp/scan.h SCAN_PERIOD_MAX_MS). */
            s->scanPeriodMs = (uint16_t)((uint16_t)value[0] | ((uint16_t)value[1] << 8));
            break;

        default:
            v16 = 0u;
            (void)v16;
            return SPINEL_STATUS_UNIMPLEMENTED;
    }

    return SPINEL_STATUS_OK;
}

/* The status a SrcMatch_* refusal becomes. NOMEM is upstream's answer for a full table
 * (OT_ERROR_NO_BUFS -> SPINEL_STATUS_NOMEM) and ITEM_NOT_FOUND for an entry that is not there
 * (OT_ERROR_NOT_FOUND -> SPINEL_STATUS_ITEM_NOT_FOUND, ncp_base.cpp ThreadErrorToSpinelStatus). */
static uint32_t propsSrcMatchStatus(int rc)
{
    if (rc == SRCMATCH_OK)
        return SPINEL_STATUS_OK;

    if (rc == SRCMATCH_FULL)
        return SPINEL_STATUS_NOMEM;

    if (rc == SRCMATCH_ABSENT)
        return SPINEL_STATUS_ITEM_NOT_FOUND;

    return SPINEL_STATUS_INVALID_ARG;
}

uint32_t Props_Insert(Props_State *s, uint32_t id, const uint8_t *value, uint16_t len)
{
    if (s == 0)
    {
        return SPINEL_STATUS_INVALID_ARG;
    }

    if (id == PROPS_ID_MAC_SRC_MATCH_SHORT)
    {
        if (value == 0 || len != 2u)
        {
            return SPINEL_STATUS_INVALID_ARG;
        }

        return propsSrcMatchStatus(
            SrcMatch_InsertShort(&s->srcMatch, (uint16_t)((uint16_t)value[0] | ((uint16_t)value[1] << 8))));
    }

    if (id == PROPS_ID_MAC_SRC_MATCH_EXT)
    {
        if (value == 0 || len != SRCMATCH_EXT_LEN)
        {
            return SPINEL_STATUS_INVALID_ARG;
        }

        return propsSrcMatchStatus(SrcMatch_InsertExt(&s->srcMatch, value));
    }

    return SPINEL_STATUS_PROP_NOT_FOUND;
}

uint32_t Props_Remove(Props_State *s, uint32_t id, const uint8_t *value, uint16_t len)
{
    if (s == 0)
    {
        return SPINEL_STATUS_INVALID_ARG;
    }

    if (id == PROPS_ID_MAC_SRC_MATCH_SHORT)
    {
        if (value == 0 || len != 2u)
        {
            return SPINEL_STATUS_INVALID_ARG;
        }

        return propsSrcMatchStatus(
            SrcMatch_RemoveShort(&s->srcMatch, (uint16_t)((uint16_t)value[0] | ((uint16_t)value[1] << 8))));
    }

    if (id == PROPS_ID_MAC_SRC_MATCH_EXT)
    {
        if (value == 0 || len != SRCMATCH_EXT_LEN)
        {
            return SPINEL_STATUS_INVALID_ARG;
        }

        return propsSrcMatchStatus(SrcMatch_RemoveExt(&s->srcMatch, value));
    }

    return SPINEL_STATUS_PROP_NOT_FOUND;
}
