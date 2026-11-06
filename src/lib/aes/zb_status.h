/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* zb_status.h — the three status values aes.h and aes_modes.c use. Their home is a larger header
 * in the library's source tree, so this lets the vendored files stay unmodified. */
#ifndef ZB_STATUS_H
#define ZB_STATUS_H

typedef enum
{
    ZB_OK = 0,
    ZB_ERR_BAD_ARGUMENT = 1,
    ZB_ERR_AUTH = 2
} ZbStatus;

#endif
