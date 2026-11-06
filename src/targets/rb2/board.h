/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* RaspBee II board descriptor (docs/boards.md §2). The instance behind Board_Get();
 * included only by the board's glue (targets/rb2/board.c).
 *
 * The silicon is the ConBee II's: same ATSAMR21E18A, same AT86RF233 on SERCOM4, same NVRAM
 * and flash layout. The host link is the difference - a SERCOM USART on the pad pair a
 * ConBee II spends on USB (docs/hardware_samr21.md §9) - and it is the only part of this
 * descriptor the ConBee II's does not share. */
#ifndef RB2_BOARD_H
#define RB2_BOARD_H

#include "../board/board.h"
#include "com.h"
#include "product.h"

/* The default origin when the build did not set one (src/CMakeLists.txt APP_ORIGIN):
 * inherited from the ConBee II, whose GCF bootloader the vendor's containers for this
 * board also target. The board's own bootloader is open (docs/hardware_samr21.md §9). */
#ifndef APP_FLASH_ORIGIN
#define APP_FLASH_ORIGIN 0x00005000u
#endif

/* deCONZ NVRAM: the last 4 KB of flash, which every flash must leave intact, so the app
 * image must end below it. The NVM user row carries the factory EUI-64 and serial, and the
 * device-type fuse at 0x80402F that marks this board as the UART module; 0x4FFF, the last
 * byte of the row at 0x4F00, is the bootloader's update flag. */
#define BOARD_NVRAM_BASE 0x0003EF00u
#define BOARD_NVRAM_SIZE 0x00001000u
#define BOARD_NVM_USER_ROW 0x00804000u
#define BOARD_NVRAM_UPDATE_FLAG 0x00004FFFu

static const Board_Descriptor boardRb2 = {
    PRODUCT_NAME,
    PRODUCT_NCP_VERSION,
    PRODUCT_VID,
    PRODUCT_PID,
    BOARD_LINK_UART,
    COM_TX_MAX,
    PRODUCT_LINK_BAUD,
    BOARD_RADIO_AT86RF233,
    -96, /* the AT86RF233's 802.15.4 receive sensitivity (docs/protocol.md §10) */
    -6,  /* the stock firmware's RaspBee II transmit power, before the front end's PA */
    -17, /* and on channel 26 */
    (1u << 16) | (1u << 19), /* PA16 and PA19 switch the front end on: PA19 where the ConBee II has PA17 (docs/hardware_samr21.md §6) */
    BOARD_NVRAM_BASE,
    BOARD_NVRAM_SIZE,
    BOARD_NVM_USER_ROW,
    BOARD_NVRAM_UPDATE_FLAG,
    APP_FLASH_ORIGIN,
    BOARD_NVRAM_BASE /* the guard bound: the image ends below the NVRAM */,
};

#endif
