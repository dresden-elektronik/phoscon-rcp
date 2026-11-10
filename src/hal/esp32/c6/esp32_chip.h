/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* esp32_chip.h — the ESP32-C6's numbers: where each block sits, which interrupt source it raises, where
 * the eFuse MAC is, and how much RAM an app may use (docs/hardware_esp32.md §12.2).
 *
 * The family layer (hal/esp32/) names blocks only through these, and the include path of the board
 * picks this directory (targets/<board>/board.cmake), so a second chip is a second copy of this file.
 * Transcribed from the ESP32-C6 TRM v1.2 and ESP-IDF v5.4.1's components/soc/esp32c6; test/unit/
 * test_esp32_regs.py compares every value with ESP-IDF's headers. */
#ifndef ESP32_CHIP_H
#define ESP32_CHIP_H

#define ESP32_CHIP_NAME "ESP32-C6"
/* The image header's chip ID (esptool's IMAGE_CHIP_ID for this part). */
#define ESP32_CHIP_ID 13u

/* ---- Block bases ---- */
/* The CPU's interrupt controller as ESP-IDF drives it (its PLIC_MX). The TRM describes the same
 * controller as INTPRI, at 0x600C5000, with another register order; the code that runs is ESP-IDF's,
 * so this tree follows it (docs/hardware_esp32.md §12.3). */
#define ESP32_PLIC_BASE 0x20001000u
#define ESP32_TIMG0_BASE 0x60008000u
#define ESP32_SYSTIMER_BASE 0x6000A000u
#define ESP32_USJ_BASE 0x6000F000u
#define ESP32_INTMTX_BASE 0x60010000u
#define ESP32_LP_CLKRST_BASE 0x600B0400u
#define ESP32_EFUSE_BASE 0x600B0800u
#define ESP32_LP_AON_BASE 0x600B1000u
#define ESP32_LP_WDT_BASE 0x600B1C00u

/* ---- Interrupt sources: the interrupt matrix's map register for source n is at INTMTX + 4n ---- */
#define ESP32_SRC_ZB_MAC 12u
#define ESP32_SRC_USJ 48u
#define ESP32_SRC_SYSTIMER_TARGET0 57u

/* ---- eFuse: the factory MAC, block 1 ---- */
/* Low 32 bits of the MAC; then the high 16 in [15:0] and MAC_EXT, the two octets the EUI-64 puts
 * between the halves, in [31:16]. */
#define ESP32_EFUSE_MAC0 0x044u
#define ESP32_EFUSE_MAC1 0x048u

/* ---- Reset ---- */
#define ESP32_LP_CLKRST_RESET_CAUSE 0x010u /* [4:0] the HP core's last reset cause */
#define ESP32_LP_AON_SYS_CFG 0x034u
#define ESP32_LP_AON_HPSYS_SW_RESET (1u << 31)

/* ---- Clocks and memory ---- */
#define ESP32_XTAL_HZ 40000000u
/* The HP SRAM, and the end of what an app may use: above it sit the bootloader's own loader segment
 * and the ROM's stack and data, which are live while the bootloader loads the app (ESP-IDF
 * bootloader.memory.ld.in, SRAM_DRAM_END). */
#define ESP32_SRAM_LOW 0x40800000u
#define ESP32_APP_RAM_END 0x4086E610u

#endif
