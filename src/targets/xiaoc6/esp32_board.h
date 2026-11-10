/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* The XIAO ESP32-C6's pins, as the ESP32 family layer reads them (hal/esp32/esp32_gpio.h,
 * docs/hardware_esp32.md §12.4). From Seeed Studio's XIAO ESP32-C6 wiki.
 *
 * The board routes its radio through an RF switch: GPIO3 low enables it, then GPIO14 picks the
 * antenna, low the ceramic one on the board and high the U.FL socket; -DDS_ESP32_ANTENNA_UFL=ON selects
 * the socket. The user LED is on GPIO15; which level lights it the wiki does not say, and the image only
 * toggles it, so a blinking LED is the loop's heartbeat either way. */
#ifndef ESP32_BOARD_H
#define ESP32_BOARD_H

#define ESP32_BOARD_RF_SWITCH_EN 3u
#define ESP32_BOARD_RF_SWITCH_EN_LEVEL 0

#define ESP32_BOARD_ANTENNA_SEL 14u
#ifdef DS_ESP32_ANTENNA_UFL
#define ESP32_BOARD_ANTENNA_SEL_LEVEL 1
#else
#define ESP32_BOARD_ANTENNA_SEL_LEVEL 0
#endif

#define ESP32_BOARD_LED 15u
#define ESP32_BOARD_LED_ON_LEVEL 0

#endif /* ESP32_BOARD_H */
