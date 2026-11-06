/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* props_debug.h - packing of debug records for the RCP log property
 * (docs/protocol.md §10). Pure: no HAL, no register access. */
#ifndef RCP_PROPS_DEBUG_H
#define RCP_PROPS_DEBUG_H

#include <stdint.h>

/* Value layout of PROPS_ID_RCP_LOG_CRASH_DUMP: a little-endian u16 record count, then that
 * many 8-byte records (src/hal/dbg.h, DBG_FRAME_SIZE). A batch is bounded by the Spinel value
 * budget, not by the escaped frame size, so this header restates that budget instead of
 * including props_stream.h (which pulls in the radio HAL): keep in step with
 * PROPS_STREAM_RX_MAX. */
#define PROPS_DEBUG_VALUE_MAX 160u
#define PROPS_DEBUG_HEADER 2u
#define PROPS_DEBUG_RECORD 8u
#define PROPS_DEBUG_MAX ((PROPS_DEBUG_VALUE_MAX - PROPS_DEBUG_HEADER) / PROPS_DEBUG_RECORD)

/* Pack `count` records into `dst`; returns the bytes written, or 0 if it does not fit. */
uint16_t PropsDebug_Pack(uint8_t *dst, uint16_t cap, const uint8_t *records, uint16_t count);

/* Decode the write that starts or stops the stream: a u8 payload sets *on, and an empty value, upstream's
 * request for a crash dump, leaves it as it was. Returns 1, or 0 if invalid. */
int PropsDebug_EnableValue(const uint8_t *value, uint16_t len, uint8_t *on);

#endif
