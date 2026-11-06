/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mac.c — see mac.h. */
#include "mac.h"

/* FCF subfields (IEEE 802.15.4-2006 7.2.1.1). */
#define MAC_FCF_TYPE_MASK 0x0007u
#define MAC_FCF_SECURITY 0x0008u
#define MAC_FCF_FRAME_PENDING 0x0010u
#define MAC_FCF_ACK_REQUEST 0x0020u
#define MAC_FCF_PAN_COMPRESSION 0x0040u
#define MAC_FCF_SEQ_SUPPRESSED 0x0100u
#define MAC_FCF_DST_MODE_MASK 0x0C00u
#define MAC_FCF_DST_MODE_SHIFT 10
#define MAC_FCF_VERSION_MASK 0x3000u
#define MAC_FCF_VERSION_SHIFT 12
#define MAC_VERSION_2015 2u
#define MAC_FCF_SRC_MODE_MASK 0xC000u
#define MAC_FCF_SRC_MODE_SHIFT 14

#define MAC_FCF_AND_SEQ 3u
#define MAC_PAN_FIELD 2u
#define MAC_PAN_BROADCAST 0xFFFFu
#define MAC_SHORT_BROADCAST 0xFFFFu

/* FCF octet 1 bit 5 of the acknowledgment frame is the Frame Pending subfield. */
#define MAC_ACK_FCF_PENDING 0x10u
#define MAC_ACK_FCF_TYPE 0x02u

static uint8_t macAddrLen(uint8_t mode)
{
    if (mode == MAC_ADDR_MODE_SHORT)
        return MAC_ADDR_LEN_SHORT;

    if (mode == MAC_ADDR_MODE_EXT)
        return MAC_ADDR_LEN_EXT;

    return 0u;
}

static uint16_t macReadU16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint8_t Mac_HeaderLenFromFcf(uint16_t fcf)
{
    uint8_t dstMode = (uint8_t)((fcf & MAC_FCF_DST_MODE_MASK) >> MAC_FCF_DST_MODE_SHIFT);
    uint8_t srcMode = (uint8_t)((fcf & MAC_FCF_SRC_MODE_MASK) >> MAC_FCF_SRC_MODE_SHIFT);
    uint8_t panComp = (fcf & MAC_FCF_PAN_COMPRESSION) ? 1u : 0u;
    uint8_t len = (uint8_t)(MAC_FCF_AND_SEQ);

    /* A security header's length is not in the FCF, so the header cannot be measured from
     * two octets: the caller reads the frame whole instead. */
    if ((fcf & MAC_FCF_SECURITY) != 0u)
        return 0u;

    if (dstMode == 1u || srcMode == 1u)
        return 0u;

    if (dstMode != MAC_ADDR_MODE_NONE)
        len = (uint8_t)(len + MAC_PAN_FIELD + macAddrLen(dstMode));

    if (srcMode != MAC_ADDR_MODE_NONE)
    {
        if (!panComp)
            len = (uint8_t)(len + MAC_PAN_FIELD);

        len = (uint8_t)(len + macAddrLen(srcMode));
    }

    return len;
}

/* Which PAN fields a frame carries. Before version 2 a destination address brings its PAN, and a source
 * address its own unless compressed. Version 2 is a table of its own (IEEE 802.15.4-2015 Table 7-2). */
static void macPanFields(const Mac_Hdr *hdr, int *dstPan, int *srcPan)
{
    int dst = hdr->dstMode != MAC_ADDR_MODE_NONE;
    int src = hdr->srcMode != MAC_ADDR_MODE_NONE;
    int compressed = hdr->panCompression != 0u;

    if (hdr->version != MAC_VERSION_2015)
    {
        *dstPan = dst;
        *srcPan = src && !compressed;
    }
    else if (dst && src)
    {
        int bothExt = hdr->dstMode == MAC_ADDR_MODE_EXT && hdr->srcMode == MAC_ADDR_MODE_EXT;

        *dstPan = !bothExt || !compressed;
        *srcPan = !bothExt && !compressed;
    }
    else if (!dst && !src)
    {
        *dstPan = compressed;
        *srcPan = 0;
    }
    else
    {
        *dstPan = dst && !compressed;
        *srcPan = src && !compressed;
    }
}

int Mac_ParseHeader(const uint8_t *psdu, uint16_t len, Mac_Hdr *out)
{
    uint16_t fcf;
    uint16_t off;
    uint8_t addrLen;
    int dstPan;
    int srcPan;

    if (psdu == 0 || out == 0 || len < MAC_FCF_AND_SEQ)
        return 0;

    fcf = macReadU16(psdu);

    out->type = (uint8_t)(fcf & MAC_FCF_TYPE_MASK);
    out->security = (fcf & MAC_FCF_SECURITY) ? 1u : 0u;
    out->framePending = (fcf & MAC_FCF_FRAME_PENDING) ? 1u : 0u;
    out->ackRequest = (fcf & MAC_FCF_ACK_REQUEST) ? 1u : 0u;
    out->panCompression = (fcf & MAC_FCF_PAN_COMPRESSION) ? 1u : 0u;
    out->version = (uint8_t)((fcf & MAC_FCF_VERSION_MASK) >> MAC_FCF_VERSION_SHIFT);
    out->dstMode = (uint8_t)((fcf & MAC_FCF_DST_MODE_MASK) >> MAC_FCF_DST_MODE_SHIFT);
    out->srcMode = (uint8_t)((fcf & MAC_FCF_SRC_MODE_MASK) >> MAC_FCF_SRC_MODE_SHIFT);
    out->seq = 0u;

    out->hasDstPan = 0u;
    out->hasSrcPan = 0u;
    out->dstPan = 0u;
    out->srcPan = 0u;
    out->dstShort = 0u;
    out->srcShort = 0u;
    out->headerLen = 0u;

    /* Mode 1 is reserved. A security header follows the addressing fields, so they parse the
     * same with or without one; headerLen then ends where the auxiliary header starts, and
     * nothing behind it - the command identifier included - is read (Mac_IsDataRequest). */
    if (out->dstMode == 1u || out->srcMode == 1u)
        return 0;

    off = 2u;

    if (out->version != MAC_VERSION_2015 || (fcf & MAC_FCF_SEQ_SUPPRESSED) == 0u)
    {
        out->seq = psdu[2];
        off = MAC_FCF_AND_SEQ;
    }

    macPanFields(out, &dstPan, &srcPan);

    if (dstPan)
    {
        if ((uint32_t)off + MAC_PAN_FIELD > (uint32_t)len)
            return 0;

        out->dstPan = macReadU16(&psdu[off]);
        out->hasDstPan = 1u;
        off = (uint16_t)(off + MAC_PAN_FIELD);
    }

    if (out->dstMode != MAC_ADDR_MODE_NONE)
    {
        addrLen = macAddrLen(out->dstMode);

        if ((uint32_t)off + addrLen > (uint32_t)len)
            return 0;

        if (out->dstMode == MAC_ADDR_MODE_SHORT)
            out->dstShort = macReadU16(&psdu[off]);
        else
        {
            uint8_t k;

            for (k = 0; k < MAC_ADDR_LEN_EXT; ++k)
                out->dstExt[k] = psdu[off + k];
        }

        off = (uint16_t)(off + addrLen);
    }

    if (srcPan)
    {
        if ((uint32_t)off + MAC_PAN_FIELD > (uint32_t)len)
            return 0;

        out->srcPan = macReadU16(&psdu[off]);
        out->hasSrcPan = 1u;
        off = (uint16_t)(off + MAC_PAN_FIELD);
    }

    if (out->srcMode != MAC_ADDR_MODE_NONE)
    {
        addrLen = macAddrLen(out->srcMode);

        if ((uint32_t)off + addrLen > (uint32_t)len)
            return 0;

        if (out->srcMode == MAC_ADDR_MODE_SHORT)
            out->srcShort = macReadU16(&psdu[off]);
        else
        {
            uint8_t k;

            for (k = 0; k < MAC_ADDR_LEN_EXT; ++k)
                out->srcExt[k] = psdu[off + k];
        }

        off = (uint16_t)(off + addrLen);
    }

    /* PAN ID compression: the source PAN is the destination PAN (7.2.1.1). */
    if (out->hasSrcPan == 0u && out->srcMode != MAC_ADDR_MODE_NONE)
        out->srcPan = out->dstPan;

    out->headerLen = (uint8_t)off;
    return 1;
}

/* Rules 3 and 4 of the third level filter: the destination PAN and address. Rule 4's
 * broadcast acceptance is what lets a broadcast through. */
static int macDstPanOk(const Mac_Hdr *hdr, const Mac_FilterCfg *cfg)
{
    if (!hdr->hasDstPan)
        return 1;

    return (hdr->dstPan == cfg->panId || hdr->dstPan == MAC_PAN_BROADCAST) ? 1 : 0;
}

static int macDstAddrOk(const Mac_Hdr *hdr, const Mac_FilterCfg *cfg)
{
    uint8_t k;

    if (hdr->dstMode == MAC_ADDR_MODE_SHORT)
        return (hdr->dstShort == cfg->shortAddr || hdr->dstShort == MAC_SHORT_BROADCAST) ? 1 : 0;

    if (hdr->dstMode == MAC_ADDR_MODE_EXT)
    {
        for (k = 0; k < MAC_ADDR_LEN_EXT; ++k)
        {
            if (hdr->dstExt[k] != cfg->extAddr[k])
                return 0;
        }

        return 1;
    }

    return 1; /* no destination address: rule 8 rejects the frame, not this check */
}

int Mac_Filter(const Mac_Hdr *hdr, const Mac_FilterCfg *cfg)
{
    if (hdr == 0 || cfg == 0)
        return 0;

    if (cfg->mode != MAC_FILTER_OFF)
        return 1;

    /* Rule 1 and 7: a reserved type and an acknowledgment are not delivered. */
    if (hdr->type > MAC_TYPE_COMMAND || hdr->type == MAC_TYPE_ACK)
        return 0;

    /* Rule 8: at least one address field. */
    if (hdr->dstMode == MAC_ADDR_MODE_NONE && hdr->srcMode == MAC_ADDR_MODE_NONE)
        return 0;

    if (!macDstPanOk(hdr, cfg) || !macDstAddrOk(hdr, cfg))
        return 0;

    /* Rule 5: a beacon's source PAN must match, unless the PAN is unconfigured. */
    if (hdr->type == MAC_TYPE_BEACON && cfg->panId != MAC_PAN_BROADCAST)
    {
        if (hdr->srcMode != MAC_ADDR_MODE_NONE && hdr->srcPan != cfg->panId)
            return 0;
    }

    /* Rule 6: source addressing only is accepted for a PAN coordinator. */
    if (hdr->dstMode == MAC_ADDR_MODE_NONE && hdr->srcMode != MAC_ADDR_MODE_NONE)
    {
        if (hdr->type == MAC_TYPE_DATA || hdr->type == MAC_TYPE_COMMAND)
            return 0; /* AACK_I_AM_COORD is clear: not a coordinator */
    }

    return 1;
}

int Mac_IsDataRequest(const Mac_Hdr *hdr, const uint8_t *psdu, uint16_t len)
{
    if (hdr == 0 || psdu == 0)
        return 0;

    if (hdr->type != MAC_TYPE_COMMAND || hdr->security)
        return 0;

    if (hdr->headerLen == 0u || (uint16_t)(hdr->headerLen + 1u) > len)
        return 0;

    return psdu[hdr->headerLen] == MAC_CMD_DATA_REQUEST ? 1 : 0;
}

/* Rules 3 and 4 for the Ack decision, without the broadcast acceptance: a broadcast is never
 * acknowledged (IEEE 802.15.4-2006 7.5.6.2, and the transceiver's own AR handling). */
static int macDstIsOurs(const Mac_Hdr *hdr, const Mac_FilterCfg *cfg)
{
    uint8_t k;

    if (hdr->dstMode == MAC_ADDR_MODE_SHORT)
        return hdr->dstShort == cfg->shortAddr ? 1 : 0;

    if (hdr->dstMode == MAC_ADDR_MODE_EXT)
    {
        for (k = 0; k < MAC_ADDR_LEN_EXT; ++k)
        {
            if (hdr->dstExt[k] != cfg->extAddr[k])
                return 0;
        }

        return 1;
    }

    return 0;
}

int Mac_AckDecide(const Mac_Hdr *hdr,
                  const uint8_t *psdu,
                  uint16_t len,
                  int crcOk,
                  const Mac_AckCfg *cfg,
                  const SrcMatch_Table *table,
                  int *framePending)
{
    int fp = 0;

    if (framePending != 0)
        *framePending = 0;

    if (hdr == 0 || cfg == 0 || table == 0 || !crcOk)
        return 0;

    /* Only the filtered mode acknowledges: a promiscuous receiver "shall not be
     * acknowledged, even if requested" (at86rf233.txt 7.2.3.2). */
    if (cfg->filter.mode != MAC_FILTER_OFF)
        return 0;

    if (hdr->type != MAC_TYPE_DATA && hdr->type != MAC_TYPE_COMMAND)
        return 0;

    if (!hdr->ackRequest || !macDstIsOurs(hdr, &cfg->filter))
        return 0;

    /* A secured command is treated as a Data Request: its identifier sits behind the auxiliary
     * security header, encrypted in a version 2 frame, and every secured command a Thread node sends
     * with an Ack request is a poll. So the table answers, as it does for a plain poll, and only a
     * source the table cannot be asked about claims pending. */
    if ((hdr->type == MAC_TYPE_COMMAND && hdr->security) || Mac_IsDataRequest(hdr, psdu, len))
    {
        if (!table->enabled)
        {
            /* The standard's fallback: a coordinator that cannot know claims pending
             * (IEEE 802.15.4-2024 10.22.3). */
            fp = 1;
        }
        else if (hdr->srcMode == MAC_ADDR_MODE_SHORT)
            fp = SrcMatch_HasShort(table, hdr->srcShort);
        else if (hdr->srcMode == MAC_ADDR_MODE_EXT)
            fp = SrcMatch_HasExt(table, hdr->srcExt);
        else
            fp = hdr->security ? 1 : 0;
    }

    if (framePending != 0)
        *framePending = fp;

    return 1;
}

void Mac_BuildImmAck(uint8_t seq, int framePending, uint8_t out[MAC_ACK_LEN])
{
    if (out == 0)
        return;

    out[0] = MAC_ACK_PHR; /* PHR counts the MPDU: FCF, DSN and the FCS the radio appends */
    out[1] = (uint8_t)(MAC_ACK_FCF_TYPE | (framePending ? MAC_ACK_FCF_PENDING : 0u));
    out[2] = 0x00u; /* frame version 0, no addressing, per IEEE 802.15.4-2006 7.2.2.3.1 */
    out[3] = seq;
}

int Mac_AckRequested(const uint8_t *psdu, uint16_t len)
{
    if (psdu == 0 || len < 1u)
        return 0;

    return (psdu[0] & MAC_FCF_ACK_REQUEST) ? 1 : 0;
}

int Mac_DstPan(const uint8_t *psdu, uint16_t len, uint16_t *pan)
{
    Mac_Hdr hdr;
    uint16_t fcf;
    uint16_t off = MAC_FCF_AND_SEQ;
    int dstPan;
    int srcPan;

    if (psdu == 0 || pan == 0 || len < 2u)
        return 0;

    fcf = macReadU16(psdu);
    hdr.version = (uint8_t)((fcf & MAC_FCF_VERSION_MASK) >> MAC_FCF_VERSION_SHIFT);
    hdr.dstMode = (uint8_t)((fcf & MAC_FCF_DST_MODE_MASK) >> MAC_FCF_DST_MODE_SHIFT);
    hdr.srcMode = (uint8_t)((fcf & MAC_FCF_SRC_MODE_MASK) >> MAC_FCF_SRC_MODE_SHIFT);
    hdr.panCompression = (fcf & MAC_FCF_PAN_COMPRESSION) ? 1u : 0u;

    if (hdr.dstMode == 1u || hdr.srcMode == 1u)
        return 0;

    macPanFields(&hdr, &dstPan, &srcPan);

    if (hdr.version == MAC_VERSION_2015 && (fcf & MAC_FCF_SEQ_SUPPRESSED) != 0u)
        off = 2u;

    if (!dstPan || (uint32_t)off + MAC_PAN_FIELD > (uint32_t)len)
        return 0;

    *pan = macReadU16(&psdu[off]);
    return 1;
}
