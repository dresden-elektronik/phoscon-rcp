/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* hdlc.h — HDLC framing for the Spinel transport (docs/protocol.md §3). */
#ifndef RCP_HDLC_H
#define RCP_HDLC_H

#include <stdint.h>

#include "u_bstream.h"

#define HDLC_FEND 0x7Eu
#define HDLC_FESC 0x7Du
#define HDLC_ESC_XOR 0x20u
/* The overflow limit, in unescaped bytes: the receiver unescapes before storing, so this bounds
 * the payload, not the wire frame. It sits above the largest real frame - a 160-byte STREAM_RX
 * value plus its header and 2-byte FCS, about 165 bytes - so a legitimate frame never overflows.
 * 2 * (160 + 3) + 6 = 332. */
#define HDLC_FRAME_MAX (2u * (160u + 3u) + 6u)
#define HDLC_FCS_LEN 2u
#define HDLC_FCS_GOOD 0xF0B8u

typedef enum
{
    HDLC_RX_NONE = 0,
    HDLC_RX_FRAME,
    HDLC_RX_ERROR
} Hdlc_RxResult;

typedef struct
{
    uint8_t buf[HDLC_FRAME_MAX];
    uint16_t len;
    uint16_t crc;
    int escaped;
    int overflow;
} Hdlc_Rx;

void Hdlc_RxInit(Hdlc_Rx *rx);

Hdlc_RxResult Hdlc_RxByte(Hdlc_Rx *rx, uint8_t byte);

uint16_t Hdlc_Crc(const uint8_t *data, uint16_t len);

uint16_t Hdlc_Encode(const uint8_t *payload, uint16_t len, U_BStream *dst);

#endif
