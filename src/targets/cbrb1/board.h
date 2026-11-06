/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* ConBee I / RaspBee I board descriptor (docs/hardware_avr.md §11.8). The instance behind Board_Get(); included
 * only by the board's glue (targets/cbrb1/board.c).
 *
 * The flash layout is the bootloader's: the application from address 0 up to the boot section at
 * 0x3E000 (docs/hardware_avr.md §11.1). There is no deCONZ NVRAM region in flash on this board; what
 * the image must keep out of is the EEPROM, whose NV basic set at 0x1FE0 holds the factory identity
 * (§11.7) and whose first 16 bytes belong to the bootloader. The NVRAM words name that set. */
#ifndef CBRB1_BOARD_H
#define CBRB1_BOARD_H

#include "../board/board.h"
#include "avr_ident.h"
#include "com.h"
#include "product.h"

#ifndef APP_FLASH_ORIGIN
#define APP_FLASH_ORIGIN 0x00000000u
#endif

/* The boot section's first byte: the image ends below it. The same number is the linker script's
 * text region end, the image guard's and tools/gcf_check.py's. */
#define BOARD_IMAGE_BOUND 0x0003E000u

static const Board_Descriptor boardCbrb1 = {
    PRODUCT_NAME,
    PRODUCT_NCP_VERSION,
    0u, /* no USB identifiers: the link is a USART; the ConBee I's USB device is the FTDI bridge's */
    0u,
    BOARD_LINK_UART,
    COM_TX_MAX,
    PRODUCT_LINK_BAUD,
    BOARD_RADIO_ATMEGA256RFR2_ON_DIE,
    -100, /* the part's 802.15.4 receive sensitivity; declared, not measured */
    -11, /* PHY_TX_PWR = 0x0E, -11.5 dBm at the transceiver: the vendor firmware's own setting with the
          * front end on, whose amplifier adds about 19 dB (docs/hardware_avr.md §11.5). The driver
          * caps there too; a higher level is not set until the board's output is measured */
    -11,
    0u, /* the front end is the radio driver's to switch (§11.2), not a board pin list */
    AVR_IDENT_BASIC_ADDR,
    AVR_IDENT_BASIC_LEN,
    0u, /* no user row: the identity is the EEPROM set above */
    0u, /* no update-flag byte */
    APP_FLASH_ORIGIN,
    BOARD_IMAGE_BOUND
};

#endif /* CBRB1_BOARD_H */
