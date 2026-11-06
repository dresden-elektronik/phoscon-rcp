/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
#include <stdint.h>

#include "usbd_int.h"
#include "usb_desc.h"

static uint8_t status2[2];
static uint8_t one_byte;

enum
{
    C_NONE = 0,
    C_ADDR,
    C_CFG
};
static uint8_t s_commit;
static uint8_t s_arg;

static void req_commit(void)
{
    switch (s_commit)
    {
        case C_ADDR:
            usbd_core_commit_addr(s_arg);
            break;
        case C_CFG:
            usbd_core_commit_cfg(s_arg);
            break;
        default:
            break;
    }

    s_commit = C_NONE;
}

/* Clamp to the descriptor length and wLength; ep0 owns ZLP and chunking
 * (USB 2.0 5.3.2). */
static uint16_t req_clamp(uint16_t wl, uint16_t total)
{
    return (wl < total) ? wl : total;
}

static int get_desc(const uint8_t *s8)
{
    uint16_t wv = (uint16_t)s8[2] | ((uint16_t)s8[3] << 8);
    uint16_t wl = (uint16_t)s8[6] | ((uint16_t)s8[7] << 8);
    const uint8_t *d;
    uint16_t len;

    switch (wv >> 8)
    {
        case 1:
            d = usb_desc_device;
            len = USB_DESC_DEVICE_BYTES;
            break;
        case 2:
            if (wv & 0xff)
                return 0;

            d = usb_desc_config;
            len = USB_DESC_CONFIG_BYTES;
            break;
        case 0x0F:
            if ((wv & 0xff) != 0)
                return 0;

            d = usb_desc_bos;
            len = USB_DESC_BOS_BYTES;
            break;
        case 3:
            /* string index is wValue's low byte; wIndex is the LANGID (accepted) */
            switch (wv & 0xff)
            {
                case USB_STRING_LANG:
                    d = usb_desc_str0;
                    len = USB_DESC_STR0_BYTES;
                    break;
                case USB_STRING_MANUF:
                    d = usb_desc_str1;
                    len = usb_desc_str1_len;

                    if (len == 0)
                        return 0;

                    break;
                case USB_STRING_PRODUCT:
                    d = usb_desc_str2;
                    len = usb_desc_str2_len;

                    if (len == 0)
                        return 0;

                    break;
                case USB_STRING_SERIAL:
                    d = UsbDesc_Serial(&len);

                    if (!d)
                        return 0;

                    break;
                default:
                    return 0;
            }

            break;
        default:
            return 0;
    }

    if (wl == 0)
        return 0;

    usbd_ep0_begin_in(d, req_clamp(wl, len), wl);
    return 1;
}

static int get_status(uint8_t recip, uint8_t ea)
{
    status2[0] = 0;
    status2[1] = 0;

    if (recip == 0)
    {
        /* bit 0: Self Powered (USB 2.0 9.4.5), mirroring bmAttributes bit 6 */
        status2[0] = (usb_desc_config[USB_DESC_CONFIG_ATTR_OFFSET] & 0x40) ? 1 : 0;
    }
    else if (recip == 1)
    {
        if (ea > 1)
            return 0;
    }
    else if (recip == 2)
    {
        if ((ea & 0x0f) > USBD_MAX_EP)
            return 0;

        status2[0] = hal_ep_stalled(ea & 0x0f, ea & 0x80) ? 1 : 0;
    }

    usbd_ep0_begin_in(status2, 2, 2);
    return 1;
}

static int set_address(const uint8_t *s8)
{
    uint8_t a = s8[2];
    /* USB 2.0 9.4.6 allows SetAddress in Default and Addressed;
     * SET_ADDRESS(0) is the way back to Default. */
    if (usbd_state() == USBD_ST_CONFIGURED || a > 127)
        return 0;

    s_commit = C_ADDR;
    s_arg = a;
    usbd_ep0_set_commit(req_commit);
    return 1;
}

static int set_configuration(const uint8_t *s8)
{
    uint8_t v = s8[2];

    if (usbd_state() == USBD_ST_DEFAULT || v > 1)
        return 0;

    s_commit = C_CFG;
    s_arg = v;
    usbd_ep0_set_commit(req_commit);
    return 1;
}

static int endpoint_halt(const uint8_t *s8, int clear)
{
    uint8_t ea = s8[4];

    if ((ea & 0x0f) > USBD_MAX_EP)
        return 0;

    if ((ea & 0x0f) == 0)
        return 1;

    if (clear)
        hal_ep_clear_stall(ea & 0x0f, ea & 0x80);
    else
        hal_ep_stall(ea & 0x0f, ea & 0x80);

    return 1;
}

int usbd_req_dispatch(const uint8_t *s8)
{
    uint8_t bm = s8[0];
    uint8_t rq = s8[1];
    uint8_t recip = bm & 0x1f;
    const usbd_cfg_t *cfg = usbd_config();

    if ((bm & 0x60) != 0x00)
    {
        if (cfg && cfg->class_request)
        {
            return cfg->class_request(s8, cfg->class_arg);
        }

        return 0;
    }

    if (bm & 0x80)
    {
        switch (rq)
        {
            case 0x00:
                if (recip > 2)
                    return 0;

                return get_status(recip, s8[4]);
            case 0x06:
                if (recip != 0)
                    return 0;

                return get_desc(s8);
            case 0x08:
                if (recip != 0)
                    return 0;

                one_byte = usbd_cfg_value();
                usbd_ep0_begin_in(&one_byte,
                                  req_clamp((uint16_t)s8[6] | ((uint16_t)s8[7] << 8), 1),
                                  (uint16_t)s8[6] | ((uint16_t)s8[7] << 8));
                return 1;
            case 0x0A:
                /* GET_INTERFACE (USB 2.0 9.4.8): no alternate settings, answer 0. */
                if (recip != 1)
                    return 0;

                one_byte = 0;
                usbd_ep0_begin_in(&one_byte,
                                  req_clamp((uint16_t)s8[6] | ((uint16_t)s8[7] << 8), 1),
                                  (uint16_t)s8[6] | ((uint16_t)s8[7] << 8));
                return 1;
            case 0x01:
                if (recip != 0)
                    return 0;

                usbd_ep0_zero_in();
                return 1;
            default:
                return 0;
        }
    }

    switch (rq)
    {
        case 0x05:
            if (recip != 0)
                return 0;

            return set_address(s8);
        case 0x09:
            if (recip != 0)
                return 0;

            return set_configuration(s8);
        case 0x03:
            if (recip == 0)
                return 1;

            if (recip != 2)
                return 0;

            return endpoint_halt(s8, 0);
        case 0x01:
            if (recip != 2)
                return 0;

            return endpoint_halt(s8, 1);
        default:
            return 0;
    }
}
