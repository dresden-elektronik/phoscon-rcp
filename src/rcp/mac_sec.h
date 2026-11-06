/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mac_sec.h — transmit security for an OpenThread host (docs/protocol.md §7.1): the three MAC
 * keys, the frame counter, and securing a frame before it reaches the radio. Core code: the state
 * is the caller's, and nothing here touches the radio. */
#ifndef RCP_MAC_SEC_H
#define RCP_MAC_SEC_H

#include <stdint.h>

#include "aes.h"
#include "srcmatch.h"

#define MAC_SEC_KEY_LEN 16u
#define MAC_SEC_EXT_LEN 8u

/* Where a secured frame's fields are. adataLen is the authenticated header, FCF through the last
 * octet before the payload; the payload runs from there to the MIC, and the FCS comes last. */
typedef struct
{
    uint8_t auxOffset; /* the Security Control octet */
    uint8_t level;
    uint8_t keyIdMode;
    uint8_t adataLen;
    uint8_t payloadLen;
    uint8_t micLen;
} MacSec_Layout;

/* Enhanced-Ack based Link Metrics probing: who asked, and for which metrics. */
#define MAC_SEC_PROBE_MAX 8u
#define MAC_SEC_PROBE_LQI 0x02u
#define MAC_SEC_PROBE_LINK_MARGIN 0x04u
#define MAC_SEC_PROBE_RSSI 0x08u
#define MAC_SEC_PROBE_IE_MAX 8u /* the IE's two octets, Thread's OUI and subtype, two metric octets */

typedef struct
{
    uint16_t shortAddr;
    uint8_t ext[MAC_SEC_EXT_LEN]; /* as Spinel carries it */
    uint8_t metrics;              /* 0 = this slot is free */
} MacSec_Probe;

typedef struct
{
    uint8_t hasKeys;
    uint8_t keyIndex;
    uint32_t frameCounter;
    uint32_t prevFrameCounter; /* for an Enhanced Ack under the key before: see MacSec_SetKeys */
    Aes_Key prev;
    Aes_Key curr;
    Aes_Key next;
    MacSec_Probe probes[MAC_SEC_PROBE_MAX];
} MacSec_State;

/* What MacSec_SecureTx did, for the transmit completion. */
typedef struct
{
    int secured;       /* the frame has the Security Enabled bit */
    int headerUpdated; /* its key index and frame counter are final */
    uint8_t keyIndex;
    uint32_t frameCounter;
} MacSec_TxResult;

/* What a frame still arriving says about its Ack. */
typedef struct
{
    uint8_t version;
    uint8_t type;
    uint8_t srcMode; /* 0 = none, 2 = short, 3 = extended */
    uint16_t srcShort;
    const uint8_t *srcExt; /* on-air order, inside the caller's buffer */
} MacSec_AckRx;

/* An Enhanced Ack's security, for the received frame's metadata. */
typedef struct
{
    int secured;
    uint8_t keyIndex;
    uint32_t frameCounter;
} MacSec_AckResult;

/* A key and a counter are written from the main loop and read from the radio interrupt, which an
 * Enhanced Ack is built in, so the glue names how to hold that interrupt off. Core code cannot. */
void MacSec_SetLock(void (*lock)(void), void (*unlock)(void));

/* Forget everything: the firmware keeps no state across a reset. */
void MacSec_Init(MacSec_State *s);

/* SPINEL_PROP_RCP_MAC_KEY: key ID mode, key index, then three keys each with a 16-bit length.
 * 1 = stored; 0 = the value is malformed, and what was stored stands. */
int MacSec_SetKeys(MacSec_State *s, const uint8_t *value, uint16_t len);

/* SPINEL_PROP_RCP_MAC_FRAME_COUNTER: the counter, and optionally a flag that only a larger value
 * may replace it. 1 = accepted. */
int MacSec_SetFrameCounter(MacSec_State *s, const uint8_t *value, uint16_t len);

/* 1 = the frame parses as far as its payload. Version 2 frames have their own PAN ID presence
 * rules, a suppressible sequence number and header IEs (IEEE 802.15.4-2015 7.2). */
int MacSec_ParseLayout(const uint8_t *psdu, uint8_t len, MacSec_Layout *out);

/* SPINEL_PROP_RCP_ENH_ACK_PROBING: short address, extended address, metrics; no metrics ends it. 1 =
 * accepted, 0 = malformed or no room. */
int MacSec_SetProbe(MacSec_State *s, const uint8_t *value, uint16_t len);

/* The probing IE an Ack to this frame carries, built from the metrics of the frame before it, as the
 * radio has no metrics yet for one still arriving. Returns its length, 0 when the source did not ask. */
uint8_t MacSec_ProbeIe(const MacSec_State *s,
                       const MacSec_AckRx *rx,
                       uint8_t lqi,
                       int8_t rssi,
                       int8_t noiseFloor,
                       uint8_t ie[MAC_SEC_PROBE_IE_MAX]);

/* 1 = the first avail octets of a frame hold its addressing. */
int MacSec_ParseAckRx(const uint8_t *rx, uint8_t avail, MacSec_AckRx *out);

/* The Frame Pending bit of its Enhanced Ack, from a table in on-air order. A table that is off cannot
 * say, which a poll hears as pending; a data frame has nothing to learn then. */
int MacSec_AckPending(const MacSec_AckRx *rx, const SrcMatch_Table *table);

/* Build the Enhanced Ack of a version 2 frame into ack, from as much of the frame as has arrived: its
 * addressing, and its auxiliary security header when it is secured (IEEE 802.15.4-2015 6.7.4.2, 7.3.3).
 * The Ack mirrors the frame's security, under the key the frame names, with the given Header IEs. It
 * has room for MAC_SEC_ACK_MAX octets, its FCS placeholder included. Returns its length, or 0 when the
 * frame must not be answered this way. */
#define MAC_SEC_ACK_MAX 37u
uint8_t MacSec_BuildEnhAck(MacSec_State *s,
                           const uint8_t *rx,
                           uint8_t avail,
                           const uint8_t ext8[MAC_SEC_EXT_LEN],
                           int framePending,
                           const uint8_t *ie,
                           uint8_t ieLen,
                           uint8_t *ack,
                           MacSec_AckResult *result);

/* The same Ack in two steps, for a radio that starts sending before the MIC is known: BuildEnhAckHeader
 * leaves the MIC and FCS octets zero and says what the MIC needs, and EnhAckMic computes it. A radio
 * that reads its buffer as it transmits has until the MIC's first octet reaches the air. */
typedef struct
{
    const Aes_Key *key;
    uint8_t nonce[13];
    uint8_t adataLen; /* where the MIC goes; 0 = the Ack is not secured, and has no MIC */
} MacSec_AckMic;

uint8_t MacSec_BuildEnhAckHeader(MacSec_State *s,
                                 const uint8_t *rx,
                                 uint8_t avail,
                                 const uint8_t ext8[MAC_SEC_EXT_LEN],
                                 int framePending,
                                 const uint8_t *ie,
                                 uint8_t ieLen,
                                 uint8_t *ack,
                                 MacSec_AckResult *result,
                                 MacSec_AckMic *mic);

/* 1 = the four octets of mic are that Ack's, computed over the ack the call above built. */
int MacSec_EnhAckMic(const MacSec_AckMic *later, const uint8_t *ack, uint8_t mic[4]);

/* Make a frame ready to send, in place: psdu is the whole PSDU with room for its MIC and FCS.
 * ext8 is the host's MAC_15_4_LADDR, as Spinel carries it, which is the nonce's own order.
 * 1 = send it; 0 = it must not be sent, because it would go out marked secured but unencrypted. */
int MacSec_SecureTx(MacSec_State *s,
                    uint8_t *psdu,
                    uint8_t len,
                    const uint8_t ext8[MAC_SEC_EXT_LEN],
                    int headerUpdated,
                    int securityProcessed,
                    MacSec_TxResult *result);

#endif
