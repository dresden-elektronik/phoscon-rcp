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

/* ---- The 802.15.4 MAC (ESP-IDF ieee802154_reg.h; the TRM has no chapter). The H2 adds two debug
 * registers above 0x180; nothing below them moves ---- */
#define ESP32_MAC_COMMAND 0x000u
#define ESP32_MAC_CMD_TX_START 0x41u
#define ESP32_MAC_CMD_RX_START 0x42u
#define ESP32_MAC_CMD_CCA_TX_START 0x43u
#define ESP32_MAC_CMD_ED_START 0x44u
#define ESP32_MAC_CMD_STOP 0x45u
#define ESP32_MAC_CMD_TIMER0_START 0x4Cu
#define ESP32_MAC_CMD_TIMER0_STOP 0x4Du
#define ESP32_MAC_CTRL_CFG 0x004u
#define ESP32_MAC_CTRL_AUTO_ACK_TX (1u << 0)
#define ESP32_MAC_CTRL_AUTO_ACK_RX (1u << 3)
#define ESP32_MAC_CTRL_PAN_COORDINATOR (1u << 6)
#define ESP32_MAC_CTRL_PROMISCUOUS (1u << 7)
#define ESP32_MAC_CTRL_INF0_ENABLE (1u << 28) /* address filter 0 of the four (multipan) */
#define ESP32_MAC_INF0_SHORT_ADDR 0x008u
#define ESP32_MAC_INF0_PAN_ID 0x00Cu
#define ESP32_MAC_INF0_EXT_ADDR0 0x010u /* the low 32 bits of the extended address */
#define ESP32_MAC_INF0_EXT_ADDR1 0x014u
#define ESP32_MAC_CHANNEL 0x048u /* HOP [6:0]: MHz above 2402, 3 + 5 (channel - 11) */
#define ESP32_MAC_CHANNEL_HOP_MASK 0x7Fu
#define ESP32_MAC_TX_POWER 0x04Cu /* [4:0] an index: 15 = +20 dBm, else 3 + (dBm + 15) / 3 from -15 dBm */
#define ESP32_MAC_TX_POWER_MASK 0x1Fu
#define ESP32_MAC_ED_SCAN_DURATION 0x050u /* in symbols, 16 us */
#define ESP32_MAC_ED_SCAN_CFG 0x054u
#define ESP32_MAC_ED_CCA_THRESHOLD_MASK 0xFFu /* [7:0] signed dBm */
#define ESP32_MAC_ED_SAMPLE_MODE_SHIFT 11u /* 0 maximum, 1 average */
#define ESP32_MAC_ED_SAMPLE_AVG 1u
#define ESP32_MAC_ED_CCA_MODE_SHIFT 14u /* 0 carrier, 1 energy, 2 either, 3 both */
#define ESP32_MAC_ED_CCA_MODE_ED 1u
#define ESP32_MAC_ED_RSS_SHIFT 16u /* [23:16] signed dBm: the last measurement */
#define ESP32_MAC_ED_CCA_BUSY (1u << 24)
#define ESP32_MAC_ACK_TIMEOUT 0x05Cu
#define ESP32_MAC_EVENT_EN 0x060u
#define ESP32_MAC_EVENT_STATUS 0x064u /* write 1 to clear */
#define ESP32_MAC_EVENT_TX_DONE (1u << 0)
#define ESP32_MAC_EVENT_RX_DONE (1u << 1)
#define ESP32_MAC_EVENT_ACK_TX_DONE (1u << 2)
#define ESP32_MAC_EVENT_ACK_RX_DONE (1u << 3)
#define ESP32_MAC_EVENT_RX_ABORT (1u << 4)
#define ESP32_MAC_EVENT_TX_ABORT (1u << 5)
#define ESP32_MAC_EVENT_ED_DONE (1u << 6)
#define ESP32_MAC_EVENT_TIMER0_OVERFLOW (1u << 8)
#define ESP32_MAC_EVENT_ALL 0x1FFFu
#define ESP32_MAC_RX_ABORT_EN 0x068u /* bit (reason - 1): which receive aborts raise RX_ABORT */
#define ESP32_MAC_RX_ABORT_COEX_BREAK 7u
#define ESP32_MAC_RX_ABORT_TX_ACK_TIMEOUT 16u
#define ESP32_MAC_RX_ABORT_TX_ACK_COEX_BREAK 18u
#define ESP32_MAC_COEX_PTI 0x070u
#define ESP32_MAC_COEX_PTI_SHIFT 0u
#define ESP32_MAC_COEX_ACK_PTI_SHIFT 4u
#define ESP32_MAC_TX_ABORT_EN 0x078u /* bit (reason - 1): which transmit aborts raise TX_ABORT */
#define ESP32_MAC_TX_ABORT_RX_ACK_TIMEOUT 16u
#define ESP32_MAC_TX_ABORT_TX_COEX_BREAK 18u
#define ESP32_MAC_TX_ABORT_TX_SECURITY_ERROR 19u
#define ESP32_MAC_TX_ABORT_CCA_FAILED 24u
#define ESP32_MAC_TX_ABORT_CCA_BUSY 25u
#define ESP32_MAC_RX_STATUS 0x080u /* FILTER_FAIL [3:0], RX_ABORT_STATUS [8:4], RX_STATE [18:16] */
#define ESP32_MAC_TX_STATUS 0x084u /* TX_STATE [3:0], TX_ABORT_STATUS [8:4] */
#define ESP32_MAC_TX_ABORT_STATUS_SHIFT 4u
#define ESP32_MAC_TX_ABORT_STATUS_MASK 0x1Fu
#define ESP32_MAC_TXRX_STATUS 0x088u /* TXRX_STATE [3:0]: 0 idle; RX_PROC 9, TX_PROC 8 */
#define ESP32_MAC_TXRX_STATE_MASK 0xFu
#define ESP32_MAC_RX_LENGTH 0x0A4u
#define ESP32_MAC_TIMER0_THRESHOLD 0x0A8u /* microseconds */
#define ESP32_MAC_ACK_FRAME_PENDING_EN 0x06Cu
#define ESP32_MAC_ACK_FRAME_PENDING (1u << 0) /* the Frame Pending bit of the next hardware Ack */
#define ESP32_MAC_ACK_TX_ACK_TIMEOUT_SHIFT 16u /* [31:16] the Ack-transmit timeout: never cleared */
#define ESP32_MAC_TXDMA_ADDR 0x0D0u
#define ESP32_MAC_RXDMA_ADDR 0x0E0u
#define ESP32_MAC_RXON_DELAY 0x110u
#define ESP32_MAC_SFD_TIMEOUT_CNT 0x144u
#define ESP32_MAC_CRC_ERROR_CNT 0x148u
#define ESP32_MAC_RX_FILTER_FAIL_CNT 0x154u
#define ESP32_MAC_NO_RSS_DETECT_CNT 0x158u
#define ESP32_MAC_RX_ABORT_COEX_CNT 0x15Cu

/* ---- GPIO and IO_MUX (TRM ch. 7): a pin as a plain software-driven output ---- */
#define ESP32_GPIO_OUT 0x004u
#define ESP32_GPIO_OUT_W1TS 0x008u
#define ESP32_GPIO_OUT_W1TC 0x00Cu
#define ESP32_GPIO_ENABLE 0x020u
#define ESP32_GPIO_ENABLE_W1TS 0x024u
#define ESP32_GPIO_ENABLE_W1TC 0x028u
#define ESP32_GPIO_FUNC0_OUT_SEL_CFG 0x554u /* pin n at + 4n */
/* The output-matrix signal that hands a pin to GPIO_OUT, rather than to a peripheral. */
#define ESP32_GPIO_SIG_GPIO_OUT 128u
#define ESP32_IO_MUX_MCU_SEL_SHIFT 12u
#define ESP32_IO_MUX_MCU_SEL_MASK (7u << 12)
#define ESP32_IO_MUX_FUNC_GPIO 1u
#define ESP32_IO_MUX_FUN_IE (1u << 9)

/* ---- The CPU interrupt controller of the C6 and H2 (ESP-IDF's PLIC_MX). The C5's is a CLIC ---- */
#define ESP32_PLIC_ENABLE 0x000u
#define ESP32_PLIC_TYPE 0x004u /* 1 = edge */
#define ESP32_PLIC_CLEAR 0x008u
#define ESP32_PLIC_PRI0 0x010u /* PRI n at PRI0 + 4n, 0..15 */
#define ESP32_PLIC_THRESH 0x090u
/* What an interrupt's priority is compared with: below it the line is masked (TRM 1.6.2). */
#define ESP32_PLIC_THRESH_ENABLED 1u

#endif
