/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mac.h — the 802.15.4 header knowledge the RCP core needs (docs/hardware_samr21.md §6,
 * docs/ack-pending-frame.md). Pure functions over a PSDU and a caller-owned config, so the
 * receive interrupt can call them and the host suite can drive them (coding-rules.md §4).
 *
 * The filter here is the transceiver's third level (at86rf233.txt §8.2), reproduced in
 * software because the receive path now runs in RX_ON, where the chip does not filter. */
#ifndef RCP_MAC_H
#define RCP_MAC_H

#include <stdint.h>

#include "srcmatch.h"

/* Frame types (IEEE 802.15.4-2006 7.2.1.1). */
#define MAC_TYPE_BEACON 0u
#define MAC_TYPE_DATA 1u
#define MAC_TYPE_ACK 2u
#define MAC_TYPE_COMMAND 3u

/* Command frame identifiers: the Data Request a polling device sends (7.3.2). */
#define MAC_CMD_DATA_REQUEST 0x04u

#define MAC_ADDR_LEN_SHORT 2u
#define MAC_ADDR_LEN_EXT 8u

/* Addressing modes of the FCF subfields. */
#define MAC_ADDR_MODE_NONE 0u
#define MAC_ADDR_MODE_SHORT 2u
#define MAC_ADDR_MODE_EXT 3u

/* An immediate acknowledgment is a five-octet MPDU: FCF, DSN and the FCS. The driver writes
 * four octets - PHR, FCF and DSN - and the transceiver computes the FCS into the last two
 * MPDU positions itself (TX_AUTO_CRC_ON), so PHR is 5 while the buffer holds 4. */
#define MAC_ACK_LEN 4u
#define MAC_ACK_PHR 5u

/* Receive filter mode, the software counterpart of the transceiver's three configurations
 * (radio.h RADIO_RX_*). PAN and ALL accept every frame: the chip bypasses its third level
 * filter there too, and the app drops foreign-PAN frames in PAN mode (docs/architecture.md §3). */
enum
{
    MAC_FILTER_OFF = 0, /* the third level filter, as AACK with no promiscuous bit applies it */
    MAC_FILTER_PAN = 1, /* accept all; the app keeps the PAN rule */
    MAC_FILTER_ALL = 2  /* accept all */
};

typedef struct
{
    uint8_t type;
    uint8_t version;
    uint8_t security;
    uint8_t framePending;
    uint8_t ackRequest;
    uint8_t panCompression;
    uint8_t seq;

    uint8_t dstMode;
    uint8_t srcMode;
    uint8_t hasDstPan;
    uint8_t hasSrcPan;

    uint16_t dstPan;
    uint16_t srcPan;
    uint16_t dstShort;
    /* On-air order (LSB first), the order the radio compares and the order the source-match
     * table stores: the reversal from Spinel's MSB-first form happens once, in the HAL. */
    uint8_t dstExt[SRCMATCH_EXT_LEN];
    uint16_t srcShort;
    uint8_t srcExt[SRCMATCH_EXT_LEN];

    uint8_t headerLen; /* FCF through the last addressing octet; 0 when it cannot be known */
} Mac_Hdr;

typedef struct
{
    uint8_t mode; /* MAC_FILTER_* */
    uint16_t panId;
    uint16_t shortAddr;
    uint8_t extAddr[SRCMATCH_EXT_LEN]; /* on-air order */
} Mac_FilterCfg;

typedef struct
{
    Mac_FilterCfg filter;
    uint8_t isCoordinator; /* AACK_I_AM_COORD; 0 on this board today */
} Mac_AckCfg;

/* Header length from the two FCF octets alone: what the receive job needs to schedule its
 * read of the addressing field before that field has arrived. 0 when a security header makes
 * the length depend on octets further in. */
uint8_t Mac_HeaderLenFromFcf(uint16_t fcf);

/* Full parse. 0 on a truncated frame or a reserved addressing mode. A secured frame parses its
 * addressing fields and sets `security`; headerLen then excludes the auxiliary security header. A
 * version 2 frame follows its own PAN ID rules and may suppress its sequence number, which reads 0. */
int Mac_ParseHeader(const uint8_t *psdu, uint16_t len, Mac_Hdr *out);

/* The third level filter of at86rf233.txt §8.2, with the frame-version rule disabled the way
 * AACK_FVN_MODE = b11 disables it. */
int Mac_Filter(const Mac_Hdr *hdr, const Mac_FilterCfg *cfg);

/* A MAC command frame carrying the Data Request identifier. 0 for a secured frame: its command
 * identifier is behind a header this parser does not decode. */
int Mac_IsDataRequest(const Mac_Hdr *hdr, const uint8_t *psdu, uint16_t len);

/* The acknowledgment decision for a received frame, and the Frame Pending bit it must carry.
 * 1 = an Ack is due. A Data Request and a secured command frame (which is taken for one) get the
 * table's answer, and every other acknowledged frame gets 0, which is what the transceiver does
 * (at86rf233.txt 7.2.3). */
int Mac_AckDecide(const Mac_Hdr *hdr,
                  const uint8_t *psdu,
                  uint16_t len,
                  int crcOk,
                  const Mac_AckCfg *cfg,
                  const SrcMatch_Table *table,
                  int *framePending);

/* The four octets the driver writes into the Frame Buffer. */
void Mac_BuildImmAck(uint8_t seq, int framePending, uint8_t out[MAC_ACK_LEN]);

/* Destination PAN, when the frame carries one. */
int Mac_DstPan(const uint8_t *psdu, uint16_t len, uint16_t *pan);

/* Acknowledgment request: FCF octet 0 bit 5 (IEEE 802.15.4-2006 7.2.1.1; at86rf233.txt
 * 7.2.4, :5948). */
int Mac_AckRequested(const uint8_t *psdu, uint16_t len);

#endif
