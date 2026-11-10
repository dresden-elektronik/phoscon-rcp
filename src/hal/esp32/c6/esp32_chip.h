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
#define ESP32_IO_MUX_BASE 0x60090000u
#define ESP32_GPIO_BASE 0x60091000u
#define ESP32_PCR_BASE 0x60096000u
#define ESP32_LP_CLKRST_BASE 0x600B0400u
#define ESP32_EFUSE_BASE 0x600B0800u
#define ESP32_LP_AON_BASE 0x600B1000u
#define ESP32_LP_WDT_BASE 0x600B1C00u
#define ESP32_MODEM_SYSCON_BASE 0x600A9800u
#define ESP32_MODEM_LPCON_BASE 0x600AF000u

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

/* ---- Modem clock gates (ESP-IDF modem_clock.c): what the PHY and the 802.15.4 MAC need running ---- */
#define ESP32_MODEM_SYSCON_CLK_CONF 0x004u
#define ESP32_MODEM_SYSCON_CLK_ETM_EN (1u << 22)
#define ESP32_MODEM_SYSCON_CLK_ZB_APB_EN (1u << 23)
#define ESP32_MODEM_SYSCON_CLK_ZB_MAC_EN (1u << 24)
#define ESP32_MODEM_SYSCON_CLK_CONF1 0x014u
#define ESP32_MODEM_SYSCON_CLK_FE_80M_EN (1u << 13)
#define ESP32_MODEM_SYSCON_CLK_FE_160M_EN (1u << 14)
#define ESP32_MODEM_SYSCON_CLK_FE_CAL_160M_EN (1u << 15)
#define ESP32_MODEM_SYSCON_CLK_FE_APB_EN (1u << 16)
#define ESP32_MODEM_SYSCON_CLK_BT_APB_EN (1u << 17)
#define ESP32_MODEM_SYSCON_CLK_BT_EN (1u << 18)
#define ESP32_MODEM_LPCON_CLK_CONF 0x018u
#define ESP32_MODEM_LPCON_CLK_COEX_EN (1u << 1)
#define ESP32_MODEM_LPCON_CLK_I2C_MST_EN (1u << 2)

/* ---- GPIO: the pins this chip has, and where each one's IO_MUX register is ---- */
#define ESP32_GPIO_PINS 31u
#define ESP32_IO_MUX_GPIO0 0x004u /* GPIO n at ESP32_IO_MUX_GPIO0 + 4n */

/* ---- Clocks and memory ---- */
#define ESP32_XTAL_HZ 40000000u
/* The PLL the HP root clock divides, fixed on this part (ESP-IDF clk_tree_ll.h). */
#define ESP32_PLL_HZ 480000000u
/* The clock registers of the PCR block (TRM ch. 8). */
#define ESP32_PCR_SYSCLK_CONF 0x110u
#define ESP32_PCR_SYSCLK_LS_DIV_SHIFT 0u
#define ESP32_PCR_SYSCLK_HS_DIV_SHIFT 8u
#define ESP32_PCR_SYSCLK_SOC_CLK_SEL_SHIFT 16u
#define ESP32_PCR_SYSCLK_SOC_CLK_PLL 1u
#define ESP32_PCR_CPU_FREQ_CONF 0x118u
#define ESP32_PCR_CPU_LS_DIV_SHIFT 0u
#define ESP32_PCR_CPU_HS_DIV_SHIFT 8u
#define ESP32_PCR_CPU_HS_120M_FORCE (1u << 16)
#define ESP32_PCR_CPU_HS_DIV_MASK (0xFFu << 8)
/* The CPU's fastest clock: the 160 MHz HP root undivided, CPU_HS_DIV_NUM 0 (ESP-IDF clk_tree_ll.h: the
 * root's own divider is fixed at 3 on the PLL path, and the CPU takes 1, 2 or 4 of it). AHB keeps its own
 * divider from the same root, 40 MHz as the bootloader leaves it. */
#define ESP32_CPU_HZ_MAX 160000000u
#define ESP32_PCR_CPU_HS_DIV_MAX_SPEED 0u
#define ESP32_PCR_AHB_FREQ_CONF 0x11Cu
/* The HP SRAM, and the end of what an app may use: above it sit the bootloader's own loader segment
 * and the ROM's stack and data, which are live while the bootloader loads the app (ESP-IDF
 * bootloader.memory.ld.in, SRAM_DRAM_END). */
#define ESP32_SRAM_LOW 0x40800000u
#define ESP32_APP_RAM_END 0x4086E610u

#endif
