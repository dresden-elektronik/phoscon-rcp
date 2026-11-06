/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mac_sec.c — see mac_sec.h. */
#include "mac_sec.h"

#define MAC_SEC_FCS_LEN 2u
#define MAC_SEC_NONCE_LEN 13u

#define MAC_SEC_FCF_TYPE_MASK 0x0007u
#define MAC_SEC_FCF_TYPE_COMMAND 0x0003u
#define MAC_SEC_FCF_TYPE_ACK 0x0002u
#define MAC_SEC_FCF_SECURITY 0x0008u
#define MAC_SEC_FCF_FRAME_PENDING 0x0010u
#define MAC_SEC_FCF_ACK_REQUEST 0x0020u
#define MAC_SEC_FCF_PAN_COMPRESSION 0x0040u
#define MAC_SEC_FCF_SEQ_SUPPRESSED 0x0100u
#define MAC_SEC_FCF_IE_PRESENT 0x0200u

#define MAC_SEC_VERSION_2015 2u
#define MAC_SEC_ADDR_NONE 0u
#define MAC_SEC_ADDR_SHORT 2u
#define MAC_SEC_ADDR_EXT 3u

#define MAC_SEC_LEVEL_ENC_MIC_32 5u
#define MAC_SEC_KEY_ID_MODE_1 1u
#define MAC_SEC_SCF_THREAD 0x0Du /* level 5, key ID mode 1 */
/* Key indexes run 1..128 and wrap (IEEE 802.15.4 Key Index, zero is reserved). */
#define MAC_SEC_KEY_INDEX_MIN 1u
#define MAC_SEC_KEY_INDEX_MAX 128u

#define MAC_SEC_IE_TERMINATION_2 0x7Fu

/* Length, 16-bit, then the key: three of them after the two leading octets. */
#define MAC_SEC_KEYS_VALUE_LEN (2u + 3u * (2u + MAC_SEC_KEY_LEN))

static void (*macSecLockFn)(void);
static void (*macSecUnlockFn)(void);

void MacSec_SetLock(void (*lock)(void), void (*unlock)(void))
{
    macSecLockFn = lock;
    macSecUnlockFn = unlock;
}

static void macSecLock(void)
{
    if (macSecLockFn != 0)
        macSecLockFn();
}

static void macSecUnlock(void)
{
    if (macSecUnlockFn != 0)
        macSecUnlockFn();
}

static uint16_t macSecReadU16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t macSecReadU32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void macSecWriteU32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

void MacSec_Init(MacSec_State *s)
{
    if (s == 0)
        return;

    s->hasKeys = 0u;
    s->keyIndex = 0u;
    s->frameCounter = 0u;
    s->prevFrameCounter = 0u;
    Aes_KeyWipe(&s->prev);
    Aes_KeyWipe(&s->curr);
    Aes_KeyWipe(&s->next);
    {
        volatile uint8_t *p = (volatile uint8_t *)s->probes;
        uint16_t i;

        for (i = 0u; i < sizeof(s->probes); ++i)
            p[i] = 0u;
    }
}

static void macSecCopyKey(Aes_Key *to, const Aes_Key *from)
{
    const uint8_t *src = (const uint8_t *)from;
    uint8_t *dst = (uint8_t *)to;
    uint16_t i;

    for (i = 0u; i < sizeof(Aes_Key); ++i)
        dst[i] = src[i];
}

int MacSec_SetKeys(MacSec_State *s, const uint8_t *value, uint16_t len)
{
    /* Scheduled aside and copied in under the lock, because an Ack may be using the keys now. */
    static Aes_Key next[3];
    uint8_t k;
    int ok = 1;

    if (s == 0 || value == 0 || len != MAC_SEC_KEYS_VALUE_LEN)
        return 0;

    for (k = 0u; k < 3u; ++k)
    {
        if (macSecReadU16(&value[2u + k * (2u + MAC_SEC_KEY_LEN)]) != MAC_SEC_KEY_LEN)
            return 0;

        if (Aes_KeySchedule(&next[k], &value[4u + k * (2u + MAC_SEC_KEY_LEN)]) != ZB_OK)
            ok = 0;
    }

    if (ok)
    {
        macSecLock();
        /* value[0] is the key ID mode. A host may send either upstream form of mode 1, and upstream's
         * current RCP ignores it (radio_spinel.cpp SetMacKey); the frame's own header decides. */
        s->keyIndex = value[1];
        macSecCopyKey(&s->prev, &next[0]);
        macSecCopyKey(&s->curr, &next[1]);
        macSecCopyKey(&s->next, &next[2]);
        /* An Ack under the key just retired counts on from where that key stopped; the host sets the
         * counter for the new key itself, and keeps none for the previous one. */
        s->prevFrameCounter = s->frameCounter;
        s->hasKeys = 1u;
        macSecUnlock();
    }

    Aes_KeyWipe(&next[0]);
    Aes_KeyWipe(&next[1]);
    Aes_KeyWipe(&next[2]);
    return ok;
}

int MacSec_SetFrameCounter(MacSec_State *s, const uint8_t *value, uint16_t len)
{
    uint32_t counter;

    if (s == 0 || value == 0 || (len != 4u && len != 5u))
        return 0;

    counter = macSecReadU32(value);
    macSecLock();

    if (len == 4u || value[4] == 0u || counter > s->frameCounter)
        s->frameCounter = counter;

    macSecUnlock();
    return 1;
}

static uint8_t macSecAddrLen(uint8_t mode)
{
    if (mode == MAC_SEC_ADDR_SHORT)
        return 2u;

    return mode == MAC_SEC_ADDR_EXT ? 8u : 0u;
}

/* IEEE 802.15.4-2015 Table 7-2 for version 2; compression alone decides for the older versions. */
static void macSecPanIds(uint8_t version, uint8_t dstMode, uint8_t srcMode, int compressed, int *dstPan, int *srcPan)
{
    int hasDst = dstMode != MAC_SEC_ADDR_NONE;
    int hasSrc = srcMode != MAC_SEC_ADDR_NONE;

    if (version != MAC_SEC_VERSION_2015)
    {
        *dstPan = hasDst;
        *srcPan = hasSrc && !compressed;
    }
    else if (hasDst && hasSrc)
    {
        *dstPan = (dstMode == MAC_SEC_ADDR_EXT && srcMode == MAC_SEC_ADDR_EXT) ? !compressed : 1;
        *srcPan = !compressed && !(dstMode == MAC_SEC_ADDR_EXT && srcMode == MAC_SEC_ADDR_EXT);
    }
    else if (hasDst)
    {
        *dstPan = !compressed;
        *srcPan = 0;
    }
    else if (hasSrc)
    {
        *dstPan = 0;
        *srcPan = !compressed;
    }
    else
    {
        *dstPan = compressed;
        *srcPan = 0;
    }
}

static uint8_t macSecMicLen(uint8_t level)
{
    switch (level & 3u)
    {
        case 1u:
            return 4u;
        case 2u:
            return 8u;
        case 3u:
            return 16u;
        default:
            return 0u;
    }
}

/* Header IEs belong to the authenticated header. Stops at Termination 2 or at the end of the frame,
 * because a frame with no payload may leave the termination out. */
static int macSecSkipHeaderIes(const uint8_t *psdu, uint16_t *off, uint16_t end)
{
    while (*off < end)
    {
        uint16_t ie;
        uint16_t length;

        if ((uint32_t)*off + 2u > (uint32_t)end)
            return 0;

        ie = macSecReadU16(&psdu[*off]);
        length = (uint16_t)(ie & 0x7Fu);

        if ((uint32_t)*off + 2u + length > (uint32_t)end)
            return 0;

        *off = (uint16_t)(*off + 2u + length);

        if (((ie >> 7) & 0xFFu) == MAC_SEC_IE_TERMINATION_2)
            break;
    }

    return 1;
}

int MacSec_ParseLayout(const uint8_t *psdu, uint8_t len, MacSec_Layout *out)
{
    uint16_t fcf;
    uint16_t off;
    uint16_t end;
    uint8_t version;
    uint8_t dstMode;
    uint8_t srcMode;
    uint8_t scf;
    int dstPan;
    int srcPan;
    static const uint8_t keySourceLen[4] = {0u, 0u, 4u, 8u};

    if (psdu == 0 || out == 0 || len < 2u + MAC_SEC_FCS_LEN)
        return 0;

    fcf = macSecReadU16(psdu);

    if ((fcf & MAC_SEC_FCF_SECURITY) == 0u || (fcf & MAC_SEC_FCF_TYPE_MASK) > MAC_SEC_FCF_TYPE_COMMAND)
        return 0;

    version = (uint8_t)((fcf >> 12) & 3u);
    dstMode = (uint8_t)((fcf >> 10) & 3u);
    srcMode = (uint8_t)((fcf >> 14) & 3u);

    if (version > MAC_SEC_VERSION_2015 || dstMode == 1u || srcMode == 1u)
        return 0;

    macSecPanIds(version, dstMode, srcMode, (fcf & MAC_SEC_FCF_PAN_COMPRESSION) != 0u, &dstPan, &srcPan);

    off = 2u;

    if (version != MAC_SEC_VERSION_2015 || (fcf & MAC_SEC_FCF_SEQ_SUPPRESSED) == 0u)
        off = (uint16_t)(off + 1u);

    off = (uint16_t)(off + (dstPan ? 2u : 0u) + macSecAddrLen(dstMode));
    off = (uint16_t)(off + (srcPan ? 2u : 0u) + macSecAddrLen(srcMode));
    end = (uint16_t)(len - MAC_SEC_FCS_LEN);

    /* Security Control, then a four-octet frame counter. */
    if ((uint32_t)off + 5u > (uint32_t)end)
        return 0;

    scf = psdu[off];
    out->auxOffset = (uint8_t)off;
    out->level = (uint8_t)(scf & 7u);
    out->keyIdMode = (uint8_t)((scf >> 3) & 3u);
    out->micLen = macSecMicLen(out->level);

    if (out->level == 0u)
        return 0;

    off = (uint16_t)(off + 5u + keySourceLen[out->keyIdMode] + (out->keyIdMode != 0u ? 1u : 0u));

    if ((uint32_t)off + out->micLen > (uint32_t)end)
        return 0;

    end = (uint16_t)(end - out->micLen);

    if (version == MAC_SEC_VERSION_2015 && (fcf & MAC_SEC_FCF_IE_PRESENT) != 0u)
    {
        if (!macSecSkipHeaderIes(psdu, &off, end))
            return 0;
    }

    /* A command identifier is part of the header before version 2, and of the encrypted payload in
     * version 2 (mac_frame.cpp, ParseInfo::ParseFrom). */
    if ((fcf & MAC_SEC_FCF_TYPE_MASK) == MAC_SEC_FCF_TYPE_COMMAND && version != MAC_SEC_VERSION_2015)
    {
        if (off >= end)
            return 0;

        off = (uint16_t)(off + 1u);
    }

    out->adataLen = (uint8_t)off;
    out->payloadLen = (uint8_t)(end - off);
    return 1;
}

/* Where a frame's fields are, from its Frame Control alone, as far as avail reaches. */
typedef struct
{
    uint16_t fcf;
    uint8_t version;
    uint8_t dstMode;
    uint8_t srcMode;
    uint8_t seqOffset; /* 0 = suppressed */
    uint8_t panOffset;  /* the PAN ID the Ack addresses; 0 = neither is carried */
    uint8_t srcOffset;
    uint8_t end; /* the first octet behind the addressing */
} MacSec_Addressing;

static int macSecAddressing(const uint8_t *psdu, uint8_t avail, MacSec_Addressing *out)
{
    uint16_t off;
    int dstPan;
    int srcPan;
    uint8_t dstPanAt;

    if (psdu == 0 || avail < 2u)
        return 0;

    out->fcf = macSecReadU16(psdu);
    out->version = (uint8_t)((out->fcf >> 12) & 3u);
    out->dstMode = (uint8_t)((out->fcf >> 10) & 3u);
    out->srcMode = (uint8_t)((out->fcf >> 14) & 3u);

    if (out->version > MAC_SEC_VERSION_2015 || out->dstMode == 1u || out->srcMode == 1u)
        return 0;

    macSecPanIds(out->version, out->dstMode, out->srcMode, (out->fcf & MAC_SEC_FCF_PAN_COMPRESSION) != 0u, &dstPan, &srcPan);

    off = 2u;
    out->seqOffset = 0u;

    if (out->version != MAC_SEC_VERSION_2015 || (out->fcf & MAC_SEC_FCF_SEQ_SUPPRESSED) == 0u)
    {
        out->seqOffset = 2u;
        off = 3u;
    }

    dstPanAt = (uint8_t)off;
    off = (uint16_t)(off + (dstPan ? 2u : 0u) + macSecAddrLen(out->dstMode));
    /* The source PAN if it is there, else the destination's (mac_frame.cpp GenerateEnhAck). */
    out->panOffset = srcPan ? (uint8_t)off : (dstPan ? dstPanAt : 0u);
    off = (uint16_t)(off + (srcPan ? 2u : 0u));
    out->srcOffset = (uint8_t)off;
    off = (uint16_t)(off + macSecAddrLen(out->srcMode));

    if (off > avail)
        return 0;

    out->end = (uint8_t)off;
    return 1;
}

int MacSec_SetProbe(MacSec_State *s, const uint8_t *value, uint16_t len)
{
    MacSec_Probe *slot = 0;
    uint8_t i;
    uint8_t k;

    if (s == 0 || value == 0 || len != 2u + MAC_SEC_EXT_LEN + 1u)
        return 0;

    for (i = 0u; i < MAC_SEC_PROBE_MAX; ++i)
    {
        MacSec_Probe *p = &s->probes[i];
        int same = p->metrics != 0u;

        for (k = 0u; same && k < MAC_SEC_EXT_LEN; ++k)
            same = p->ext[k] == value[2u + k];

        if (same)
        {
            slot = p;
            break;
        }

        if (slot == 0 && p->metrics == 0u)
            slot = p;
    }

    if (slot == 0)
        return value[2u + MAC_SEC_EXT_LEN] == 0u;

    macSecLock();
    slot->shortAddr = macSecReadU16(value);

    for (k = 0u; k < MAC_SEC_EXT_LEN; ++k)
        slot->ext[k] = value[2u + k];

    slot->metrics = (uint8_t)(value[2u + MAC_SEC_EXT_LEN] & (MAC_SEC_PROBE_LQI | MAC_SEC_PROBE_LINK_MARGIN | MAC_SEC_PROBE_RSSI));
    macSecUnlock();
    return 1;
}

uint8_t MacSec_ProbeIe(const MacSec_State *s,
                       const MacSec_AckRx *rx,
                       uint8_t lqi,
                       int8_t rssi,
                       int8_t noiseFloor,
                       uint8_t ie[MAC_SEC_PROBE_IE_MAX])
{
    uint8_t i;
    uint8_t n = 6u;

    if (s == 0 || rx == 0 || ie == 0)
        return 0u;

    for (i = 0u; i < MAC_SEC_PROBE_MAX; ++i)
    {
        const MacSec_Probe *p = &s->probes[i];
        int same = p->metrics != 0u;

        if (same && rx->srcMode == MAC_SEC_ADDR_SHORT)
        {
            same = p->shortAddr == rx->srcShort;
        }
        else if (same && rx->srcMode == MAC_SEC_ADDR_EXT)
        {
            uint8_t k;

            for (k = 0u; same && k < MAC_SEC_EXT_LEN; ++k)
                same = p->ext[k] == rx->srcExt[MAC_SEC_EXT_LEN - 1u - k];
        }
        else
        {
            same = 0;
        }

        if (same)
        {
            /* Two values at most, in this order (Thread 1.2, Link Metrics; OpenThread's own). */
            if ((p->metrics & MAC_SEC_PROBE_LQI) != 0u)
                ie[n++] = lqi;

            if ((p->metrics & MAC_SEC_PROBE_LINK_MARGIN) != 0u)
            {
                int16_t margin = (int16_t)rssi - noiseFloor;

                margin = margin < 0 ? 0 : (margin > 130 ? 130 : margin);
                ie[n++] = (uint8_t)((uint16_t)margin * 255u / 130u);
            }

            if (n < MAC_SEC_PROBE_IE_MAX && (p->metrics & MAC_SEC_PROBE_RSSI) != 0u)
            {
                int16_t level = (int16_t)rssi + 130;

                level = level < 0 ? 0 : (level > 130 ? 130 : level);
                ie[n++] = (uint8_t)((uint16_t)level * 255u / 130u);
            }

            /* A vendor Header IE, element 0, under Thread's OUI, subtype 0. */
            ie[0] = (uint8_t)(n - 2u);
            ie[1] = 0u;
            ie[2] = 0x9Bu;
            ie[3] = 0xB8u;
            ie[4] = 0xEAu;
            ie[5] = 0u;
            return n;
        }
    }

    return 0u;
}

int MacSec_ParseAckRx(const uint8_t *rx, uint8_t avail, MacSec_AckRx *out)
{
    MacSec_Addressing a;

    if (out == 0 || !macSecAddressing(rx, avail, &a))
        return 0;

    out->version = a.version;
    out->type = (uint8_t)(a.fcf & MAC_SEC_FCF_TYPE_MASK);
    out->srcMode = a.srcMode;
    out->srcShort = a.srcMode == MAC_SEC_ADDR_SHORT ? macSecReadU16(&rx[a.srcOffset]) : 0u;
    out->srcExt = a.srcMode == MAC_SEC_ADDR_EXT ? &rx[a.srcOffset] : 0;
    return 1;
}

int MacSec_AckPending(const MacSec_AckRx *rx, const SrcMatch_Table *table)
{
    if (!table->enabled)
        return rx->type == MAC_SEC_FCF_TYPE_COMMAND;

    if (rx->srcMode == MAC_SEC_ADDR_SHORT)
        return SrcMatch_HasShort(table, rx->srcShort);

    if (rx->srcMode == MAC_SEC_ADDR_EXT)
        return SrcMatch_HasExt(table, rx->srcExt);

    return 0;
}

uint8_t MacSec_BuildEnhAckHeader(MacSec_State *s,
                                 const uint8_t *rx,
                                 uint8_t avail,
                                 const uint8_t ext8[MAC_SEC_EXT_LEN],
                                 int framePending,
                                 const uint8_t *ie,
                                 uint8_t ieLen,
                                 uint8_t *ack,
                                 MacSec_AckResult *result,
                                 MacSec_AckMic *mic)
{
    MacSec_Addressing a;
    uint16_t ackFcf;
    uint8_t n;
    uint8_t k;

    if (s == 0 || ext8 == 0 || ack == 0 || result == 0 || mic == 0 || !macSecAddressing(rx, avail, &a))
        return 0u;

    mic->adataLen = 0u;

    result->secured = 0;
    result->keyIndex = 0u;
    result->frameCounter = 0u;

    if (a.version != MAC_SEC_VERSION_2015 || a.seqOffset == 0u || a.srcMode == MAC_SEC_ADDR_NONE ||
        (a.fcf & MAC_SEC_FCF_ACK_REQUEST) == 0u || a.dstMode == MAC_SEC_ADDR_NONE)
        return 0u;

    ackFcf = (uint16_t)(MAC_SEC_FCF_TYPE_ACK | ((uint16_t)MAC_SEC_VERSION_2015 << 12) | ((uint16_t)a.srcMode << 10));

    if (framePending)
        ackFcf |= MAC_SEC_FCF_FRAME_PENDING;

    if (a.panOffset == 0u)
        ackFcf |= MAC_SEC_FCF_PAN_COMPRESSION;

    if ((a.fcf & MAC_SEC_FCF_SECURITY) != 0u)
        ackFcf |= MAC_SEC_FCF_SECURITY;

    if (ieLen != 0u)
        ackFcf |= MAC_SEC_FCF_IE_PRESENT;

    ack[0] = (uint8_t)(ackFcf & 0xFFu);
    ack[1] = (uint8_t)(ackFcf >> 8);
    ack[2] = rx[a.seqOffset];
    n = 3u;

    if (a.panOffset != 0u)
    {
        ack[n++] = rx[a.panOffset];
        ack[n++] = rx[a.panOffset + 1u];
    }

    for (k = 0u; k < macSecAddrLen(a.srcMode); ++k)
        ack[n++] = rx[a.srcOffset + k];

    if ((a.fcf & MAC_SEC_FCF_SECURITY) != 0u)
    {
        const Aes_Key *key;
        uint32_t counter;
        uint8_t keyIndex;
        uint8_t prev;
        uint8_t next;

        /* The auxiliary header of a frame in level 5, key ID mode 1: control, counter, key index. */
        if (!s->hasKeys || (uint16_t)a.end + 6u > avail || rx[a.end] != MAC_SEC_SCF_THREAD)
            return 0u;

        keyIndex = rx[a.end + 5u];
        prev = (uint8_t)(s->keyIndex == MAC_SEC_KEY_INDEX_MIN ? MAC_SEC_KEY_INDEX_MAX : s->keyIndex - 1u);
        next = (uint8_t)(s->keyIndex == MAC_SEC_KEY_INDEX_MAX ? MAC_SEC_KEY_INDEX_MIN : s->keyIndex + 1u);

        if (keyIndex == s->keyIndex)
        {
            if (s->frameCounter == 0xFFFFFFFFuL)
                return 0u;

            key = &s->curr;
            counter = s->frameCounter++;
        }
        else if (keyIndex == prev)
        {
            if (s->prevFrameCounter == 0xFFFFFFFFuL)
                return 0u;

            key = &s->prev;
            counter = s->prevFrameCounter++;
        }
        else if (keyIndex == next)
        {
            /* The host keeps no counter for a key it has not moved to yet, and starts it from zero. */
            key = &s->next;
            counter = 0u;
        }
        else
        {
            return 0u;
        }

        ack[n++] = MAC_SEC_SCF_THREAD;
        macSecWriteU32(&ack[n], counter);
        n = (uint8_t)(n + 4u);
        ack[n++] = keyIndex;

        for (k = 0u; k < ieLen; ++k)
            ack[n++] = ie[k];

        for (k = 0u; k < MAC_SEC_EXT_LEN; ++k)
            mic->nonce[k] = ext8[k];

        mic->nonce[8] = (uint8_t)(counter >> 24);
        mic->nonce[9] = (uint8_t)((counter >> 16) & 0xFFu);
        mic->nonce[10] = (uint8_t)((counter >> 8) & 0xFFu);
        mic->nonce[11] = (uint8_t)(counter & 0xFFu);
        mic->nonce[12] = MAC_SEC_LEVEL_ENC_MIC_32;

        mic->key = key;
        mic->adataLen = n;

        for (k = 0u; k < 4u; ++k)
            ack[n + k] = 0u;

        n = (uint8_t)(n + 4u);
        result->secured = 1;
        result->keyIndex = keyIndex;
        result->frameCounter = counter;
    }
    else
    {
        for (k = 0u; k < ieLen; ++k)
            ack[n++] = ie[k];
    }

    ack[n++] = 0u;
    ack[n++] = 0u;
    return n;
}

int MacSec_EnhAckMic(const MacSec_AckMic *later, const uint8_t *ack, uint8_t mic[4])
{
    return later != 0 && ack != 0 && mic != 0 &&
           Aes_CcmEncrypt(later->key, later->nonce, MAC_SEC_NONCE_LEN, ack, later->adataLen, mic, 0u, 4u, mic, mic) == ZB_OK;
}

uint8_t MacSec_BuildEnhAck(MacSec_State *s,
                           const uint8_t *rx,
                           uint8_t avail,
                           const uint8_t ext8[MAC_SEC_EXT_LEN],
                           int framePending,
                           const uint8_t *ie,
                           uint8_t ieLen,
                           uint8_t *ack,
                           MacSec_AckResult *result)
{
    MacSec_AckMic mic;
    uint8_t n = MacSec_BuildEnhAckHeader(s, rx, avail, ext8, framePending, ie, ieLen, ack, result, &mic);

    if (n != 0u && mic.adataLen != 0u && !MacSec_EnhAckMic(&mic, ack, &ack[mic.adataLen]))
        return 0u;

    return n;
}

static const Aes_Key *macSecSelectKey(const MacSec_State *s, uint8_t keyIndex)
{
    uint8_t prev = (uint8_t)(s->keyIndex == MAC_SEC_KEY_INDEX_MIN ? MAC_SEC_KEY_INDEX_MAX : s->keyIndex - 1u);
    uint8_t next = (uint8_t)(s->keyIndex == MAC_SEC_KEY_INDEX_MAX ? MAC_SEC_KEY_INDEX_MIN : s->keyIndex + 1u);

    if (keyIndex != s->keyIndex && s->keyIndex >= MAC_SEC_KEY_INDEX_MIN && s->keyIndex <= MAC_SEC_KEY_INDEX_MAX)
    {
        if (keyIndex == prev)
            return &s->prev;

        if (keyIndex == next)
            return &s->next;
    }

    return &s->curr;
}

int MacSec_SecureTx(MacSec_State *s,
                    uint8_t *psdu,
                    uint8_t len,
                    const uint8_t ext8[MAC_SEC_EXT_LEN],
                    int headerUpdated,
                    int securityProcessed,
                    MacSec_TxResult *result)
{
    MacSec_Layout layout;
    uint8_t nonce[MAC_SEC_NONCE_LEN];
    uint8_t *counterAt;
    uint8_t *keyIndexAt;
    uint8_t k;

    if (s == 0 || psdu == 0 || ext8 == 0 || result == 0 || len < 1u)
        return 0;

    result->secured = (psdu[0] & MAC_SEC_FCF_SECURITY) != 0u;
    result->headerUpdated = headerUpdated;
    result->keyIndex = 0u;
    result->frameCounter = 0u;

    if (!result->secured)
        return 1;

    if (!MacSec_ParseLayout(psdu, len, &layout))
        return 0;

    counterAt = &psdu[layout.auxOffset + 1u];
    keyIndexAt = &psdu[layout.auxOffset + 5u];

    /* The host finished this one itself: key ID mode 2 for an MLE Announce or Discovery, say. */
    if (securityProcessed)
    {
        result->frameCounter = macSecReadU32(counterAt);
        result->keyIndex = layout.keyIdMode == MAC_SEC_KEY_ID_MODE_1 ? *keyIndexAt : 0u;
        return 1;
    }

    /* Anything else would go out with the bit set and the payload in the clear. */
    if (!s->hasKeys || layout.keyIdMode != MAC_SEC_KEY_ID_MODE_1 || layout.level != MAC_SEC_LEVEL_ENC_MIC_32)
        return 0;

    if (!headerUpdated)
    {
        uint32_t counter;

        macSecLock();
        counter = s->frameCounter;

        /* A frame counter must never repeat under one key. */
        if (counter != 0xFFFFFFFFuL)
            s->frameCounter = counter + 1u;

        macSecUnlock();

        if (counter == 0xFFFFFFFFuL)
            return 0;

        *keyIndexAt = s->keyIndex;
        macSecWriteU32(counterAt, counter);
        result->headerUpdated = 1;
    }

    result->keyIndex = *keyIndexAt;
    result->frameCounter = macSecReadU32(counterAt);

    /* The nonce: the source's extended address most significant octet first, the frame counter big
     * endian, the security level (IEEE 802.15.4-2015 9.3.2.2). */
    for (k = 0u; k < MAC_SEC_EXT_LEN; ++k)
        nonce[k] = ext8[k];

    nonce[8] = (uint8_t)(result->frameCounter >> 24);
    nonce[9] = (uint8_t)((result->frameCounter >> 16) & 0xFFu);
    nonce[10] = (uint8_t)((result->frameCounter >> 8) & 0xFFu);
    nonce[11] = (uint8_t)(result->frameCounter & 0xFFu);
    nonce[12] = layout.level;

    return Aes_CcmEncrypt(macSecSelectKey(s, *keyIndexAt),
                          nonce,
                          MAC_SEC_NONCE_LEN,
                          psdu,
                          layout.adataLen,
                          &psdu[layout.adataLen],
                          layout.payloadLen,
                          layout.micLen,
                          &psdu[layout.adataLen],
                          &psdu[layout.adataLen + layout.payloadLen]) == ZB_OK;
}
