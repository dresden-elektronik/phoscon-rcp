/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* Seeed Studio XIAO ESP32-C6 board descriptor (docs/hardware_esp32.md §12.4). The instance behind Board_Get();
 * included only by the board's glue (targets/xiaoc6/board.c).

The board keeps its ESP-IDF bootloader and partition table; the image is the app in the ota_0 slot, at
0x10000 and 2 MiB long, which is the bound. There is no deCONZ NVRAM, no user row and no update flag: the
identity is the chip's eFuse MAC, which nothing writes. The USB identifiers are the controller's own,
fixed in silicon. The radio is not driven yet, so its figures are 0. */
#ifndef XIAOC6_BOARD_H
#define XIAOC6_BOARD_H

#include "../board/board.h"
#include "com.h"
#include "product.h"

#ifndef APP_FLASH_ORIGIN
#define APP_FLASH_ORIGIN 0x00010000u
#endif

#define BOARD_IMAGE_BOUND 0x00210000u

static const Board_Descriptor boardXiaoC6 = {
    PRODUCT_NAME,
    PRODUCT_NCP_VERSION,
    0x303Au, /* Espressif's USB Serial/JTAG controller (TRM ch. 32) */
    0x1001u,
    BOARD_LINK_UART, /* through com/uart.c: the controller is a byte pipe behind hal/uart_drv.h */
    COM_TX_MAX,
    PRODUCT_LINK_BAUD,
    BOARD_RADIO_ESP32_ON_DIE,
    0, /* no radio driver yet */
    0,
    0,
    0u, /* the RF switch is GPIO-driven, not a SAMR21 front end (docs/hardware_esp32.md §12.4) */
    0u, /* no region outside the app slot is this image's to keep out of but the bootloader's, below it */
    0u,
    0u,
    0u,
    APP_FLASH_ORIGIN,
    BOARD_IMAGE_BOUND};

#endif /* XIAOC6_BOARD_H */
