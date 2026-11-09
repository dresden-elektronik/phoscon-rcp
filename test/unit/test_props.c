/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* test_props.c — the property table's suite (docs/testing.md §2): table invariants, then
 * a form-network sequence captured from a Silabs EFR32 RCP, each value on the wire. */
#include "props.h"
#include "spinel.h"
#include "srcmatch.h"
#include "tu.h"
#include "u_bstream.h"

#define BUF 64

static uint8_t buf[BUF];

static uint16_t getValue(Props_State *s, uint32_t id)
{
    U_BStream bs;

    U_bstream_init(&bs, buf, sizeof(buf));
    TU_EQ(Props_Get(s, id, &bs), SPINEL_STATUS_OK);
    return (uint16_t)(BUF - U_bstream_remaining(&bs));
}

/* CAPS is a list of packed uints, one capability number each, and OpenThread pushes every
 * element and later tests membership (spinel_driver.cpp:416-426, spinel_driver.hpp:206).
 * Decode it the same way, so a bitmask would fail this and not just the host. */
static int capsContains(const uint8_t *data, uint16_t len, uint32_t cap)
{
    uint16_t pos = 0u;

    while (pos < len)
    {
        uint32_t v = 0u;
        uint16_t used = 0u;

        if (!Spinel_PackedRead(&data[pos], (uint16_t)(len - pos), &v, &used))
            return 0;

        if (v == cap)
            return 1;

        pos = (uint16_t)(pos + used);
    }

    return 0;
}

/* One constructor, all fields, no exceptions: a partially filled Props_Identity is an
 * uninitialised pointer read in the GET loop below. */
static const uint8_t factoryEui64[8] = {0x00u, 0x21u, 0x2eu, 0xffu, 0xffu, 0x0eu, 0x21u, 0x2eu};

static void initIdentity(Props_Identity *id)
{
    id->ncpVersion = "DSPINEL/1.0; AT86RF233";
    id->rxSensitivityDbm = -96; /* AT86RF233 per docs/protocol.md §10 */
    id->txPowerMaxDbm = 127;    /* no cap: the cap has its own tests */
    id->txPowerMaxCh26Dbm = 127;
    id->hwAddr = factoryEui64;  /* the factory address a real board reports */
}

TU_TEST(mac_property_ids_match_upstream_spinel)
{
    /* Pinned as literals: docs/protocol.md §10 disagreed with captures and
   * with upstream spinel.h - the MAC addresses start at 0x34, and a drift is a silent
   * PROP_NOT_FOUND against a real host (docs/protocol.md §10). */
    TU_EQ(PROPS_ID_MAC_LADDR, 0x34u);
    TU_EQ(PROPS_ID_MAC_SADDR, 0x35u);
    TU_EQ(PROPS_ID_MAC_PANID, 0x36u);
    TU_EQ(PROPS_ID_MAC_RAW_STREAM, 0x37u);
    TU_EQ(PROPS_ID_MAC_PROMISCUOUS, 0x38u);
    TU_EQ(PROPS_ID_MAC_RX_ON_WHEN_IDLE, 0x3Bu);
    /* ... and the PHY block, which the captures also use (0x20 enabled, 0x21 channel,
   * 0x24 CCA threshold, 0x25 tx power, 0x26 RSSI, 0x27 sensitivity). */
    TU_EQ(PROPS_ID_PHY_ENABLED, 0x20u);
    TU_EQ(PROPS_ID_PHY_CHAN, 0x21u);
    TU_EQ(PROPS_ID_PHY_CCA_THRESHOLD, 0x24u);
    TU_EQ(PROPS_ID_PHY_TX_POWER, 0x25u);
    TU_EQ(PROPS_ID_PHY_RSSI, 0x26u);
    TU_EQ(PROPS_ID_PHY_RX_SENSITIVITY, 0x27u);
    TU_EQ(PROPS_ID_PROTOCOL_VERSION, 1u);
    TU_EQ(PROPS_ID_NCP_VERSION, 2u);
    TU_EQ(PROPS_ID_INTERFACE_TYPE, 3u);
    TU_EQ(PROPS_ID_RCP_API_VERSION, 0xB0u);
    TU_EQ(PROPS_ID_RCP_MIN_HOST_API, 0xB1u);
}

TU_TEST(table_ids_are_unique_and_bounded)
{
    const Props_Desc *t = Props_Table(0);
    uint16_t n = 0u;
    uint16_t i;
    uint16_t j;

    (void)Props_Table(&n);
    TU_CHECK(n > 10u); /* docs/protocol.md §10's surface, not a stub */

    for (i = 0; i < n; ++i)
    {
        TU_CHECK(t[i].id <= SPINEL_PROP_MAX);

        for (j = (uint16_t)(i + 1u); j < n; ++j)
        {
            TU_CHECK(t[i].id != t[j].id); /* a duplicate id would shadow */
        }
    }
}

TU_TEST(every_writable_property_is_readable)
{
    /* docs/protocol.md §5: a SET is echoed only if the property has a GET handler - so every
   * accepted SET echoable. The MAC security properties are the exception, on purpose: they would
   * echo a key. */
    const Props_Desc *t = Props_Table(0);
    uint16_t n = 0u;
    uint16_t i;

    (void)Props_Table(&n);

    for (i = 0; i < n; ++i)
    {
        if ((t[i].access & PROPS_ACC_WRITE) != 0u && t[i].type != PROPS_T_MAC_SEC)
            TU_CHECK((t[i].access & PROPS_ACC_READ) != 0u);
    }
}

TU_TEST(every_table_entry_answers_a_get)
{
    Props_Identity id;
    Props_State s;
    const Props_Desc *t = Props_Table(0);
    uint16_t n = 0u;
    uint16_t i;

    initIdentity(&id);
    Props_Init(&s, &id);
    (void)Props_Table(&n);

    for (i = 0; i < n; ++i)
    {
        uint16_t len;

        if ((t[i].access & PROPS_ACC_READ) == 0u)
            continue;

        len = getValue(&s, t[i].id);

        switch (t[i].type)
        {
            case PROPS_T_BOOL:
            case PROPS_T_U8:
            case PROPS_T_I8:
                TU_EQ(len, 1u);
                break;
            case PROPS_T_U16:
                TU_EQ(len, 2u);
                break;
            case PROPS_T_BE64:
                TU_EQ(len, 8u);
                break;
            case PROPS_T_PACKED1:
            case PROPS_T_CAP_LIST:
                TU_CHECK(len >= 1u); /* 'i' and 'A(i)': at least one byte */
                break;
            case PROPS_T_ADDR_LIST_SHORT:
            case PROPS_T_ADDR_LIST_EXT:
                /* 'A(S)' and 'A(E)': an empty table is a legitimate empty value, and a full
                 * one is still inside the reply buffer. */
                TU_CHECK(len <= SRCMATCH_SHORT_MAX * 2u);
                TU_EQ(len % (t[i].type == PROPS_T_ADDR_LIST_SHORT ? 2u : 8u), 0u);
                break;
            default:
                TU_CHECK(len >= 1u); /* 'ii' and 'U': at least one byte */
                break;
        }
    }
}

/* Props_Init is the power-on state, and a RESET runs it on a state that has lived: nothing of what was
 * there before may show. A state full of garbage and a zeroed one must read back the same, property by
 * property, and the record stream a host had switched on is off again. */
TU_TEST(init_leaves_nothing_of_what_was_there_before)
{
    static const uint8_t on = 1u;
    Props_Identity id;
    Props_State dirty;
    Props_State clean;
    const Props_Desc *t = Props_Table(0);
    uint16_t n = 0u;
    uint16_t i;
    uint16_t k;

    initIdentity(&id);

    for (k = 0u; k < sizeof(dirty); ++k)
    {
        ((uint8_t *)&dirty)[k] = 0xA5u;
        ((uint8_t *)&clean)[k] = 0x00u;
    }

    Props_Init(&dirty, &id);
    Props_Init(&clean, &id);
    (void)Props_Table(&n);

    for (i = 0; i < n; ++i)
    {
        uint8_t a[64];
        uint8_t b[64];
        U_BStream sa;
        U_BStream sb;

        if ((t[i].access & PROPS_ACC_READ) == 0u)
            continue;

        U_bstream_init(&sa, a, sizeof(a));
        U_bstream_init(&sb, b, sizeof(b));
        TU_EQ(Props_Get(&dirty, t[i].id, &sa), Props_Get(&clean, t[i].id, &sb));
        TU_EQ(U_bstream_remaining(&sa), U_bstream_remaining(&sb));

        for (k = 0u; k < (uint16_t)(sizeof(a) - U_bstream_remaining(&sa)); ++k)
        {
            if (a[k] != b[k])
            {
                TU_NOTE("property differs after init", t[i].id);
                TU_CHECK(0);
                break;
            }
        }
    }

    TU_EQ(dirty.logEnabled, 0u);

    /* A host turned the record stream on; a RESET turns it off. */
    TU_EQ(Props_Set(&clean, PROPS_ID_RCP_LOG_CRASH_DUMP, &on, 1u), SPINEL_STATUS_OK);
    TU_EQ(clean.logEnabled, 1u);
    Props_Init(&clean, &id);
    TU_EQ(clean.logEnabled, 0u);
}

TU_TEST(protocol_version_is_the_captured_04_03)
{
    Props_Identity id;
    Props_State s;

    initIdentity(&id);
    id.ncpVersion = "DSPINEL/1.0";
    Props_Init(&s, &id);
    TU_EQ(getValue(&s, PROPS_ID_PROTOCOL_VERSION), 2u);
    TU_EQ(buf[0], 4u); /* 'ii' = major, minor = 4.3 */
    TU_EQ(buf[1], 3u);
}

TU_TEST(ncp_version_is_zero_terminated_and_self_identifying)
{
    Props_Identity id;
    Props_State s;
    uint16_t len;

    initIdentity(&id);
    Props_Init(&s, &id);
    len = getValue(&s, PROPS_ID_NCP_VERSION);
    TU_CHECK(len >= 3u);
    TU_EQ(buf[len - 1u], 0u); /* 'U' includes its terminator */
    TU_EQ(buf[0], (uint8_t)'D');
}

TU_TEST(interface_type_is_spinel)
{
    Props_Identity id;
    Props_State s;

    initIdentity(&id);
    Props_Init(&s, &id);
    TU_EQ(getValue(&s, PROPS_ID_INTERFACE_TYPE), 1u);
    TU_EQ(buf[0], 3u); /* docs/protocol.md §10: 3 = SPINEL, which zoh expects */
}

TU_TEST(caps_property_ids_match_upstream_spinel)
{
    /* Pinned as literals: a drift is a silent PROP_NOT_FOUND against a real host. */
    TU_EQ(PROPS_ID_CAPS, 5u);             /* SPINEL_PROP_CAPS, spinel.h:1428 */
    TU_EQ(PROPS_ID_RADIO_CAPS, 0x120Bu);  /* PHY_EXT 0x1200 + 11, spinel.h:1895 */
}

TU_TEST(caps_lists_exactly_the_true_capabilities)
{
    Props_Identity id;
    Props_State s;
    uint16_t len;
    uint16_t pos = 0u;
    uint16_t count = 0u;

    initIdentity(&id);
    Props_Init(&s, &id);
    len = getValue(&s, PROPS_ID_CAPS);
    TU_CHECK(len >= 1u);

    /* Every element must be one of the five we mean to advertise: the value is a list of
   * capability numbers, not a bitfield. */
    while (pos < len)
    {
        uint32_t v = 0u;
        uint16_t used = 0u;

        TU_CHECK(Spinel_PackedRead(&buf[pos], (uint16_t)(len - pos), &v, &used));
        TU_CHECK(used >= 1u);
        pos = (uint16_t)(pos + used);
        count++;

        switch (v)
        {
            case PROPS_CAP_CONFIG_RADIO:
            case PROPS_CAP_RCP_API_VERSION:
            case PROPS_CAP_RCP_MIN_HOST_API_VERSION:
            case PROPS_CAP_RCP_LOG_CRASH_DUMP:
            case PROPS_CAP_MAC_RAW:
                break;
            default:
                TU_CHECK(0); /* an advertised capability that is not true of this firmware */
                break;
        }
    }

    TU_EQ(count, 5u);
    TU_CHECK(capsContains(buf, len, PROPS_CAP_CONFIG_RADIO));
    TU_CHECK(capsContains(buf, len, PROPS_CAP_MAC_RAW));
    TU_CHECK(capsContains(buf, len, PROPS_CAP_RCP_API_VERSION));
    TU_CHECK(capsContains(buf, len, PROPS_CAP_RCP_MIN_HOST_API_VERSION));
    TU_CHECK(capsContains(buf, len, PROPS_CAP_RCP_LOG_CRASH_DUMP));
    /* The capabilities we deliberately do not have: no whitelist, no network, no logging,
   * no second config, no bootloader reset. */
    TU_CHECK(!capsContains(buf, len, 32u));  /* CONFIG_FTD */
    TU_CHECK(!capsContains(buf, len, 33u));  /* CONFIG_MTD */
    TU_CHECK(!capsContains(buf, len, 52u));  /* NET_THREAD_1_0 */
    TU_CHECK(!capsContains(buf, len, 54u));  /* NET_THREAD_1_2 */
    TU_CHECK(!capsContains(buf, len, 66u));  /* RCP_RESET_TO_BOOTLOADER */
    TU_CHECK(!capsContains(buf, len, 512u)); /* MAC_ALLOWLIST */
    TU_CHECK(!capsContains(buf, len, 518u)); /* OPENTHREAD_LOG_METADATA */
}

TU_TEST(radio_caps_advertises_the_supported_radio_features)
{
    Props_Identity id;
    Props_State s;
    uint32_t v = 0u;
    uint16_t used = 0u;
    uint16_t len;

    initIdentity(&id);
    Props_Init(&s, &id);
    len = getValue(&s, PROPS_ID_RADIO_CAPS);
    TU_EQ(len, Spinel_PackedSize(PROPS_RADIO_CAPS_VALUE));
    TU_CHECK(Spinel_PackedRead(buf, len, &v, &used));
    TU_EQ(used, len);
    TU_EQ(v, PROPS_RADIO_CAPS_VALUE);
    /* The three OpenThread's CheckRadioCapabilities demands, plus RX-on-when-idle. */
    TU_CHECK((v & PROPS_RADIO_CAP_ACK_TIMEOUT) != 0u);
    TU_CHECK((v & PROPS_RADIO_CAP_TRANSMIT_RETRIES) != 0u);
    TU_CHECK((v & PROPS_RADIO_CAP_CSMA_BACKOFF) != 0u);
    TU_CHECK((v & PROPS_RADIO_CAP_RX_ON_WHEN_IDLE) != 0u);
    /* The byte a host measures, pinned: docs/protocol.md §10. */
#if defined(RCP_TX_TIMING)
    TU_EQ(v, 0x16Fu);
#elif defined(RCP_THREAD)
    TU_EQ(v, 0x12Fu);
#else
    TU_EQ(v, 0x10Fu); /* a board without Thread support advertises exactly what it always did */
#endif
    /* The energy kind of the scan family is implemented, so ENERGY_SCAN is now a promise rather
     * than a gap: a host that sees it will write MAC_SCAN_STATE and expect a scan to happen. */
    TU_CHECK((v & PROPS_RADIO_CAP_ENERGY_SCAN) != 0u); /* ENERGY_SCAN: MAC_SCAN_* are live */
    /* ... and the ones this radio does not do. */
    TU_CHECK((v & (1u << 4)) == 0u);  /* SLEEP_TO_TX */
#ifndef RCP_THREAD
    TU_CHECK((v & (1u << 5)) == 0u); /* TRANSMIT_SEC: no MAC security */
    TU_CHECK(Props_Find(PROPS_ID_RCP_MAC_KEY) == 0);
#endif
#ifdef RCP_TX_TIMING
    TU_CHECK((v & PROPS_RADIO_CAP_TRANSMIT_TIMING) != 0u);
    len = getValue(&s, PROPS_ID_RCP_CSL_ACCURACY);
    TU_EQ(len, 1u);
    TU_EQ(buf[0], RCP_CSL_ACCURACY_PPM);
    len = getValue(&s, PROPS_ID_RCP_CSL_UNCERTAINTY);
    TU_EQ(len, 1u);
    TU_EQ(buf[0], RCP_CSL_UNCERTAINTY_10US);
#else
    TU_CHECK((v & (1u << 6)) == 0u); /* TRANSMIT_TIMING */
    TU_CHECK(Props_Find(PROPS_ID_RCP_CSL_ACCURACY) == 0);
#endif
    TU_CHECK((v & (1u << 7)) == 0u);  /* RECEIVE_TIMING */
    TU_CHECK((v & (1u << 9)) == 0u);  /* TRANSMIT_FRAME_POWER */
    TU_CHECK((v & (1u << 10)) == 0u); /* ALT_SHORT_ADDR */
}

TU_TEST(caps_and_radio_caps_are_read_only)
{
    Props_Identity id;
    Props_State s;
    static const uint8_t v[8] = {0u};

    initIdentity(&id);
    Props_Init(&s, &id);
    TU_EQ(Props_Find(PROPS_ID_CAPS)->access, PROPS_ACC_READ);
    TU_EQ(Props_Find(PROPS_ID_RADIO_CAPS)->access, PROPS_ACC_READ);
    TU_EQ(Props_Set(&s, PROPS_ID_CAPS, v, 1u), SPINEL_STATUS_UNIMPLEMENTED);
    TU_EQ(Props_Set(&s, PROPS_ID_RADIO_CAPS, v, 1u), SPINEL_STATUS_UNIMPLEMENTED);
    /* A refused SET left the GET answer alone. */
    {
        uint16_t len = getValue(&s, PROPS_ID_CAPS);

        TU_CHECK(capsContains(buf, len, PROPS_CAP_MAC_RAW));
    }
}

TU_TEST(sensitivity_comes_from_the_board_identity)
{
    Props_Identity id;
    Props_State s;

    initIdentity(&id); /* AT86RF233 per docs/protocol.md §10 */
    Props_Init(&s, &id);
    TU_EQ(getValue(&s, PROPS_ID_PHY_RX_SENSITIVITY), 1u);
    TU_EQ((int8_t)buf[0], -96);
}

TU_TEST(rssi_is_unknown_until_the_session_reports_one)
{
    Props_Identity id;
    Props_State s;

    initIdentity(&id);
    Props_Init(&s, &id);
    TU_EQ(getValue(&s, PROPS_ID_PHY_RSSI), 1u);
    TU_EQ((int8_t)buf[0], -128); /* not 0: 0 dBm would be a measurement */
    Props_SetRssi(&s, -73);
    TU_EQ(getValue(&s, PROPS_ID_PHY_RSSI), 1u);
    TU_EQ((int8_t)buf[0], -73);
}

TU_TEST(replays_the_captured_set_sequence)
{
    Props_Identity id;
    Props_State s;

    initIdentity(&id);
    Props_Init(&s, &id);

    /* In order, with the values the captures carry. */
    {
        static const uint8_t en[1] = {1u}; /* PHY_ENABLED = 1 */
        TU_EQ(Props_Set(&s, PROPS_ID_PHY_ENABLED, en, 1u), SPINEL_STATUS_OK);
        TU_EQ(getValue(&s, PROPS_ID_PHY_ENABLED), 1u);
        TU_EQ(buf[0], 1u);
    }
    {
        static const uint8_t ch[1] = {20u}; /* PHY_CHAN = 20 */
        TU_EQ(Props_Set(&s, PROPS_ID_PHY_CHAN, ch, 1u), SPINEL_STATUS_OK);
        TU_EQ(getValue(&s, PROPS_ID_PHY_CHAN), 1u);
        TU_EQ(buf[0], 20u);
    }
    {
        static const uint8_t p[1] = {19u}; /* PHY_TX_POWER = 19 dBm (0x13) */
        TU_EQ(Props_Set(&s, PROPS_ID_PHY_TX_POWER, p, 1u), SPINEL_STATUS_OK);
        TU_EQ(getValue(&s, PROPS_ID_PHY_TX_POWER), 1u);
        TU_EQ((int8_t)buf[0], 19);
    }
    {
        static const uint8_t saddr[2] = {0x00u, 0x00u}; /* MAC_15_4_SADDR = 0 */
        TU_EQ(Props_Set(&s, PROPS_ID_MAC_SADDR, saddr, 2u), SPINEL_STATUS_OK);
        TU_EQ(s.shortAddr, 0u);
    }
    {
        static const uint8_t panid[2] = {0xD9u, 0x85u}; /* MAC_15_4_PANID = 0x85d9 */
        TU_EQ(Props_Set(&s, PROPS_ID_MAC_PANID, panid, 2u), SPINEL_STATUS_OK);
        TU_EQ(s.panId, 0x85D9u);
        TU_EQ(getValue(&s, PROPS_ID_MAC_PANID), 2u);
        TU_EQ(buf[0], 0xD9u); /* little-endian 'S' */
        TU_EQ(buf[1], 0x85u);
    }
    {
        /* MAC_15_4_LADDR: big-endian 'E' (docs/protocol.md §10). */
        static const uint8_t laddr[8] = {0x4Du, 0x32u, 0x5Au, 0x6Fu, 0x48u, 0x6Fu, 0x5Au, 0x8Fu};
        TU_EQ(Props_Set(&s, PROPS_ID_MAC_LADDR, laddr, 8u), SPINEL_STATUS_OK);
        TU_EQ(getValue(&s, PROPS_ID_MAC_LADDR), 8u);
        TU_CHECK(buf[0] == 0x4Du && buf[7] == 0x8Fu);
    }
    {
        static const uint8_t raw[1] = {1u}; /* MAC_RAW_STREAM_ENABLED = 1 */
        TU_EQ(Props_Set(&s, PROPS_ID_MAC_RAW_STREAM, raw, 1u), SPINEL_STATUS_OK);
        TU_EQ(s.rawStreamEnabled, 1u);
    }
    {
        static const uint8_t on[1] = {1u}; /* MAC_RX_ON_WHEN_IDLE_MODE = 1 */
        TU_EQ(Props_Set(&s, PROPS_ID_MAC_RX_ON_WHEN_IDLE, on, 1u), SPINEL_STATUS_OK);
        TU_EQ(s.rxOnWhenIdle, 1u);
    }
    {
        static const uint8_t prom[1] = {2u}; /* MAC_PROMISCUOUS_MODE = FULL */
        TU_EQ(Props_Set(&s, PROPS_ID_MAC_PROMISCUOUS, prom, 1u), SPINEL_STATUS_OK);
        TU_EQ(getValue(&s, PROPS_ID_MAC_PROMISCUOUS), 1u);
        TU_EQ(buf[0], 2u);
    }
}

TU_TEST(unknown_property_is_prop_not_found)
{
    Props_Identity id;
    Props_State s;

    initIdentity(&id);
    Props_Init(&s, &id);
    TU_EQ(Props_Find(0x1234u), (const Props_Desc *)0);
    TU_EQ(Props_Get(&s, 0x1234u, 0), SPINEL_STATUS_INVALID_ARG); /* no stream */
    {
        U_BStream bs;
        U_bstream_init(&bs, buf, sizeof(buf));
        TU_EQ(Props_Get(&s, 0x1234u, &bs), SPINEL_STATUS_PROP_NOT_FOUND);
    }
    TU_EQ(Props_Set(&s, 0x1234u, 0, 0u), SPINEL_STATUS_PROP_NOT_FOUND);
}

TU_TEST(read_only_properties_refuse_to_be_written)
{
    Props_Identity id;
    Props_State s;
    static const uint8_t v[8] = {0u};

    initIdentity(&id);
    Props_Init(&s, &id);
    TU_EQ(Props_Set(&s, PROPS_ID_PROTOCOL_VERSION, v, 2u), SPINEL_STATUS_UNIMPLEMENTED);
    TU_EQ(Props_Set(&s, PROPS_ID_NCP_VERSION, v, 1u), SPINEL_STATUS_UNIMPLEMENTED);
    TU_EQ(Props_Set(&s, PROPS_ID_INTERFACE_TYPE, v, 1u), SPINEL_STATUS_UNIMPLEMENTED);
    TU_EQ(Props_Set(&s, PROPS_ID_PHY_RSSI, v, 1u), SPINEL_STATUS_UNIMPLEMENTED);
    TU_EQ(Props_Set(&s, PROPS_ID_PHY_RX_SENSITIVITY, v, 1u), SPINEL_STATUS_UNIMPLEMENTED);
}

TU_TEST(wrong_length_or_out_of_range_is_invalid_arg)
{
    Props_Identity id;
    Props_State s;
    static const uint8_t one[1] = {1u};
    static const uint8_t two[2] = {1u, 2u};
    static const uint8_t chanLow[1] = {10u};
    static const uint8_t chanHigh[1] = {27u};
    static const uint8_t chanOk[1] = {11u};
    static const uint8_t promBad[1] = {3u};
    static const uint8_t eight[8] = {0u};

    initIdentity(&id);
    Props_Init(&s, &id);

    TU_EQ(Props_Set(&s, PROPS_ID_MAC_PANID, one, 1u), SPINEL_STATUS_INVALID_ARG);
    TU_EQ(Props_Set(&s, PROPS_ID_MAC_LADDR, two, 2u), SPINEL_STATUS_INVALID_ARG);
    TU_EQ(Props_Set(&s, PROPS_ID_PHY_ENABLED, two, 2u), SPINEL_STATUS_INVALID_ARG);
    TU_EQ(Props_Set(&s, PROPS_ID_PHY_ENABLED, 0, 1u), SPINEL_STATUS_INVALID_ARG);
    TU_EQ(Props_Set(&s, PROPS_ID_MAC_LADDR, eight, 8u), SPINEL_STATUS_OK);
    TU_EQ(Props_Set(&s, PROPS_ID_PHY_CHAN, chanLow, 1u), SPINEL_STATUS_INVALID_ARG);
    TU_EQ(Props_Set(&s, PROPS_ID_PHY_CHAN, chanHigh, 1u), SPINEL_STATUS_INVALID_ARG);
    TU_EQ(Props_Set(&s, PROPS_ID_MAC_PROMISCUOUS, promBad, 1u), SPINEL_STATUS_INVALID_ARG);
    /* a refused channel must not be stored */
    TU_EQ(s.channel, 0u);
    TU_EQ(Props_Set(&s, PROPS_ID_PHY_CHAN, chanOk, 1u), SPINEL_STATUS_OK);
    TU_EQ(s.channel, 11u);
}

TU_TEST(random_sets_never_corrupt_the_state)
{
    Props_Identity id;
    Props_State s;
    static const uint8_t val[16] = {0u};
    uint32_t seed = 0x5EEDu;
    unsigned i;

    initIdentity(&id);
    Props_Init(&s, &id);

    /* Random id + length: every call returns a status and the state stays in range. */
    for (i = 0; i < 5000u; ++i)
    {
        uint32_t pid;
        uint16_t len;

        seed = (seed * 1103515245u) + 12345u;
        pid = (seed >> 16) & 0xFFu;
        len = (uint16_t)((seed >> 8) & 0x0Fu);
        (void)Props_Set(&s, pid, val, len);
    }

    TU_CHECK(s.channel == 0u || (s.channel >= 11u && s.channel <= 26u));
    TU_CHECK(s.promiscuousMode <= 2u);
    TU_CHECK(s.phyEnabled <= 1u);
    {
        static const uint8_t ch[1] = {15u};
        TU_EQ(Props_Set(&s, PROPS_ID_PHY_CHAN, ch, 1u), SPINEL_STATUS_OK);
        TU_EQ(getValue(&s, PROPS_ID_PHY_CHAN), 1u);
        TU_EQ(buf[0], 15u);
    }
}

TU_TEST(hwaddr_is_the_factory_address_in_display_order)
{
    Props_Identity id;
    Props_State s;

    initIdentity(&id);
    Props_Init(&s, &id);

    TU_EQ(getValue(&s, PROPS_ID_HWADDR), 8u);
    /* Byte 0 is the OUI's first byte: display order, the reverse of the NVM user row's
   * storage order (Nvram_FactoryEui64). The ConBee III reports 00 21 2e ff ff 0e 21 2e. */
    TU_EQ(buf[0], 0x00u);
    TU_EQ(buf[1], 0x21u);
    TU_EQ(buf[2], 0x2eu);
    TU_EQ(buf[7], 0x2eu);
}

TU_TEST(the_tx_power_is_capped_per_board_and_lower_on_channel_26)
{
    Props_Identity id;
    Props_State s;
    static const uint8_t ch25[1] = {25u};
    static const uint8_t ch26[1] = {26u};
    static const uint8_t p0[1] = {0u};
    static const uint8_t pMinus20[1] = {(uint8_t)-20};

    /* The ConBee II's stock levels: -3 dBm, -12 dBm on channel 26. */
    initIdentity(&id);
    id.txPowerMaxDbm = -3;
    id.txPowerMaxCh26Dbm = -12;
    Props_Init(&s, &id);

    /* The host's default 0 dBm is above the cap: the radio gets the cap, and so does a GET. */
    TU_EQ(Props_TxPowerDbm(&s, 11u), -3);
    TU_EQ(Props_TxPowerDbm(&s, 25u), -3);
    TU_EQ(Props_TxPowerDbm(&s, 26u), -12);
    TU_EQ(Props_Set(&s, PROPS_ID_PHY_CHAN, ch25, 1u), SPINEL_STATUS_OK);
    TU_EQ(getValue(&s, PROPS_ID_PHY_TX_POWER), 1u);
    TU_EQ((int8_t)buf[0], -3);
    TU_EQ(Props_Set(&s, PROPS_ID_PHY_CHAN, ch26, 1u), SPINEL_STATUS_OK);
    TU_EQ(getValue(&s, PROPS_ID_PHY_TX_POWER), 1u);
    TU_EQ((int8_t)buf[0], -12);

    /* The host's value is kept, not overwritten: off channel 26 it applies capped again. */
    TU_EQ(Props_Set(&s, PROPS_ID_PHY_TX_POWER, p0, 1u), SPINEL_STATUS_OK);
    TU_EQ(s.txPowerDbm, 0);
    TU_EQ(Props_TxPowerDbm(&s, 20u), -3);

    /* Below every cap it passes through on every channel. */
    TU_EQ(Props_Set(&s, PROPS_ID_PHY_TX_POWER, pMinus20, 1u), SPINEL_STATUS_OK);
    TU_EQ(Props_TxPowerDbm(&s, 20u), -20);
    TU_EQ(Props_TxPowerDbm(&s, 26u), -20);
    TU_EQ(getValue(&s, PROPS_ID_PHY_TX_POWER), 1u);
    TU_EQ((int8_t)buf[0], -20);

    /* No identity, no cap. */
    Props_Init(&s, 0);
    TU_EQ(Props_Set(&s, PROPS_ID_PHY_TX_POWER, p0, 1u), SPINEL_STATUS_OK);
    TU_EQ(Props_TxPowerDbm(&s, 26u), 0);
}

TU_TEST(hwaddr_without_a_factory_address_is_invalid_state)
{
    Props_Identity id;
    Props_State s;
    U_BStream bs;

    initIdentity(&id);
    id.hwAddr = 0; /* a part whose user row was never programmed */
    Props_Init(&s, &id);

    U_bstream_init(&bs, buf, sizeof(buf));
    TU_EQ(Props_Get(&s, PROPS_ID_HWADDR, &bs), SPINEL_STATUS_INVALID_STATE);
    /* Nothing partial: eight zero bytes must not be mistaken for an address. */
    TU_EQ(U_bstream_remaining(&bs), sizeof(buf));
}

TU_TEST(the_scan_family_defaults_to_what_an_rcp_reports)
{
    Props_Identity id;
    Props_State s;
    uint16_t len;

    initIdentity(&id);
    Props_Init(&s, &id);

    /* Fresh state, before any host touched it: idle, the whole Zigbee band, 200 ms - upstream's own
     * defaults, so a host that writes only the state meets the same numbers it would on a stock RCP
     * (docs/protocol.md §10.1). */
    len = getValue(&s, PROPS_ID_MAC_SCAN_STATE);
    TU_EQ(len, 1u);
    TU_EQ(buf[0], PROPS_SCAN_STATE_IDLE);
    len = getValue(&s, PROPS_ID_MAC_SCAN_MASK);
    TU_EQ(len, 17u); /* one count byte and the 16 channels 11..26 */
    TU_EQ(buf[0], 16u);
    TU_EQ(buf[1], 11u);
    TU_EQ(buf[16], 26u);
    len = getValue(&s, PROPS_ID_MAC_SCAN_PERIOD);
    TU_EQ(len, 2u);
    TU_EQ((uint16_t)(buf[0] | (buf[1] << 8)), PROPS_SCAN_PERIOD_DEFAULT);
    TU_CHECK(s.scanStart == 0u);
}

/* RadioSpinel dies without these two, and reads the value to its end: no count byte. */
TU_TEST(the_channel_masks_list_the_band_in_upstreams_shape)
{
    Props_Identity id;
    Props_State s;
    uint16_t len;
    uint8_t k;

    initIdentity(&id);
    Props_Init(&s, &id);

    len = getValue(&s, PROPS_ID_PHY_CHAN_SUPPORTED);
    TU_EQ(len, 16u);

    for (k = 0u; k < 16u; ++k)
        TU_EQ(buf[k], 11u + k);

    len = getValue(&s, PROPS_ID_PHY_CHAN_PREFERRED);
    TU_EQ(len, 16u);
    TU_EQ(buf[0], 11u);
    TU_EQ(buf[15], 26u);

    TU_EQ(Props_Set(&s, PROPS_ID_PHY_CHAN_SUPPORTED, buf, 1u), SPINEL_STATUS_UNIMPLEMENTED);
}

/* The bytes RadioSpinel::EnergyScan writes: one bare channel number. */
TU_TEST(scan_mask_takes_upstreams_bare_channel_numbers)
{
    Props_Identity id;
    Props_State s;

    initIdentity(&id);
    Props_Init(&s, &id);

    {
        static const uint8_t one[1] = {15u};
        static const uint8_t two[2] = {11u, 26u};
        static const uint8_t wide[2] = {11u, 32u};
        static const uint8_t outOfBand[1] = {5u};

        TU_EQ(Props_Set(&s, PROPS_ID_MAC_SCAN_MASK, one, 1u), SPINEL_STATUS_OK);
        TU_EQ(s.scanMask, 1uL << 15);
        TU_EQ(Props_Set(&s, PROPS_ID_MAC_SCAN_MASK, two, 2u), SPINEL_STATUS_OK);
        TU_EQ(s.scanMask, (1uL << 11) | (1uL << 26));
        TU_EQ(Props_Set(&s, PROPS_ID_MAC_SCAN_MASK, wide, 2u), SPINEL_STATUS_INVALID_ARG);
        TU_EQ(Props_Set(&s, PROPS_ID_MAC_SCAN_MASK, outOfBand, 1u), SPINEL_STATUS_INVALID_ARG);
        TU_EQ(s.scanMask, (1uL << 11) | (1uL << 26));
    }

    /* An empty value is upstream's empty list. */
    TU_EQ(Props_Set(&s, PROPS_ID_MAC_SCAN_MASK, 0, 0u), SPINEL_STATUS_OK);
    TU_EQ(s.scanMask, 0u);
}

TU_TEST(scan_mask_is_a_channel_list_not_a_count_of_channels)
{
    Props_Identity id;
    Props_State s;
    uint16_t len;

    initIdentity(&id);
    Props_Init(&s, &id);

    {
        static const uint8_t two[3] = {2u, 11u, 20u};

        TU_EQ(Props_Set(&s, PROPS_ID_MAC_SCAN_MASK, two, 3u), SPINEL_STATUS_OK);
        TU_EQ(s.scanMask, (1uL << 11) | (1uL << 20));
    }

    len = getValue(&s, PROPS_ID_MAC_SCAN_MASK);
    TU_EQ(len, 3u);
    TU_EQ(buf[0], 2u);
    TU_EQ(buf[1], 11u); /* ascending, whatever order the host wrote them in */
    TU_EQ(buf[2], 20u);

    /* An empty list is a legal value: it stores, and only a start refuses it. A host may state that
     * it cares about nothing without being told its write was malformed. */
    {
        static const uint8_t none[1] = {0u};

        TU_EQ(Props_Set(&s, PROPS_ID_MAC_SCAN_MASK, none, 1u), SPINEL_STATUS_OK);
        TU_EQ(s.scanMask, 0u);
        len = getValue(&s, PROPS_ID_MAC_SCAN_MASK);
        TU_EQ(len, 1u);
        TU_EQ(buf[0], 0u);
    }

    /* A list that does not match its own count, a channel number the wire format cannot carry, and a
     * list wider than this band are all refused, and a refused write leaves the stored mask alone -
     * upstream's bound on a channel number is 31 (ncp_base.cpp:1755). */
    {
        static const uint8_t shortList[2] = {2u, 11u};
        static const uint8_t wide[2] = {1u, 32u};
        static const uint8_t tooMany[34] = {33u, 0u,  1u,  2u,  3u,  4u,  5u,  6u,  7u,  8u,
                                            9u,  10u, 11u, 12u, 13u, 14u, 15u, 16u, 17u, 18u,
                                            19u, 20u, 21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u,
                                            29u, 30u, 31u, 0u};
        uint32_t before = s.scanMask;

        TU_EQ(Props_Set(&s, PROPS_ID_MAC_SCAN_MASK, shortList, 2u), SPINEL_STATUS_INVALID_ARG);
        TU_EQ(Props_Set(&s, PROPS_ID_MAC_SCAN_MASK, wide, 2u), SPINEL_STATUS_INVALID_ARG);
        TU_EQ(Props_Set(&s, PROPS_ID_MAC_SCAN_MASK, tooMany, 34u), SPINEL_STATUS_INVALID_ARG);
        TU_EQ(s.scanMask, before);
    }

    /* A channel this radio does not have round-trips anyway, because the echo of a SET is the same
     * question the host just asked, not a filtered answer: it is the dwell that refuses it, and the
     * cell below proves that. */
    {
        static const uint8_t low[3] = {2u, 1u, 5u};

        TU_EQ(Props_Set(&s, PROPS_ID_MAC_SCAN_MASK, low, 3u), SPINEL_STATUS_OK);
        len = getValue(&s, PROPS_ID_MAC_SCAN_MASK);
        TU_EQ(len, 3u);
        TU_EQ(buf[0], 2u);
        TU_EQ(buf[1], 1u);
        TU_EQ(buf[2], 5u);
    }
}

TU_TEST(scan_period_is_stored_as_written)
{
    Props_Identity id;
    Props_State s;
    uint16_t len;

    initIdentity(&id);
    Props_Init(&s, &id);

    /* Little-endian 'S', stored verbatim: the bound on a dwell belongs to the dwell, not to the
     * property, so a host reads back what it wrote (rcp/scan.h clamps what one channel actually
     * measures). */
    {
        static const uint8_t p[2] = {0x88u, 0x13u}; /* 5000 */

        TU_EQ(Props_Set(&s, PROPS_ID_MAC_SCAN_PERIOD, p, 2u), SPINEL_STATUS_OK);
    }

    len = getValue(&s, PROPS_ID_MAC_SCAN_PERIOD);
    TU_EQ(len, 2u);
    TU_EQ((uint16_t)(buf[0] | (buf[1] << 8)), 5000u);

    {
        static const uint8_t z[2] = {0u, 0u};

        TU_EQ(Props_Set(&s, PROPS_ID_MAC_SCAN_PERIOD, z, 2u), SPINEL_STATUS_OK);
        len = getValue(&s, PROPS_ID_MAC_SCAN_PERIOD);
        TU_EQ((uint16_t)(buf[0] | (buf[1] << 8)), 0u);
    }
}

TU_TEST(scan_state_answers_the_ways_an_rcp_does)
{
    Props_Identity id;
    Props_State s;
    uint32_t mask;
    uint16_t period;

    initIdentity(&id);
    Props_Init(&s, &id);

    /* A fresh session has the PHY off, which is the state a scan cannot begin in, and a whole-band
     * mask, which is not a channel a dwell can sit on. */
    TU_EQ(Props_Set(&s, PROPS_ID_MAC_SCAN_STATE, (const uint8_t[]){PROPS_SCAN_STATE_ENERGY}, 1u),
          SPINEL_STATUS_INVALID_STATE);

    s.phyEnabled = 1u;

    /* The kinds an RCP build does not implement answer UNIMPLEMENTED - upstream's own arm for the
     * default case in link_raw.cpp:217. */
    TU_EQ(Props_Set(&s, PROPS_ID_MAC_SCAN_STATE, (const uint8_t[]){PROPS_SCAN_STATE_BEACON}, 1u),
          SPINEL_STATUS_UNIMPLEMENTED);
    TU_EQ(Props_Set(&s, PROPS_ID_MAC_SCAN_STATE, (const uint8_t[]){PROPS_SCAN_STATE_DISCOVER}, 1u),
          SPINEL_STATUS_UNIMPLEMENTED);
    TU_EQ(Props_Set(&s, PROPS_ID_MAC_SCAN_STATE, (const uint8_t[]){4u}, 1u),
          SPINEL_STATUS_UNIMPLEMENTED);

    /* A whole-band mask stores happily and is refused only when a dwell is asked to begin on it. This
     * is zoh's exact call shape - mask first, whole band, then start - and an RCP answers it as a bad
     * argument rather than measuring one channel of a sixteen-channel request (docs/protocol.md §10.1,
     * and zigbee-on-host's src/spinel/commands.ts for the call). */
    TU_EQ(Props_Set(&s, PROPS_ID_MAC_SCAN_STATE, (const uint8_t[]){PROPS_SCAN_STATE_ENERGY}, 1u),
          SPINEL_STATUS_INVALID_ARG);
    {
        static const uint8_t band[17] = {16u, 11u, 12u, 13u, 14u, 15u, 16u, 17u, 18u,
                                         19u, 20u, 21u, 22u, 23u, 24u, 25u, 26u};
        TU_EQ(Props_Set(&s, PROPS_ID_MAC_SCAN_MASK, band, 17u), SPINEL_STATUS_OK);
        TU_EQ(s.scanMask, 0x007FFF800u);
        TU_EQ(Props_Set(&s, PROPS_ID_MAC_SCAN_STATE, (const uint8_t[]){PROPS_SCAN_STATE_ENERGY}, 1u),
              SPINEL_STATUS_INVALID_ARG);
        TU_CHECK(s.scanStart == 0u); /* a refused start asks the app for nothing */
        TU_EQ(s.scanState, PROPS_SCAN_STATE_IDLE);
    }

    /* One channel: accepted, reported as running, and the request is handed to the app once. */
    {
        static const uint8_t ch15[2] = {1u, 15u};

        TU_EQ(Props_Set(&s, PROPS_ID_MAC_SCAN_MASK, ch15, 2u), SPINEL_STATUS_OK);
        TU_EQ(Props_Set(&s, PROPS_ID_MAC_SCAN_STATE, (const uint8_t[]){PROPS_SCAN_STATE_ENERGY}, 1u),
              SPINEL_STATUS_OK);
        TU_EQ(s.scanState, PROPS_SCAN_STATE_ENERGY);
        TU_EQ(Props_TakeScanStart(&s, &mask, &period), 1);
        TU_EQ(mask, 1uL << 15);
        TU_EQ(period, PROPS_SCAN_PERIOD_DEFAULT);
        TU_EQ(Props_TakeScanStart(&s, &mask, &period), 0); /* taken once, not twice */

        /* A second dwell while the first owns the machine is refused, and a channel outside this
         * silicon's band never starts. */
        TU_EQ(Props_Set(&s, PROPS_ID_MAC_SCAN_STATE, (const uint8_t[]){PROPS_SCAN_STATE_ENERGY}, 1u),
              SPINEL_STATUS_INVALID_STATE);
        Props_SetScanState(&s, PROPS_SCAN_STATE_IDLE);

        {
            static const uint8_t ch5[2] = {1u, 5u};
            TU_EQ(Props_Set(&s, PROPS_ID_MAC_SCAN_MASK, ch5, 2u), SPINEL_STATUS_OK);
            TU_EQ(Props_Set(&s, PROPS_ID_MAC_SCAN_STATE, (const uint8_t[]){PROPS_SCAN_STATE_ENERGY}, 1u),
                  SPINEL_STATUS_INVALID_ARG);
        }
    }

    /* A cancel is accepted and changes nothing - upstream's cancel is a no-op too, and a dwell here
     * ends by its own dwell (docs/protocol.md §10.1). */
    {
        static const uint8_t ch11[2] = {1u, 11u};
        Props_Set(&s, PROPS_ID_MAC_SCAN_MASK, ch11, 2u);
        TU_EQ(Props_Set(&s, PROPS_ID_MAC_SCAN_STATE, (const uint8_t[]){PROPS_SCAN_STATE_ENERGY}, 1u),
              SPINEL_STATUS_OK);
        TU_EQ(Props_Set(&s, PROPS_ID_MAC_SCAN_STATE, (const uint8_t[]){PROPS_SCAN_STATE_IDLE}, 1u),
              SPINEL_STATUS_OK);
        TU_EQ(s.scanState, PROPS_SCAN_STATE_ENERGY); /* still running, and still asked for once */
        TU_EQ(Props_TakeScanStart(&s, &mask, &period), 1);
        TU_EQ(mask, 1uL << 11);
    }
}

TU_TEST(the_scan_result_id_is_not_a_property)
{
    Props_Identity id;
    Props_State s;

    initIdentity(&id);
    Props_Init(&s, &id);

    /* MAC_SCAN_RESULT and MAC_SCAN_BEACON are events, not properties: they stay out of the table, so
     * a GET answers PROP_NOT_FOUND exactly as a conformant RCP does (docs/protocol.md §10.1). */
    TU_CHECK(Props_Find(PROPS_ID_MAC_SCAN_RESULT) == 0);
    {
        U_BStream bs;

        U_bstream_init(&bs, buf, sizeof(buf));
        TU_EQ(Props_Get(&s, PROPS_ID_MAC_SCAN_RESULT, &bs), SPINEL_STATUS_PROP_NOT_FOUND);
    }
}

TU_TEST(hwaddr_is_read_only)
{
    Props_Identity id;
    Props_State s;
    static const uint8_t write[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};

    initIdentity(&id);
    Props_Init(&s, &id);

    /* Factory address is not host-settable; MAC_15_4_LADDR (0x34) is the operational one. */
    TU_EQ(Props_Set(&s, PROPS_ID_HWADDR, write, 8u), SPINEL_STATUS_UNIMPLEMENTED);
    TU_EQ(getValue(&s, PROPS_ID_HWADDR), 8u);
    TU_EQ(buf[0], 0x00u);
    TU_EQ(buf[1], 0x21u);
}

TU_TEST(the_source_match_family_has_upstream_ids_types_and_defaults)
{
    Props_Identity id;
    Props_State s;
    const Props_Desc *d;

    /* SPINEL_PROP_MAC_EXT__BEGIN (0x1300) + 3, + 4, + 5. Pinned as literals: a host writes
     * these numbers, so a typo here is a silent property-not-found on the wire. */
    TU_EQ(PROPS_ID_MAC_SRC_MATCH_ENABLED, 0x1303u);
    TU_EQ(PROPS_ID_MAC_SRC_MATCH_SHORT, 0x1304u);
    TU_EQ(PROPS_ID_MAC_SRC_MATCH_EXT, 0x1305u);

    initIdentity(&id);
    Props_Init(&s, &id);

    d = Props_Find(PROPS_ID_MAC_SRC_MATCH_ENABLED);
    TU_CHECK(d != 0);
    TU_EQ(d->type, PROPS_T_BOOL);
    TU_EQ(d->access, (PROPS_ACC_READ | PROPS_ACC_WRITE));

    d = Props_Find(PROPS_ID_MAC_SRC_MATCH_SHORT);
    TU_EQ(d->type, PROPS_T_ADDR_LIST_SHORT);
    TU_EQ(d->access, (PROPS_ACC_READ | PROPS_ACC_WRITE));

    d = Props_Find(PROPS_ID_MAC_SRC_MATCH_EXT);
    TU_EQ(d->type, PROPS_T_ADDR_LIST_EXT);

    /* Disabled and empty after a reset: the standard's fallback, which claims pending. */
    TU_EQ(getValue(&s, PROPS_ID_MAC_SRC_MATCH_ENABLED), 1u);
    TU_EQ(buf[0], 0u);
    TU_EQ(getValue(&s, PROPS_ID_MAC_SRC_MATCH_SHORT), 0u);
    TU_EQ(getValue(&s, PROPS_ID_MAC_SRC_MATCH_EXT), 0u);
}

TU_TEST(a_short_list_is_stored_and_read_back_byte_for_byte)
{
    Props_Identity id;
    Props_State s;
    static const uint8_t wire[6] = {0x02u, 0x00u, 0x34u, 0x12u, 0xFFu, 0xFFu};

    initIdentity(&id);
    Props_Init(&s, &id);

    TU_EQ(Props_Set(&s, PROPS_ID_MAC_SRC_MATCH_SHORT, wire, sizeof(wire)), SPINEL_STATUS_OK);
    TU_EQ(getValue(&s, PROPS_ID_MAC_SRC_MATCH_SHORT), 6u);

    for (uint8_t k = 0; k < sizeof(wire); ++k)
        TU_EQ(buf[k], wire[k]);

    /* An empty list is a legitimate value, and it clears the table. */
    TU_EQ(Props_Set(&s, PROPS_ID_MAC_SRC_MATCH_SHORT, 0, 0u), SPINEL_STATUS_OK);
    TU_EQ(getValue(&s, PROPS_ID_MAC_SRC_MATCH_SHORT), 0u);
}

TU_TEST(a_list_that_does_not_fit_is_refused_and_stores_nothing)
{
    Props_Identity id;
    Props_State s;
    uint8_t wire[(SRCMATCH_SHORT_MAX + 1u) * 2u];

    initIdentity(&id);
    Props_Init(&s, &id);

    for (uint16_t k = 0; k < (SRCMATCH_SHORT_MAX + 1u) * 2u; ++k)
        wire[k] = (uint8_t)k;

    TU_EQ(Props_Set(&s, PROPS_ID_MAC_SRC_MATCH_SHORT, wire, sizeof(wire)), SPINEL_STATUS_NOMEM);
    TU_EQ(getValue(&s, PROPS_ID_MAC_SRC_MATCH_SHORT), 0u);

    /* An odd length is not a list of 16-bit addresses. */
    TU_EQ(Props_Set(&s, PROPS_ID_MAC_SRC_MATCH_SHORT, wire, 3u), SPINEL_STATUS_INVALID_ARG);
    TU_EQ(getValue(&s, PROPS_ID_MAC_SRC_MATCH_SHORT), 0u);

    /* An extended list is a multiple of 8. */
    TU_EQ(Props_Set(&s, PROPS_ID_MAC_SRC_MATCH_EXT, wire, 9u), SPINEL_STATUS_INVALID_ARG);
}

TU_TEST(insert_and_remove_answer_the_ways_upstream_does)
{
    Props_Identity id;
    Props_State s;
    static const uint8_t one[2] = {0x02u, 0x00u};
    static const uint8_t other[2] = {0x03u, 0x00u};
    static const uint8_t ext[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    initIdentity(&id);
    Props_Init(&s, &id);

    TU_EQ(Props_Insert(&s, PROPS_ID_MAC_SRC_MATCH_SHORT, one, 2u), SPINEL_STATUS_OK);
    TU_EQ(Props_Insert(&s, PROPS_ID_MAC_SRC_MATCH_SHORT, one, 2u), SPINEL_STATUS_OK);
    TU_EQ(getValue(&s, PROPS_ID_MAC_SRC_MATCH_SHORT), 2u); /* stored once */

    TU_EQ(Props_Insert(&s, PROPS_ID_MAC_SRC_MATCH_EXT, ext, 8u), SPINEL_STATUS_OK);
    TU_EQ(getValue(&s, PROPS_ID_MAC_SRC_MATCH_EXT), 8u);

    /* A REMOVE of something that is not there is ITEM_NOT_FOUND, not a silent success. */
    TU_EQ(Props_Remove(&s, PROPS_ID_MAC_SRC_MATCH_SHORT, other, 2u),
          SPINEL_STATUS_ITEM_NOT_FOUND);
    TU_EQ(Props_Remove(&s, PROPS_ID_MAC_SRC_MATCH_SHORT, one, 2u), SPINEL_STATUS_OK);
    TU_EQ(getValue(&s, PROPS_ID_MAC_SRC_MATCH_SHORT), 0u);

    /* Wrong lengths are refused. */
    TU_EQ(Props_Insert(&s, PROPS_ID_MAC_SRC_MATCH_SHORT, one, 1u), SPINEL_STATUS_INVALID_ARG);
    TU_EQ(Props_Insert(&s, PROPS_ID_MAC_SRC_MATCH_EXT, ext, 7u), SPINEL_STATUS_INVALID_ARG);
    TU_EQ(Props_Remove(&s, PROPS_ID_MAC_SRC_MATCH_EXT, 0, 8u), SPINEL_STATUS_INVALID_ARG);

    /* A property with no insert or remove handler answers PROP_NOT_FOUND, which is what a
     * host expects for one that is not a table (ncp_base.cpp HandleCommandPropertyInsertRemove). */
    TU_EQ(Props_Insert(&s, PROPS_ID_MAC_PANID, one, 2u), SPINEL_STATUS_PROP_NOT_FOUND);
    TU_EQ(Props_Remove(&s, PROPS_ID_PHY_CHAN, one, 1u), SPINEL_STATUS_PROP_NOT_FOUND);
    TU_EQ(Props_Insert(&s, 0x7Fu, one, 2u), SPINEL_STATUS_PROP_NOT_FOUND);
}

TU_TEST(the_enabled_flag_is_stored_and_survives_a_table_write)
{
    Props_Identity id;
    Props_State s;
    static const uint8_t on[1] = {1u};
    static const uint8_t off[1] = {0u};
    static const uint8_t two[2] = {1u, 0u};
    static const uint8_t one[2] = {0x02u, 0x00u};

    initIdentity(&id);
    Props_Init(&s, &id);

    TU_EQ(Props_Set(&s, PROPS_ID_MAC_SRC_MATCH_ENABLED, on, 1u), SPINEL_STATUS_OK);
    TU_EQ(getValue(&s, PROPS_ID_MAC_SRC_MATCH_ENABLED), 1u);
    TU_EQ(buf[0], 1u);

    /* Writing the list must not clear the flag: a host sets the flag once and rewrites the
     * list as its queue changes (ziggurat's src_match_sync_task). */
    TU_EQ(Props_Set(&s, PROPS_ID_MAC_SRC_MATCH_SHORT, one, 2u), SPINEL_STATUS_OK);
    TU_EQ(getValue(&s, PROPS_ID_MAC_SRC_MATCH_ENABLED), 1u);
    TU_EQ(buf[0], 1u);

    TU_EQ(Props_Set(&s, PROPS_ID_MAC_SRC_MATCH_ENABLED, off, 1u), SPINEL_STATUS_OK);
    TU_EQ(getValue(&s, PROPS_ID_MAC_SRC_MATCH_ENABLED), 1u);
    TU_EQ(buf[0], 0u);

    /* Any non-zero write enables it, like the other u8 gates; a bad length is refused. */
    TU_EQ(Props_Set(&s, PROPS_ID_MAC_SRC_MATCH_ENABLED, two, 2u), SPINEL_STATUS_INVALID_ARG);
    TU_EQ(Props_Set(&s, PROPS_ID_MAC_SRC_MATCH_ENABLED, 0, 1u), SPINEL_STATUS_INVALID_ARG);
}

TU_TEST(a_reset_clears_the_source_match_table)
{
    Props_Identity id;
    Props_State s;
    static const uint8_t on[1] = {1u};
    static const uint8_t one[2] = {0x02u, 0x00u};

    initIdentity(&id);
    Props_Init(&s, &id);
    (void)Props_Set(&s, PROPS_ID_MAC_SRC_MATCH_ENABLED, on, 1u);
    (void)Props_Set(&s, PROPS_ID_MAC_SRC_MATCH_SHORT, one, 2u);

    /* Props_Init is what Rcp_Reset calls (session.c). */
    Props_Init(&s, &id);
    TU_EQ(getValue(&s, PROPS_ID_MAC_SRC_MATCH_ENABLED), 1u);
    TU_EQ(buf[0], 0u);
    TU_EQ(getValue(&s, PROPS_ID_MAC_SRC_MATCH_SHORT), 0u);
}

int main(void)
{
    static const Tu_Test tests[] = {
        {"mac_property_ids_match_upstream_spinel", mac_property_ids_match_upstream_spinel},
        {"init_leaves_nothing_of_what_was_there_before", init_leaves_nothing_of_what_was_there_before},
        {"the_source_match_family_has_upstream_ids_types_and_defaults",
         the_source_match_family_has_upstream_ids_types_and_defaults},
        {"a_short_list_is_stored_and_read_back_byte_for_byte",
         a_short_list_is_stored_and_read_back_byte_for_byte},
        {"a_list_that_does_not_fit_is_refused_and_stores_nothing",
         a_list_that_does_not_fit_is_refused_and_stores_nothing},
        {"insert_and_remove_answer_the_ways_upstream_does",
         insert_and_remove_answer_the_ways_upstream_does},
        {"the_enabled_flag_is_stored_and_survives_a_table_write",
         the_enabled_flag_is_stored_and_survives_a_table_write},
        {"a_reset_clears_the_source_match_table", a_reset_clears_the_source_match_table},
        {"table_ids_are_unique_and_bounded", table_ids_are_unique_and_bounded},
        {"every_writable_property_is_readable", every_writable_property_is_readable},
        {"every_table_entry_answers_a_get", every_table_entry_answers_a_get},
        {"protocol_version_is_the_captured_04_03", protocol_version_is_the_captured_04_03},
        {"ncp_version_is_zero_terminated_and_self_identifying",
         ncp_version_is_zero_terminated_and_self_identifying},
        {"interface_type_is_spinel", interface_type_is_spinel},
        {"caps_property_ids_match_upstream_spinel", caps_property_ids_match_upstream_spinel},
        {"caps_lists_exactly_the_true_capabilities", caps_lists_exactly_the_true_capabilities},
        {"radio_caps_advertises_the_supported_radio_features",
         radio_caps_advertises_the_supported_radio_features},
        {"caps_and_radio_caps_are_read_only", caps_and_radio_caps_are_read_only},
        {"sensitivity_comes_from_the_board_identity", sensitivity_comes_from_the_board_identity},
        {"rssi_is_unknown_until_the_session_reports_one",
         rssi_is_unknown_until_the_session_reports_one},
        {"replays_the_captured_set_sequence", replays_the_captured_set_sequence},
        {"unknown_property_is_prop_not_found", unknown_property_is_prop_not_found},
        {"read_only_properties_refuse_to_be_written", read_only_properties_refuse_to_be_written},
        {"wrong_length_or_out_of_range_is_invalid_arg",
         wrong_length_or_out_of_range_is_invalid_arg},
        {"random_sets_never_corrupt_the_state", random_sets_never_corrupt_the_state},
        {"hwaddr_is_the_factory_address_in_display_order",
         hwaddr_is_the_factory_address_in_display_order},
        {"the_tx_power_is_capped_per_board_and_lower_on_channel_26",
         the_tx_power_is_capped_per_board_and_lower_on_channel_26},
        {"hwaddr_without_a_factory_address_is_invalid_state",
         hwaddr_without_a_factory_address_is_invalid_state},
        {"hwaddr_is_read_only", hwaddr_is_read_only},
        {"the_scan_family_defaults_to_what_an_rcp_reports",
         the_scan_family_defaults_to_what_an_rcp_reports},
        {"the_channel_masks_list_the_band_in_upstreams_shape", the_channel_masks_list_the_band_in_upstreams_shape},
        {"scan_mask_takes_upstreams_bare_channel_numbers", scan_mask_takes_upstreams_bare_channel_numbers},
        {"scan_mask_is_a_channel_list_not_a_count_of_channels",
         scan_mask_is_a_channel_list_not_a_count_of_channels},
        {"scan_period_is_stored_as_written", scan_period_is_stored_as_written},
        {"scan_state_answers_the_ways_an_rcp_does", scan_state_answers_the_ways_an_rcp_does},
        {"the_scan_result_id_is_not_a_property", the_scan_result_id_is_not_a_property}};

    Tu_Run("props", tests, (int)(sizeof(tests) / sizeof(tests[0])));
    return 0; /* Tu_Run never returns */
}
