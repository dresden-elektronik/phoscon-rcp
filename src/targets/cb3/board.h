/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* ConBee III board descriptor (docs/hardware_mg21.md §10.8). The instance behind Board_Get(); included only by
 * the board's glue (targets/cb3/board.c).

Identity strings are this image's, not the product's: the product identity of a ConBee III lives in
its user data page (docs/hardware_mg21.md §10.2) and a bring-up image does not read it. The USB
identifiers are zero because this board's host link is a USART behind a bridge the stick did not
design (docs/hardware_mg21.md §10.8): the device a host sees on the bus is the bridge's.

The NVRAM words are the regions a cb3 image must keep out of, which is a different list from the
ConBee II's: there is no deCONZ NVRAM region in the last 4 KiB here. The vendor application's state is
NVM3 at 0xF8000, the bootloader stages an image in the ImageSector below that, and the stick's
identity is in the user data page outside the main flash array. The update-flag byte has no
counterpart: this bootloader verifies with the AppInfo trailer instead (docs/hardware_mg21.md §10.7). */
#ifndef CB3_BOARD_H
#define CB3_BOARD_H

#include "../board/board.h"
#include "com.h"
#include "mg21_regs.h"
#include "product.h"

/* The default origin when the build did not set one (src/CMakeLists.txt APP_ORIGIN): behind this
 * board's bootloader, which occupies 0x0..0x3FFF. */
#ifndef APP_FLASH_ORIGIN
#define APP_FLASH_ORIGIN 0x00004000u
#endif

/* The AppInfo trailer the bootloader writes after the application, at the end of the AppSection. The
 * image bound is the byte it starts at, so the two cannot drift apart: the same number is derived in
 * targets/cb3/flash_layout.cmake and asserted against the linker scripts. */
#define BOARD_APPINFO_TRAILER 16u
#define BOARD_IMAGE_BOUND 0x0007BFF0u

/* Regions a cb3 image does not own: the staging area the bootloader writes an update into, the
 * application storage above it, and the identity page outside the flash array. */
#define BOARD_IMAGE_SECTOR 0x0007C000u
#define BOARD_NVM3_BASE 0x000F8000u
#define BOARD_NVM3_SIZE 0x00008000u

static const Board_Descriptor boardCb3 = {
    PRODUCT_NAME,
    PRODUCT_NCP_VERSION,
    0u, /* no USB identifiers: the link is a USART (docs/hardware_mg21.md §10.8) */
    0u,
    BOARD_LINK_UART,
    COM_TX_MAX,
    PRODUCT_LINK_BAUD,
    BOARD_RADIO_MG21_ON_DIE,
    -104, /* the MG21 on-die radio's 802.15.4 receive sensitivity, the data sheet's -104.3 dBm
          * (docs/hardware_mg21.md §10.1); not measured on this board */
    10, /* the stock firmware's 10 dBm, the part's medium PA at its top; no front end */
    10, /* the stock firmware has no lower level for channel 26 */
    0u, /* no external front end */
    BOARD_NVM3_BASE,
    BOARD_NVM3_SIZE,
    MG21_USERDATA_BASE,
    0u, /* no update-flag byte: this bootloader verifies with the trailer */
    APP_FLASH_ORIGIN,
    BOARD_IMAGE_BOUND /* the guard bound: the image ends below the AppInfo trailer */
};

#endif /* CB3_BOARD_H */
