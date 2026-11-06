/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
#include <stdint.h>

#include "usb_desc.h"
#include "hal.h"
#include "dbg.h"
#include "product.h"
#include "u_bstream.h"
#include "u_sstream.h"

#define EP0_SIZE 64u
#define EP_MPS 64u
#define EP_DATA_IN_ADDR 0x81u
#define EP_DATA_OUT_ADDR 0x02u
#define EP_NOTIF_ADDR 0x83u

#define STRING_LEN(s) (sizeof(s "") - 1u)

typedef char usbDescTableSizeIsDbgStringMax[USB_DESC_STRING_MAX_CHARS == DBG_STRING_MAX ? 1 : -1];
typedef char usbDescProductFitsDbgStringMax[STRING_LEN(PRODUCT_NAME) <= DBG_STRING_MAX ? 1 : -1];
typedef char usbDescManufFitsDbgStringMax[STRING_LEN(PRODUCT_MANUF) <= DBG_STRING_MAX ? 1 : -1];
typedef char
    usbDescConfigTotalMatchesTable[USB_DESC_CONFIG_TOTAL == USB_DESC_CONFIG_BYTES ? 1 : -1];

const uint8_t usb_desc_device[USB_DESC_DEVICE_BYTES] = {
    0x12,
    0x01,
    (uint8_t)PRODUCT_BCD_USB,
    (uint16_t)PRODUCT_BCD_USB >> 8,
    0x02,
    0x00,
    0x00, /* CDC, no subclass/protocol */
    EP0_SIZE,
    /* USB 2.0 ch. 9.6.1: idVendor at [8:9], idProduct at [10:11], little endian. */
    (uint8_t)PRODUCT_VID,
    (uint16_t)PRODUCT_VID >> 8,
    (uint8_t)PRODUCT_PID,
    (uint16_t)PRODUCT_PID >> 8,
    (uint8_t)PRODUCT_BCD_DEV,
    (uint16_t)PRODUCT_BCD_DEV >> 8,
    1,
    2,
    3, /* iManuf, iProduct, iSerial */
    1};

/* bmAttributes 0x80 = bus-powered; bMaxPower 0x32 = 100 mA. */
const uint8_t usb_desc_config[USB_DESC_CONFIG_BYTES] = {
    0x09,
    0x02,
    (uint8_t)USB_DESC_CONFIG_TOTAL,
    (uint8_t)(USB_DESC_CONFIG_TOTAL >> 8),
    2,
    0x01,
    0x00,
    0x80,
    0x32,
    0x09,
    0x04,
    0x00,
    0x00,
    1,
    0x02,
    0x02,
    0x01,
    0x00,
    0x05,
    0x24,
    0x00,
    0x10,
    0x01,
    0x04,
    0x24,
    0x02,
    0x02,
    /* CDC 1.10 5.2.3.8 Union FD (control if 0 -> data if 1); macOS needs it to
   * pair the interfaces, and 5.2.3.2 Call Management FD. */
    0x05,
    0x24,
    0x06,
    0x00,
    0x01,
    0x05,
    0x24,
    0x01,
    0x03,
    0x01,
    0x07,
    0x05,
    EP_NOTIF_ADDR,
    0x03,
    EP_MPS,
    0x00,
    0x10,
    0x09,
    0x04,
    0x01,
    0x00,
    2,
    0x0A,
    0x00,
    0x00,
    0x00,
    0x07,
    0x05,
    EP_DATA_OUT_ADDR,
    0x02,
    EP_MPS,
    0x00,
    0x00,
    0x07,
    0x05,
    EP_DATA_IN_ADDR,
    0x02,
    EP_MPS,
    0x00,
    0x00};

/* USB 2.0 Extension: no LPM (CTRLB.LPMHDSK is never set); bit 0 reserved. */
const uint8_t usb_desc_bos[USB_DESC_BOS_BYTES] = {
    0x05, 0x0F, 0x0C, 0x00, 0x01, 0x07, 0x10, 0x02, 0x00, 0x00, 0x00, 0x00};

const uint8_t usb_desc_str0[USB_DESC_STR0_BYTES] = {
    0x04, 0x03, 0x09, 0x04 /* bLength, STRING, LANGID 0x0409 = en-US */
};

uint8_t usb_desc_str1[USB_DESC_STRING_MAX];
uint8_t usb_desc_str2[USB_DESC_STRING_MAX];
uint16_t usb_desc_str1_len;
uint16_t usb_desc_str2_len;

#define SERIAL_BYTES_MAX 12u
#define UID_FALLBACK_BYTES 6u

static char serialText[SERIAL_BYTES_MAX + 1u];
static uint8_t serialRaw[SERIAL_BYTES_MAX];
static uint8_t serialDesc[USB_DESC_STRING_MAX];
static uint16_t serialDescLen;

/* bLength + STRING type + UTF-16LE payload. */
static void descPutString(uint8_t *dst, uint16_t *len, const char *src, uint8_t chars)
{
    U_BStream bs;
    uint8_t i;

    U_bstream_init(&bs, dst, 2u + 2u * (unsigned long)chars);
    U_bstream_put_u8(&bs, (unsigned char)(2u + 2u * chars));
    U_bstream_put_u8(&bs, 0x03u);

    for (i = 0; i < chars; ++i)
        U_bstream_put_u16_le(&bs, (unsigned short)(uint8_t)src[i]);

    if (len)
        *len = (uint16_t)(2u + 2u * chars);
}

void UsbDesc_Init(void)
{
    U_SStream ss;
    uint8_t chars;
    const char *serial;

    descPutString(
        usb_desc_str1, &usb_desc_str1_len, PRODUCT_MANUF, (uint8_t)STRING_LEN(PRODUCT_MANUF));
    descPutString(
        usb_desc_str2, &usb_desc_str2_len, PRODUCT_NAME, (uint8_t)STRING_LEN(PRODUCT_NAME));

    /* U_sstream_put_str would compile to strlen (coding-rules §1). */
    U_sstream_init(&ss, serialText, sizeof(serialText));
    chars = Nvram_SerialAscii(serialRaw, sizeof(serialRaw) - 1u);

    if (chars != 0u)
        serial = (const char *)serialRaw;
    else
    {
        uint8_t uid[12];

        Nvram_Uid(uid);
        U_sstream_put_hex(&ss, uid, UID_FALLBACK_BYTES);

        if (U_sstream_at_end(&ss))
        {
            serialDescLen = 0u;
            return;
        }

        serial = U_sstream_str(&ss);
        chars = (uint8_t)U_sstream_pos(&ss);
    }

    if (chars == 0u)
    {
        serialDescLen = 0u;
        return;
    }

    descPutString(serialDesc, &serialDescLen, serial, chars);
}

const uint8_t *UsbDesc_Serial(uint16_t *len)
{
    if (serialDescLen == 0u)
        return 0;

    *len = serialDescLen;
    return serialDesc;
}
