/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* board.h — the board descriptor (docs/boards.md §4, docs/architecture.md §2).
 * What the firmware has to know about the board the image is built for:
 * identity, host link, radio, NVRAM and the flash layout. One const instance
 * per board under src/targets/<board>/; an accessor over that instance is
 * declared here and defined by the board's glue. No function pointers, no
 * runtime lookup - the descriptor is data. */
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

/* Host link kind: the ConBee II's native USB CDC, the RaspBee II's SERCOM USART. */
enum
{
    BOARD_LINK_USB_CDC = 0,
    BOARD_LINK_UART
};

/* Radio kind: the silicon the board's transceiver is, not the driver this tree has for it. The
 * ConBee III's is on-die, driven by one of two back-ends (hal/mg21/own/, hal/mg21/rail/;
 * docs/hardware_mg21.md §10.5, §10.19). */
enum
{
    BOARD_RADIO_AT86RF233 = 0,
    BOARD_RADIO_MG21_ON_DIE,
    BOARD_RADIO_ATMEGA256RFR2_ON_DIE /* the ConBee I / RaspBee I: no driver yet (docs/hardware_avr.md §11.8) */
};

typedef struct
{
    /* Identity: iProduct, the Spinel NCP version on the wire, and the USB IDs
     * when the link is USB (0 otherwise). */
    const char *product;
    const char *ncpVersion;
    uint16_t usbVid;
    uint16_t usbPid;

    /* Host link: kind, and the largest single write it accepts. */
    uint8_t linkKind;
    uint8_t linkMaxWrite;
    /* Line rate where the link has one of its own (a USART); 0 where the rate is the
     * bus's, as native USB CDC is. */
    uint32_t linkBaud;

    /* Radio kind: which silicon, per the enum above. */
    uint8_t radioKind;

    /* The radio's receive sensitivity in dBm, as the image reports it for PHY_RX_SENSITIVITY
     * (docs/protocol.md §10): a declared capability, per silicon, not a measurement. */
    int8_t rxSensitivityDbm;

    /* The highest transmit power the board puts out at the transceiver, in dBm, before any front end:
     * the stock firmware's levels, and on channel 26 its lower one (out-of-band emission at the top of
     * the band, docs/hardware_samr21.md §6). The host's PHY_TX_POWER is capped at these. */
    int8_t txPowerMaxDbm;
    int8_t txPowerMaxCh26Dbm;

    /* The radio's external front end: the PORT A pins that switch it on, driven high once at radio
     * init; 0 where the board has no such front end. Its RX/TX control signals are not per board
     * (docs/hardware_samr21.md §6). */
    uint32_t radioFrontEndPins;

    /* What the board's own firmware must keep out of: on the ConBee II and RaspBee II that is the
     * deCONZ NVRAM region (base, size) and the NVM user row holding the factory identity, with the
     * GCF bootloader's update-flag byte inside it. On the ConBee III the same three words name the
     * application's NVM3 region and the user data page, and the update flag is 0 because this
     * bootloader has none (docs/hardware_mg21.md §10.7). */
    uint32_t nvramBase;
    uint32_t nvramSize;
    uint32_t nvmUserRow;
    uint32_t nvramUpdateFlag;

    /* Flash layout: the image origin, and the bound the image must end below
     * (the guard checks it). */
    uint32_t appOrigin;
    uint32_t imageBound;
} Board_Descriptor;

/* The descriptor of the board this image is built for. */
const Board_Descriptor *Board_Get(void);

#endif
