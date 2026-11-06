/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* CDC ACM class adapter for the usbd driver (CDC120). */
#ifndef USBD_CDC_H
#define USBD_CDC_H

#include <stdint.h>

#include "usbd.h"

/* CDC120 Table 20: 8-byte SETUP-shaped header; NETWORK_CONNECTION payload 2
 * bytes (0 = disconnected, 1 = connected). */
#define CDC_NOTIF_NETWORK_CONNECTION 0x00u
#define CDC_NOTIF_NET_DISCONNECTED 0x0000u
#define CDC_NOTIF_NET_CONNECTED 0x0001u
#define CDC_NOTIF_HEADER_BYTES 8u
#define CDC_NOTIF_NET_BYTES 2u
#define CDC_NOTIF_NET_SIZE (CDC_NOTIF_HEADER_BYTES + CDC_NOTIF_NET_BYTES)

/* SET_LINE_CODING / GET_LINE_CODING payload (CDC120 Table 41): baud u32 LE,
 * stopbits u8, parity u8, databits u8. */
#define CDC_LINE_CODING_BYTES 7u

/* Communication interface the class requests and notify pipe belong to. */
#define CDC_COMM_INTERFACE 0u

/* Class-request entry, installed as usbd_cfg_t.class_request. 1 = answered,
 * 0 = the driver stalls EP0 IN (CDC120 §4.4). */
int usbd_cdc_class_request(const uint8_t *s8, void *arg);

/* SET_CONFIGURATION commit hook, installed as usbd_cfg_t.on_config. */
void usbd_cdc_on_config(uint8_t cfg_value, void *arg);

/* Reset the class state (bus reset / re-init). */
void usbd_cdc_reset(void);

/* Queue a notification; newest state wins, never a queue of history. 1 = queued. */
int usbd_cdc_notify_queue(uint8_t type, uint16_t value);

int usbd_cdc_notify_pending(void);

/* Build the queued notification into dst (CDC_NOTIF_NET_SIZE bytes) and
 * consume it; returns the byte count, 0 when nothing was queued or dst is
 * too small. */
uint16_t usbd_cdc_notify_build(uint8_t *dst, uint16_t max);

/* What the host last told us (SET_LINE_CODING / SET_CONTROL_LINE_STATE): the
 * 7-byte payload as received, zeroed until the host sends one; dtr/rts are 0/1. */
const uint8_t *usbd_cdc_line_coding(void);
int usbd_cdc_dtr(void);
int usbd_cdc_rts(void);

#endif
