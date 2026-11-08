/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* USB device control driver — public API.
 * Adopted from deconz-cb2-fw with its symbols kept verbatim: a deliberate
 * naming exception to coding-rules §2 so future syncs stay a plain diff. */
#ifndef USBD_H
#define USBD_H

#include <stdint.h>

#include "usbd_hal.h"

/* Public driver API - frozen since P6; changes require a plan amendment. */

#define USBD_EP0_MPS 64u
#define USBD_BULK_MPS 64u

typedef enum
{
    USBD_ST_DEFAULT = 0,
    USBD_ST_ADDRESSED,
    USBD_ST_CONFIGURED
} usbd_state_t;

typedef void (*usbd_bulk_cb_t)(uint8_t ep, uint16_t len, void *arg);
typedef int (*usbd_class_cb_t)(const uint8_t *setup8, void *arg);
typedef void (*usbd_config_cb_t)(uint8_t cfg_value, void *arg);

typedef struct
{
    uint8_t *ep0_rx; /* 64 bytes, SETUP + data-out buffer */
    uint8_t *ep0_tx; /* 64 bytes, data-in buffer; never a bulk endpoint's */
    uint8_t *ep1_rx;
    uint8_t *ep2_rx;
    uint8_t *ep1_tx;
    uint8_t *ep2_tx;
    uint8_t *ep3_tx;
    uint8_t *ep3_rx;
    const uint8_t *serial_u16;
    uint8_t serial_chars;
    usbd_class_cb_t class_request;
    void *class_arg;
    /* fired in the SET_CONFIGURATION commit context (value 0 or 1) */
    usbd_config_cb_t on_config;
    void *on_config_arg;
} usbd_cfg_t;

int usbd_init(const usbd_cfg_t *cfg);
int usbd_reinit(void);
void usbd_irq(void);

usbd_state_t usbd_state(void);
uint8_t usbd_addr(void);
int usbd_suspended(void);
uint32_t usbd_recoveries(void);

void usbd_bulk_in_cb(uint8_t ep, usbd_bulk_cb_t cb, void *arg);
int usbd_bulk_in(uint8_t ep, const void *data, uint16_t len);
void usbd_bulk_out_cb(uint8_t ep, usbd_bulk_cb_t cb, void *arg);
void usbd_bulk_out_rearm(uint8_t ep);

#endif
