/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* esp32_regs.h — the register layouts of the Espressif blocks the ESP32 802.15.4 parts share: only what
 * this tree's HAL uses (docs/hardware_esp32.md §12.1).
 *
 * Offsets and fields are the family's; where a block sits is the chip's (esp32_chip.h, found on the
 * board's include path). The layouts were compared field by field across the ESP32-C6, -H2 and -C5 in
 * ESP-IDF's register headers, and a block that differs on a chip says so here. Transcribed from the
 * ESP32-C6 TRM v1.2 and ESP-IDF v5.4.1; test/unit/test_esp32_regs.py compares every value with ESP-IDF's
 * headers. */
#ifndef ESP32_REGS_H
#define ESP32_REGS_H

#include <stdint.h>

#include "esp32_chip.h"

#define ESP32_REG(base, offset) (*(volatile uint32_t *)((base) + (offset)))

/* The write key every watchdog takes before any other write to it is accepted (TRM 15.2.2.3). */
#define ESP32_WDT_WKEY 0x50D83AA1u

/* ---- SYSTIMER (TRM ch. 13): a 52-bit counter at 16 MHz, 1/16 us a tick ---- */
#define ESP32_SYSTIMER_TICKS_PER_US 16u
#define ESP32_SYSTIMER_CONF 0x000u
#define ESP32_SYSTIMER_CONF_TARGET0_WORK_EN (1u << 24)
#define ESP32_SYSTIMER_CONF_UNIT0_CORE0_STALL_EN (1u << 28)
#define ESP32_SYSTIMER_CONF_UNIT0_WORK_EN (1u << 30)
#define ESP32_SYSTIMER_CONF_CLK_EN (1u << 31)
#define ESP32_SYSTIMER_UNIT0_OP 0x004u
#define ESP32_SYSTIMER_UNIT0_OP_VALUE_VALID (1u << 29)
#define ESP32_SYSTIMER_UNIT0_OP_UPDATE (1u << 30)
#define ESP32_SYSTIMER_TARGET0_HI 0x01Cu
#define ESP32_SYSTIMER_TARGET0_LO 0x020u
#define ESP32_SYSTIMER_TARGET0_CONF 0x034u
#define ESP32_SYSTIMER_TARGET0_CONF_PERIOD_MASK 0x03FFFFFFu
#define ESP32_SYSTIMER_TARGET0_CONF_PERIOD_MODE (1u << 30)
#define ESP32_SYSTIMER_TARGET0_CONF_UNIT1 (1u << 31)
#define ESP32_SYSTIMER_UNIT0_VALUE_HI 0x040u
#define ESP32_SYSTIMER_UNIT0_VALUE_LO 0x044u
#define ESP32_SYSTIMER_COMP0_LOAD 0x050u
#define ESP32_SYSTIMER_COMP0_LOAD_GO (1u << 0)
#define ESP32_SYSTIMER_INT_ENA 0x064u
#define ESP32_SYSTIMER_INT_CLR 0x06Cu
#define ESP32_SYSTIMER_INT_ST 0x070u
#define ESP32_SYSTIMER_INT_TARGET0 (1u << 0)

/* ---- Timer group watchdog, MWDT (TRM ch. 15); four of its fields differ on the C5 ---- */
#define ESP32_TIMG_WDTCONFIG0 0x048u
#define ESP32_TIMG_WDTCONFIG0_FLASHBOOT_MOD_EN (1u << 14)
#define ESP32_TIMG_WDTCONFIG0_SYS_RESET_LENGTH_SHIFT 15u
#define ESP32_TIMG_WDTCONFIG0_CPU_RESET_LENGTH_SHIFT 18u
#define ESP32_TIMG_WDTCONFIG0_CONF_UPDATE_EN (1u << 22)
#define ESP32_TIMG_WDTCONFIG0_STG0_SHIFT 29u
#define ESP32_TIMG_WDTCONFIG0_EN (1u << 31)
#define ESP32_TIMG_WDTCONFIG1 0x04Cu
#define ESP32_TIMG_WDTCONFIG1_DIVCNT_RST (1u << 0)
#define ESP32_TIMG_WDTCONFIG1_CLK_PRESCALE_SHIFT 16u
#define ESP32_TIMG_WDTCONFIG2 0x050u /* stage 0's hold, in prescaled ticks */
#define ESP32_TIMG_WDTFEED 0x060u
#define ESP32_TIMG_WDTWPROTECT 0x064u
/* A stage's action (ESP-IDF wdt_types.h): off, interrupt, CPU reset, system reset. */
#define ESP32_WDT_STAGE_OFF 0u
#define ESP32_WDT_STAGE_RESET_SYSTEM 3u

/* ---- RTC watchdog and super watchdog in the LP domain (TRM ch. 15) ---- */
#define ESP32_LP_WDT_CONFIG0 0x000u
#define ESP32_LP_WDT_CONFIG0_FLASHBOOT_MOD_EN (1u << 12)
#define ESP32_LP_WDT_CONFIG0_EN (1u << 31)
#define ESP32_LP_WDT_FEED 0x014u
#define ESP32_LP_WDT_WPROTECT 0x018u
#define ESP32_LP_WDT_SWD_CONFIG 0x01Cu
#define ESP32_LP_WDT_SWD_CONFIG_AUTO_FEED_EN (1u << 18)
#define ESP32_LP_WDT_SWD_WPROTECT 0x020u

/* ---- USB Serial/JTAG controller, its CDC-ACM half (TRM ch. 32); the C5 adds two registers ---- */
#define ESP32_USJ_EP1 0x000u /* a byte in or out of the FIFO per access, [7:0] */
#define ESP32_USJ_EP1_CONF 0x004u
#define ESP32_USJ_EP1_CONF_WR_DONE (1u << 0)
#define ESP32_USJ_EP1_CONF_IN_EP_DATA_FREE (1u << 1)
#define ESP32_USJ_EP1_CONF_OUT_EP_DATA_AVAIL (1u << 2)
#define ESP32_USJ_INT_RAW 0x008u
#define ESP32_USJ_INT_ST 0x00Cu
#define ESP32_USJ_INT_ENA 0x010u
#define ESP32_USJ_INT_CLR 0x014u
#define ESP32_USJ_INT_OUT_RECV_PKT (1u << 2)
#define ESP32_USJ_INT_IN_EMPTY (1u << 3)
#define ESP32_USJ_FRAM_NUM 0x024u /* [10:0] the last SOF frame number: moves while a host is attached */

/* ---- The CPU interrupt controller of the C6 and H2 (ESP-IDF's PLIC_MX). The C5's is a CLIC ---- */
#define ESP32_PLIC_ENABLE 0x000u
#define ESP32_PLIC_TYPE 0x004u /* 1 = edge */
#define ESP32_PLIC_CLEAR 0x008u
#define ESP32_PLIC_PRI0 0x010u /* PRI n at PRI0 + 4n, 0..15 */
#define ESP32_PLIC_THRESH 0x090u
/* What an interrupt's priority is compared with: below it the line is masked (TRM 1.6.2). */
#define ESP32_PLIC_THRESH_ENABLED 1u

#endif
