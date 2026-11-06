/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* ConBee II board descriptor (docs/boards.md §2). The instance behind
 * Board_Get(); included only by the board's glue (targets/cb2/board.c).
 * Identity comes from product.h, the NVRAM and flash numbers from the layout
 * the ConBee II hardware and its GCF bootloader fix. The NVRAM geometry is
 * also still held as compile-time constants in hal/samr21/samr21_nvram.c,
 * where the build-time row asserts need them; unifying the two is a later
 * step. */
#ifndef CB2_BOARD_H
#define CB2_BOARD_H

#include "../board/board.h"
#include "com.h"
#include "product.h"

/* The default origin when the build did not set one (src/CMakeLists.txt
 * APP_ORIGIN): the real layout, the GCF bootloader occupying 0x0..0x4FFF. */
#ifndef APP_FLASH_ORIGIN
#define APP_FLASH_ORIGIN 0x00005000u
#endif

/* deCONZ NVRAM: the last 4 KB of flash, which every flash must leave intact,
 * so the app image must end below it. The NVM user row carries the factory
 * EUI-64 and serial; 0x4FFF, the last byte of the row at 0x4F00, is the
 * bootloader's update flag. */
#define BOARD_NVRAM_BASE 0x0003EF00u
#define BOARD_NVRAM_SIZE 0x00001000u
#define BOARD_NVM_USER_ROW 0x00804000u
#define BOARD_NVRAM_UPDATE_FLAG 0x00004FFFu

static const Board_Descriptor boardCb2 = {
    PRODUCT_NAME,
    PRODUCT_NCP_VERSION,
    PRODUCT_VID,
    PRODUCT_PID,
    BOARD_LINK_USB_CDC,
    COM_TX_MAX,
    0u, /* CDC has no line rate of its own: its rate is the bus's */
    BOARD_RADIO_AT86RF233,
    -96, /* the AT86RF233's 802.15.4 receive sensitivity (docs/protocol.md §10) */
    -3,  /* the stock firmware's ConBee II transmit power, before the front end's PA */
    -12, /* and on channel 26 */
    (1u << 16) | (1u << 17), /* PA16 and PA17 switch the front end on (docs/hardware_samr21.md §6) */
    BOARD_NVRAM_BASE,
    BOARD_NVRAM_SIZE,
    BOARD_NVM_USER_ROW,
    BOARD_NVRAM_UPDATE_FLAG,
    APP_FLASH_ORIGIN,
    BOARD_NVRAM_BASE /* the guard bound: the image ends below the NVRAM */,
};

#endif
