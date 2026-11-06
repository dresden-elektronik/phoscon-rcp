/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* USB device driver — internal contract between usbd_core / usbd_ep0 /
 * usbd_req / usbd_cdc. */
#ifndef USBD_INT_H
#define USBD_INT_H

#include "usbd.h"
#include "usbd_hal.h"

typedef enum
{
    EP0_IDLE = 0,
    EP0_DATA_IN,
    EP0_DATA_OUT,
    EP0_STATUS,
    EP0_STALLED
} ep0_state_t;

void usbd_ep0_init(int fresh);
void usbd_ep0_recover(void);
void usbd_ep0_reject(void);
void usbd_ep0_setup(const uint8_t *s8);
void usbd_ep0_tx(void);
void usbd_ep0_rx(void);
void usbd_ep0_begin_in(const uint8_t *data, uint16_t total, uint16_t hostLen);
void usbd_ep0_begin_out(uint8_t *dst, uint16_t total);
void usbd_ep0_zero_in(void);
void usbd_ep0_set_commit(void (*fn)(void));
uint8_t usbd_ep0_state(void);
uint8_t usbd_ep0_toggle(int in);

int usbd_req_dispatch(const uint8_t *s8);

void usbd_core_reset(void);
void usbd_core_commit_addr(uint8_t a);
void usbd_core_commit_cfg(uint8_t v);
const usbd_cfg_t *usbd_config(void);
uint8_t usbd_cfg_value(void);

#endif
