/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* Descriptor content of our device (see usb/usbd.h for the naming exception). */
#ifndef USB_DESC_H
#define USB_DESC_H

#include <stdint.h>

#define USB_DESC_STRING_MAX_CHARS 40u
#define USB_DESC_STRING_MAX (2u + 2u * USB_DESC_STRING_MAX_CHARS)

#define USB_DESC_DEVICE_BYTES 18u
#define USB_DESC_CONFIG_BYTES 67u
#define USB_DESC_CONFIG_TOTAL 67u
#define USB_DESC_BOS_BYTES 12u
#define USB_DESC_STR0_BYTES 4u

/* bmAttributes index; GET_STATUS device recipient mirrors its self-power bit
 * (USB 2.0 9.4.5). */
#define USB_DESC_CONFIG_ATTR_OFFSET 7u

enum
{
    USB_STRING_LANG = 0,
    USB_STRING_MANUF = 1,
    USB_STRING_PRODUCT = 2,
    USB_STRING_SERIAL = 3,
    USB_STRING_COUNT = 4
};

extern const uint8_t usb_desc_device[USB_DESC_DEVICE_BYTES];
extern const uint8_t usb_desc_config[USB_DESC_CONFIG_BYTES];
extern const uint8_t usb_desc_bos[USB_DESC_BOS_BYTES];
extern const uint8_t usb_desc_str0[USB_DESC_STR0_BYTES];
extern uint8_t usb_desc_str1[USB_DESC_STRING_MAX];
extern uint8_t usb_desc_str2[USB_DESC_STRING_MAX];
extern uint16_t usb_desc_str1_len; /* built byte count; 0 before UsbDesc_Init */
extern uint16_t usb_desc_str2_len;

/* Must run before hal_usb_attach() can let a host ask. */
void UsbDesc_Init(void);

/* The built serial-number descriptor, or 0 when no identity is available. */
const uint8_t *UsbDesc_Serial(uint16_t *len);

#endif
