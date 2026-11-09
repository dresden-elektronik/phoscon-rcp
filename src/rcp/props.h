/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* props.h — the Spinel property table and its handlers (docs/protocol.md §10). */
#ifndef RCP_PROPS_H
#define RCP_PROPS_H

#include <stdint.h>

#ifdef RCP_THREAD
#include "mac_sec.h"
#endif
#include "srcmatch.h"
#include "u_bstream.h"

/* Property ids of docs/protocol.md §10 (values per spinel.h). */
#define PROPS_ID_LAST_STATUS 0x0000u
#define PROPS_ID_PROTOCOL_VERSION 0x0001u
#define PROPS_ID_NCP_VERSION 0x0002u
#define PROPS_ID_INTERFACE_TYPE 0x0003u
/* Upstream's read-only capability list (openthread/src/lib/spinel/spinel.h:1428
 * SPINEL_PROP_CAPS). The value is a list of packed uints, one capability number per element. */
#define PROPS_ID_CAPS 0x0005u
#define PROPS_ID_RCP_API_VERSION 0x00B0u
#define PROPS_ID_RCP_MIN_HOST_API 0x00B1u
/* Upstream's crash-dump trigger, which is the RCP log channel: an empty write starts the
 * record stream (openthread/src/lib/spinel/spinel.h:4296, SPINEL_PROP_RCP__BEGIN
 * is 0xB0). We also accept a u8 payload so the host can stop it (docs/protocol.md §10). */
#define PROPS_ID_RCP_LOG_CRASH_DUMP 0x00B2u
/* Transmit security: SPINEL_PROP_RCP_EXT__BEGIN (0x800) + 0 and + 1 (spinel.h:4700, :4712). Both
 * are write-only, so a key is never answered back. */
#define PROPS_ID_RCP_MAC_KEY 0x0800u
#define PROPS_ID_RCP_MAC_FRAME_COUNTER 0x0801u
/* Enhanced-Ack based Link Metrics probing: SPINEL_PROP_RCP_EXT__BEGIN + 3 (spinel.h:4741). */
#define PROPS_ID_RCP_ENH_ACK_PROBING 0x0803u
/* The radio's clock, 64-bit microseconds (spinel.h:4721). The host reads it on every pass of its
 * loop until it gets an answer, so without one it floods the link. */
#define PROPS_ID_RCP_TIMESTAMP 0x0802u
/* What a CSL child is told to allow for: the clock's accuracy in ppm and a timed transmission's
 * uncertainty in units of 10 us (spinel.h:4749, :4757). */
#define PROPS_ID_RCP_CSL_ACCURACY 0x0804u
#define PROPS_ID_RCP_CSL_UNCERTAINTY 0x0805u
#define PROPS_ID_PHY_ENABLED 0x0020u
/* The factory-assigned IEEE EUI-64, upstream's SPINEL_PROP_HWADDR, distinct from
 * MAC_15_4_LADDR: this is what the module was born with, MSB first
 * (openthread/src/lib/spinel/spinel.h:1258). */
#define PROPS_ID_HWADDR 0x0008u
#define PROPS_ID_PHY_CHAN 0x0021u
/* The channels this radio has and the ones a host should prefer: SPINEL_PROP_PHY__BEGIN + 2 and
 * + 9 (spinel.h:1706, :1713). A host that forms a network cannot start without them
 * (radio_spinel.cpp GetRadioChannelMask is SuccessOrDie). */
#define PROPS_ID_PHY_CHAN_SUPPORTED 0x0022u
#define PROPS_ID_PHY_CCA_THRESHOLD 0x0024u
#define PROPS_ID_PHY_TX_POWER 0x0025u
#define PROPS_ID_PHY_RSSI 0x0026u
#define PROPS_ID_PHY_RX_SENSITIVITY 0x0027u
#define PROPS_ID_PHY_CHAN_PREFERRED 0x0029u
/* The radio-capability bitfield, one packed uint in the PHY_EXT block:
 * SPINEL_PROP_PHY_EXT__BEGIN (0x1200, spinel.h:1753) + 11 (spinel.h:1895). */
#define PROPS_ID_RADIO_CAPS 0x120Bu
/* The MAC block starts at 0x30, so addressing is 0x34..0x37 and modes 0x38/0x3B;
 * docs/protocol.md §10 lists the scan properties for these instead
 * (openthread/src/lib/spinel/spinel.h:1947 MAC__BEGIN). */
#define PROPS_ID_MAC_LADDR 0x0034u
#define PROPS_ID_MAC_SADDR 0x0035u
#define PROPS_ID_MAC_PANID 0x0036u
#define PROPS_ID_MAC_RAW_STREAM 0x0037u
#define PROPS_ID_MAC_PROMISCUOUS 0x0038u
#define PROPS_ID_MAC_RX_ON_WHEN_IDLE 0x003Bu
/* The source-match family: SPINEL_PROP_MAC_EXT__BEGIN (0x1300) + 3, + 4, + 5
 * (openthread/src/lib/spinel/spinel.h:2157-2169). Upstream implements GET only
 * for ENABLED and INSERT/REMOVE/SET for the two lists; what this firmware answers for each is
 * docs/protocol.md §10.2. */
#define PROPS_ID_MAC_SRC_MATCH_ENABLED 0x1303u
#define PROPS_ID_MAC_SRC_MATCH_SHORT 0x1304u
#define PROPS_ID_MAC_SRC_MATCH_EXT 0x1305u
/* The scan family (docs/protocol.md §10.1): ids pinned to spinel.h:1947-1956 -
 * MAC__BEGIN + 0, +1, +2 and +9. MAC_SCAN_RESULT is upstream's own id for the per-channel result
 * (spinel.h:1956), which a conformant RCP *never* answers with a GET or SET; what this firmware
 * extends is emitting it as an event, because a scan here is host-driven and a result has to
 * reach the host as a frame rather than as a callback. */
#define PROPS_ID_MAC_SCAN_STATE 0x0030u
#define PROPS_ID_MAC_SCAN_MASK 0x0031u
#define PROPS_ID_MAC_SCAN_PERIOD 0x0032u
#define PROPS_ID_MAC_SCAN_RESULT 0x0039u
#define PROPS_SCAN_STATE_IDLE 0u
#define PROPS_SCAN_STATE_BEACON 1u
#define PROPS_SCAN_STATE_ENERGY 2u
#define PROPS_SCAN_STATE_DISCOVER 3u
/* The "nothing was measured on this channel" level, upstream's +127 sentinel: it means no usable
 * sample, and a real -128 dBm reading stays distinguishable from it. */
#define PROPS_SCAN_LEVEL_NONE 127
#define PROPS_SCAN_MASK_DEFAULT 0x007FFF800u /* bits 11..26 set, upstream's own default */
#define PROPS_SCAN_PERIOD_DEFAULT 200u      /* upstream's energy-scan dwell default, in ms. Stored
                                               verbatim; the dwell a channel gets is bounded where it
                                               is used, so GET reports what was written */
#define PROPS_SCAN_MAX_LIST 32u /* a 32-bit mask can hold this many channel numbers, and the table
                                   stores any of them: the band restriction is what a *dwell* refuses,
                                   not what a GET forgets */

/* Capability numbers PROPS_ID_CAPS may carry, pinned to upstream spinel.h. Only what this
 * firmware implements is advertised; the numbers go on the wire one packed uint each because
 * OpenThread tests membership, not a bitmask (spinel_driver.hpp:206 CoprocessorHasCap ->
 * mCoprocessorCaps.Contains, and spinel_driver.cpp:416-426 parses the list element by
 * element). */
#define PROPS_CAP_CONFIG_RADIO 34u             /* CONFIG__BEGIN 32 + 2, spinel.h:1262 */
#define PROPS_CAP_RCP_API_VERSION 64u          /* RCP__BEGIN 64 + 0, spinel.h:1277 */
#define PROPS_CAP_RCP_MIN_HOST_API_VERSION 65u /* + 1, spinel.h:1278 */
#define PROPS_CAP_RCP_LOG_CRASH_DUMP 67u       /* + 3, spinel.h:1280 */
#define PROPS_CAP_MAC_RAW 513u                 /* OPENTHREAD__BEGIN 512 + 1, spinel.h:1285 */

/* The OT_RADIO_CAPS_* bits PROPS_ID_RADIO_CAPS advertises, pinned to upstream
 * openthread/include/openthread/platform/radio.h:129-137. The AT86RF233's
 * ARET engine reports the ACK wait (ACK_TIMEOUT), retries (TRANSMIT_RETRIES) and runs
 * CSMA-CA/CCA (CSMA_BACKOFF); MAC_RX_ON_WHEN_IDLE is a real property, and MAC_SCAN_STATE /
 * MAC_SCAN_MASK / MAC_SCAN_PERIOD are implemented for the energy kind only, so ENERGY_SCAN is
 * advertised. Beacon and discover are refused with UNIMPLEMENTED. TRANSMIT_SEC is the promise that
 * this firmware encrypts (rcp/mac_sec.h), because a host that reads it stops encrypting, and
 * TRANSMIT_TIMING that it honours txDelay: a board's build sets each only with the code. */
#define PROPS_RADIO_CAP_ACK_TIMEOUT (1u << 0)
#define PROPS_RADIO_CAP_ENERGY_SCAN (1u << 1)
#define PROPS_RADIO_CAP_TRANSMIT_RETRIES (1u << 2)
#define PROPS_RADIO_CAP_CSMA_BACKOFF (1u << 3)
#define PROPS_RADIO_CAP_TRANSMIT_SEC (1u << 5)
#define PROPS_RADIO_CAP_TRANSMIT_TIMING (1u << 6)
#define PROPS_RADIO_CAP_RX_ON_WHEN_IDLE (1u << 8)
#if defined(RCP_THREAD) && defined(RCP_TX_TIMING)
#define PROPS_RADIO_CAPS_THREAD (PROPS_RADIO_CAP_TRANSMIT_SEC | PROPS_RADIO_CAP_TRANSMIT_TIMING)
#elif defined(RCP_THREAD)
#define PROPS_RADIO_CAPS_THREAD PROPS_RADIO_CAP_TRANSMIT_SEC
#else
#define PROPS_RADIO_CAPS_THREAD 0u
#endif
#define PROPS_RADIO_CAPS_VALUE                                                                     \
    (PROPS_RADIO_CAP_ACK_TIMEOUT | PROPS_RADIO_CAP_ENERGY_SCAN |                                   \
     PROPS_RADIO_CAP_TRANSMIT_RETRIES | PROPS_RADIO_CAP_CSMA_BACKOFF |                             \
     PROPS_RADIO_CAP_RX_ON_WHEN_IDLE | PROPS_RADIO_CAPS_THREAD)

/* Wire types of docs/protocol.md §4, as far as this table needs them. */
enum
{
    PROPS_T_U8 = 1,   /* 'C'  */
    PROPS_T_I8,       /* 'c'  */
    PROPS_T_BOOL,     /* 'b'  */
    PROPS_T_U16,      /* 'S'  */
    PROPS_T_BE64,     /* 'E'  */
    PROPS_T_PACKED2,  /* 'ii' - two packed uints (PROTOCOL_VERSION) */
    PROPS_T_STR,      /* 'U'  - zero-terminated */
    PROPS_T_PACKED1,  /* 'i'  - one packed uint (RADIO_CAPS) */
    PROPS_T_CAP_LIST,  /* 'A(i)' - packed uints, one capability number each (CAPS) */
    PROPS_T_CHAN_LIST, /* 'A(C)' - a count byte then that many channel numbers (MAC_SCAN_MASK) */
    PROPS_T_CHAN_BAND, /* 'A(C)' as upstream encodes it: channel numbers, no count (PHY_CHAN_*) */
    /* The source-match lists: 'A(S)' and 'A(E)', a multiple of 2 and of 8 bytes, and the
     * one-entry forms INSERT and REMOVE carry. Decoded by length, so the type only names
     * which list it is. */
    PROPS_T_ADDR_LIST_SHORT,
    PROPS_T_ADDR_LIST_EXT,
    PROPS_T_MAC_SEC, /* decoded by rcp/mac_sec.c */
    PROPS_T_U64      /* 'X' */
};

#define PROPS_ACC_READ 1u
#define PROPS_ACC_WRITE 2u

typedef struct
{
    uint16_t id;
    uint8_t type;
    uint8_t access;
} Props_Desc;

typedef struct
{
    const char *ncpVersion;
    const uint8_t *hwAddr;
    int8_t rxSensitivityDbm; /* per silicon: AT86RF233 = -96 (docs/protocol.md §10) */
    int8_t txPowerMaxDbm;     /* the board's transmit cap at the transceiver, channels 11..25 */
    int8_t txPowerMaxCh26Dbm; /* and on channel 26 (docs/protocol.md §10) */
} Props_Identity;

typedef struct
{
    uint8_t phyEnabled;
    uint8_t channel;
    /* 1 from a PHY_CHAN write until the glue has the radio on it. A write is an instruction even when
     * it repeats the value: a frame's own channel and rxChannelAfterTx move the radio without touching
     * the property, and the host's write is what brings it back (docs/protocol.md §7.1). */
    uint8_t channelWritten;
    int8_t ccaThresholdDbm;
    int8_t txPowerDbm; /* what the host set; Props_TxPowerDbm is what the radio uses */
    int8_t txPowerMaxDbm;
    int8_t txPowerMaxCh26Dbm;
    int8_t rssiDbm;
    int8_t rxSensitivityDbm;
    uint16_t panId;
    uint16_t shortAddr;
    uint8_t extAddr[8];
#ifdef RCP_THREAD
    MacSec_State macSec;
    uint64_t radioTimeUs; /* the glue's clock, set before each frame is fed in */
#endif
    uint8_t rawStreamEnabled;
    uint8_t logEnabled; /* non-zero while the host reads the record stream */
    uint8_t rxOnWhenIdle;
    uint8_t promiscuousMode; /* 0 off, 1 network, 2 full (sniffer) */
    uint8_t scanState;     /* MAC_SCAN_STATE as GET reports it: 0 idle, 2 a dwell running */
    uint32_t scanMask;     /* MAC_SCAN_MASK: bit N set means channel N is in the list */
    uint16_t scanPeriodMs; /* MAC_SCAN_PERIOD: how long one channel is measured */
    uint8_t scanStart;     /* a dwell the host asked for that the app layer has not taken yet */
    /* The source-match table: state like any other property, read by the radio through
     * Props_SrcMatch (coding-rules.md §4). */
    SrcMatch_Table srcMatch;
    const char *ncpVersion;
    const uint8_t *hwAddr;
} Props_State;

void Props_Init(Props_State *s, const Props_Identity *id);

const Props_Desc *Props_Table(uint16_t *count);
const Props_Desc *Props_Find(uint32_t id);

uint32_t Props_Get(const Props_State *s, uint32_t id, U_BStream *dst);

uint32_t Props_Set(Props_State *s, uint32_t id, const uint8_t *value, uint16_t len);

/* INSERT and REMOVE carry one entry. Returns a SPINEL_STATUS_* value, the way Props_Set does;
 * a property without an insert or remove handler answers PROP_NOT_FOUND, which is upstream's
 * answer for the same request (ncp_base.cpp HandleCommandPropertyInsertRemove). */
uint32_t Props_Insert(Props_State *s, uint32_t id, const uint8_t *value, uint16_t len);
uint32_t Props_Remove(Props_State *s, uint32_t id, const uint8_t *value, uint16_t len);

void Props_SetRssi(Props_State *s, int8_t dbm);

/* The power a transmission on `channel` goes at: the host's PHY_TX_POWER, capped at the board's
 * limit for that channel - the lower channel 26 one on 26. PHY_TX_POWER reads back this value for
 * the current channel (docs/protocol.md §10). */
int8_t Props_TxPowerDbm(const Props_State *s, uint8_t channel);

/* The scan start is a request the app layer consumes: a property is state, and only the app
 * turns it into a HAL call (coding-rules.md §4). Reports 1 and fills the out-parameters when a
 * dwell is pending, clearing the request so it is acted on once. */
int Props_TakeScanStart(Props_State *s, uint32_t *mask, uint16_t *dwellMs);

/* Mirror the scan machine's state into what GET 0x30 reports, the way the RSSI mirror works. */
void Props_SetScanState(Props_State *s, uint8_t state);

#endif
