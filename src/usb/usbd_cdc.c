/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
#include <stdint.h>

#include "usbd_cdc.h"
#include "usbd_int.h"
#include "u_bstream.h"

/* CDC120 Table 41/42/43 class requests on the communication interface. */
enum
{
    CDC_REQ_GET_LINE_CODING = 0x21,
    CDC_REQ_SET_LINE_CODING = 0x20,
    CDC_REQ_SET_CONTROL_LINE = 0x22,
    CDC_REQ_SEND_BREAK = 0x23
};

/* CDC120 Table 43: DTR bit 0, RTS bit 1. */
#define CDC_CTRL_DTR (1u << 0)
#define CDC_CTRL_RTS (1u << 1)

/* SET_LINE_CODING arms its payload in place; only the commit marks it
 * negotiated, so an aborted transfer cannot count as agreed. */
static uint8_t lineCoding[CDC_LINE_CODING_BYTES];
static uint8_t lineCodingValid;
static uint8_t ctrlDtr;
static uint8_t ctrlRts;
static uint8_t notifQueued;
static uint8_t notifType;
static uint16_t notifValue;

static void cdcCommitLineCoding(void)
{
    lineCodingValid = 1;
}

int usbd_cdc_class_request(const uint8_t *s8, void *arg)
{
    uint8_t bm = s8[0];
    uint8_t wIndex = s8[4];
    uint16_t wValue = (uint16_t)s8[2] | ((uint16_t)s8[3] << 8);
    uint16_t wl = (uint16_t)s8[6] | ((uint16_t)s8[7] << 8);

    (void)arg;

    if ((bm & 0x60) != 0x20 || wIndex != CDC_COMM_INTERFACE)
        return 0;

    if (bm & 0x80)
    {
        switch (s8[1])
        {
            case CDC_REQ_GET_LINE_CODING:
                if (wl != CDC_LINE_CODING_BYTES)
                    return 0;

                usbd_ep0_begin_in(lineCoding, CDC_LINE_CODING_BYTES, wl);
                return 1;
            default:
                return 0;
        }
    }

    switch (s8[1])
    {
        case CDC_REQ_SET_LINE_CODING:
            if (wl != CDC_LINE_CODING_BYTES)
                return 0;

            usbd_ep0_set_commit(cdcCommitLineCoding);
            usbd_ep0_begin_out(lineCoding, CDC_LINE_CODING_BYTES);
            return 1;
        case CDC_REQ_SET_CONTROL_LINE:
            if (wl != 0)
                return 0;

            ctrlDtr = (wValue & CDC_CTRL_DTR) ? 1u : 0u;
            ctrlRts = (wValue & CDC_CTRL_RTS) ? 1u : 0u;
            return 1;
        default:
            /* Unimplemented CDC-ACM request: the driver stalls EP0 IN (CDC120 §4.4). */
            return 0;
    }
}

int usbd_cdc_notify_queue(uint8_t type, uint16_t value)
{
    notifType = type;
    notifValue = value;
    notifQueued = 1;
    return 1;
}

int usbd_cdc_notify_pending(void)
{
    return notifQueued;
}

uint16_t usbd_cdc_notify_build(uint8_t *dst, uint16_t max)
{
    U_BStream bs;

    if (!notifQueued || max < CDC_NOTIF_NET_SIZE)
        return 0;
    /* CDC120 Table 22: the header is shaped like a SETUP packet; bmRequestType
   * 0x01 (class, interface recipient) and wIndex is the communication interface. */
    U_bstream_init(&bs, dst, max);
    U_bstream_put_u8(&bs, 0x01u);
    U_bstream_put_u8(&bs, notifType);
    U_bstream_put_u16_le(&bs, notifValue);
    U_bstream_put_u16_le(&bs, CDC_COMM_INTERFACE);
    U_bstream_put_u16_le(&bs, CDC_NOTIF_NET_BYTES);
    U_bstream_put_u16_le(&bs, notifValue);
    notifQueued = 0;
    return CDC_NOTIF_NET_SIZE;
}

void usbd_cdc_on_config(uint8_t cfg_value, void *arg)
{
    (void)arg;

    /* CDC120 §6.2.11: queued in the commit context, after the endpoints exist. */
    usbd_cdc_notify_queue(CDC_NOTIF_NETWORK_CONNECTION,
                          cfg_value ? CDC_NOTIF_NET_CONNECTED : CDC_NOTIF_NET_DISCONNECTED);
}

void usbd_cdc_reset(void)
{
    notifQueued = 0;
}

const uint8_t *usbd_cdc_line_coding(void)
{
    return lineCoding;
}

int usbd_cdc_line_coding_valid(void)
{
    return lineCodingValid != 0u;
}

int usbd_cdc_dtr(void)
{
    return ctrlDtr != 0u;
}

int usbd_cdc_rts(void)
{
    return ctrlRts != 0u;
}
