#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh
"""test_esp32_regs.py - the ESP32 register transcription against ESP-IDF's own headers.

src/hal/esp32/esp32_regs.h (the family's layouts) and src/hal/esp32/<chip>/esp32_chip.h (the chip's
numbers) are this tree's transcription (docs/hardware_esp32.md §12). A wrong offset names another register
and a wrong bit writes a neighbouring field, and neither shows in an arithmetic suite, so every value is
compared with the same fact in ESP-IDF's components/soc/<chip>: block bases from reg_base.h, offsets as a
register's address less its block's base, fields as ESP-IDF's masks and shifts, interrupt sources as the
interrupt matrix's map-register offsets divided by four. Every ESP32_* name is either compared or listed
in NOT_COMPARED with where its value comes from instead; a canary with one wrong value proves the gate can
fail.

ESP-IDF is reference material outside the tree: the suite reads it from ESP_IDF_PATH, or from refs/
esp-idf-v5.4.1 where a maintainer's checkout keeps it, and is skipped (exit 77) without it. Run by ctest as
`esp32_regs`.
"""
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SKIP = 77
CHIP = "esp32c6"
OURS = [os.path.join(REPO, "src", "hal", "esp32", "esp32_regs.h"),
        os.path.join(REPO, "src", "hal", "esp32", "c6", "esp32_chip.h")]

# Where the remaining names come from, and why ESP-IDF's headers do not say.
NOT_COMPARED = {
    "ESP32_REGS_H": "include guard",
    "ESP32_CHIP_H": "include guard",
    "ESP32_REG": "the accessor",
    "ESP32_CHIP_NAME": "a label",
    "ESP32_CHIP_ID": "esptool's IMAGE_CHIP_ID for the part; the guard and the bench read agree",
    "ESP32_XTAL_HZ": "TRM 13.3 and esptool's crystal read on the bench board",
    "ESP32_SYSTIMER_TICKS_PER_US": "TRM 13.3: XTAL_CLK / 2.5 = 16 MHz",
    "ESP32_PLIC_THRESH_ENABLED": "this tree's choice of threshold (TRM 1.6.3.2)",
    "ESP32_CPU_HZ_MAX": "the datasheet's 160 MHz, read back on the bench board",
    "ESP32_PCR_CPU_HS_DIV_MAX_SPEED": "ESP-IDF clk_tree_ll.h's divider rule: CPU_HS_DIV_NUM = divider / 3 - 1",
}

# ours -> an expression over ESP-IDF's names (evaluated with REG_TIMG_BASE(i) at i = 0).
COMPARE = {
    "ESP32_PLIC_BASE": "DR_REG_PLIC_MX_BASE",
    "ESP32_TIMG0_BASE": "DR_REG_TIMERGROUP0_BASE",
    "ESP32_SYSTIMER_BASE": "DR_REG_SYSTIMER_BASE",
    "ESP32_USJ_BASE": "DR_REG_USB_SERIAL_JTAG_BASE",
    "ESP32_INTMTX_BASE": "DR_REG_INTERRUPT_MATRIX_BASE",
    "ESP32_LP_CLKRST_BASE": "DR_REG_LP_CLKRST_BASE",
    "ESP32_EFUSE_BASE": "DR_REG_EFUSE_BASE",
    "ESP32_LP_AON_BASE": "DR_REG_LP_AON_BASE",
    "ESP32_LP_WDT_BASE": "DR_REG_LP_WDT_BASE",
    "ESP32_SRC_ZB_MAC": "(INTMTX_CORE0_ZB_MAC_INTR_MAP_REG - DR_REG_INTERRUPT_MATRIX_BASE) // 4",
    "ESP32_SRC_USJ": "(INTMTX_CORE0_USB_INTR_MAP_REG - DR_REG_INTERRUPT_MATRIX_BASE) // 4",
    "ESP32_SRC_SYSTIMER_TARGET0": "(INTMTX_CORE0_SYSTIMER_TARGET0_INTR_MAP_REG - DR_REG_INTERRUPT_MATRIX_BASE) // 4",
    "ESP32_EFUSE_MAC0": "EFUSE_RD_MAC_SPI_SYS_0_REG - DR_REG_EFUSE_BASE",
    "ESP32_EFUSE_MAC1": "EFUSE_RD_MAC_SPI_SYS_1_REG - DR_REG_EFUSE_BASE",
    "ESP32_LP_CLKRST_RESET_CAUSE": "LP_CLKRST_RESET_CAUSE_REG - DR_REG_LP_CLKRST_BASE",
    "ESP32_LP_AON_SYS_CFG": "LP_AON_SYS_CFG_REG - DR_REG_LP_AON_BASE",
    "ESP32_LP_AON_HPSYS_SW_RESET": "LP_AON_HPSYS_SW_RESET",
    "ESP32_SRAM_LOW": "SOC_DRAM_LOW",
    "ESP32_APP_RAM_END": "BOOTLOADER_IRAM_LOADER_SEG_START",
    "ESP32_WDT_WKEY": "LP_WDT_WKEY_VALUE",
    "ESP32_SYSTIMER_CONF": "SYSTIMER_CONF_REG - DR_REG_SYSTIMER_BASE",
    "ESP32_SYSTIMER_CONF_TARGET0_WORK_EN": "SYSTIMER_TARGET0_WORK_EN",
    "ESP32_SYSTIMER_CONF_UNIT0_CORE0_STALL_EN": "SYSTIMER_TIMER_UNIT0_CORE0_STALL_EN",
    "ESP32_SYSTIMER_CONF_UNIT0_WORK_EN": "SYSTIMER_TIMER_UNIT0_WORK_EN",
    "ESP32_SYSTIMER_CONF_CLK_EN": "SYSTIMER_CLK_EN",
    "ESP32_SYSTIMER_UNIT0_OP": "SYSTIMER_UNIT0_OP_REG - DR_REG_SYSTIMER_BASE",
    "ESP32_SYSTIMER_UNIT0_OP_VALUE_VALID": "SYSTIMER_TIMER_UNIT0_VALUE_VALID",
    "ESP32_SYSTIMER_UNIT0_OP_UPDATE": "SYSTIMER_TIMER_UNIT0_UPDATE",
    "ESP32_SYSTIMER_TARGET0_HI": "SYSTIMER_TARGET0_HI_REG - DR_REG_SYSTIMER_BASE",
    "ESP32_SYSTIMER_TARGET0_LO": "SYSTIMER_TARGET0_LO_REG - DR_REG_SYSTIMER_BASE",
    "ESP32_SYSTIMER_TARGET0_CONF": "SYSTIMER_TARGET0_CONF_REG - DR_REG_SYSTIMER_BASE",
    "ESP32_SYSTIMER_TARGET0_CONF_PERIOD_MASK": "SYSTIMER_TARGET0_PERIOD_V",
    "ESP32_SYSTIMER_TARGET0_CONF_PERIOD_MODE": "SYSTIMER_TARGET0_PERIOD_MODE",
    "ESP32_SYSTIMER_TARGET0_CONF_UNIT1": "SYSTIMER_TARGET0_TIMER_UNIT_SEL",
    "ESP32_SYSTIMER_UNIT0_VALUE_HI": "SYSTIMER_UNIT0_VALUE_HI_REG - DR_REG_SYSTIMER_BASE",
    "ESP32_SYSTIMER_UNIT0_VALUE_LO": "SYSTIMER_UNIT0_VALUE_LO_REG - DR_REG_SYSTIMER_BASE",
    "ESP32_SYSTIMER_COMP0_LOAD": "SYSTIMER_COMP0_LOAD_REG - DR_REG_SYSTIMER_BASE",
    "ESP32_SYSTIMER_COMP0_LOAD_GO": "SYSTIMER_TIMER_COMP0_LOAD",
    "ESP32_SYSTIMER_INT_ENA": "SYSTIMER_INT_ENA_REG - DR_REG_SYSTIMER_BASE",
    "ESP32_SYSTIMER_INT_CLR": "SYSTIMER_INT_CLR_REG - DR_REG_SYSTIMER_BASE",
    "ESP32_SYSTIMER_INT_ST": "SYSTIMER_INT_ST_REG - DR_REG_SYSTIMER_BASE",
    "ESP32_SYSTIMER_INT_TARGET0": "SYSTIMER_TARGET0_INT_ENA",
    "ESP32_TIMG_WDTCONFIG0": "TIMG_WDTCONFIG0_REG(0) - DR_REG_TIMERGROUP0_BASE",
    "ESP32_TIMG_WDTCONFIG0_FLASHBOOT_MOD_EN": "TIMG_WDT_FLASHBOOT_MOD_EN",
    "ESP32_TIMG_WDTCONFIG0_SYS_RESET_LENGTH_SHIFT": "TIMG_WDT_SYS_RESET_LENGTH_S",
    "ESP32_TIMG_WDTCONFIG0_CPU_RESET_LENGTH_SHIFT": "TIMG_WDT_CPU_RESET_LENGTH_S",
    "ESP32_TIMG_WDTCONFIG0_CONF_UPDATE_EN": "TIMG_WDT_CONF_UPDATE_EN",
    "ESP32_TIMG_WDTCONFIG0_STG0_SHIFT": "TIMG_WDT_STG0_S",
    "ESP32_TIMG_WDTCONFIG0_EN": "TIMG_WDT_EN",
    "ESP32_TIMG_WDTCONFIG1": "TIMG_WDTCONFIG1_REG(0) - DR_REG_TIMERGROUP0_BASE",
    "ESP32_TIMG_WDTCONFIG1_DIVCNT_RST": "TIMG_WDT_DIVCNT_RST",
    "ESP32_TIMG_WDTCONFIG1_CLK_PRESCALE_SHIFT": "TIMG_WDT_CLK_PRESCALE_S",
    "ESP32_TIMG_WDTCONFIG2": "TIMG_WDTCONFIG2_REG(0) - DR_REG_TIMERGROUP0_BASE",
    "ESP32_TIMG_WDTFEED": "TIMG_WDTFEED_REG(0) - DR_REG_TIMERGROUP0_BASE",
    "ESP32_TIMG_WDTWPROTECT": "TIMG_WDTWPROTECT_REG(0) - DR_REG_TIMERGROUP0_BASE",
    "ESP32_WDT_STAGE_OFF": "WDT_STAGE_ACTION_OFF",
    "ESP32_WDT_STAGE_RESET_SYSTEM": "WDT_STAGE_ACTION_RESET_SYSTEM",
    "ESP32_LP_WDT_CONFIG0": "LP_WDT_CONFIG0_REG - DR_REG_LP_WDT_BASE",
    "ESP32_LP_WDT_CONFIG0_FLASHBOOT_MOD_EN": "LP_WDT_WDT_FLASHBOOT_MOD_EN",
    "ESP32_LP_WDT_CONFIG0_EN": "LP_WDT_WDT_EN",
    "ESP32_LP_WDT_FEED": "LP_WDT_FEED_REG - DR_REG_LP_WDT_BASE",
    "ESP32_LP_WDT_WPROTECT": "LP_WDT_WPROTECT_REG - DR_REG_LP_WDT_BASE",
    "ESP32_LP_WDT_SWD_CONFIG": "LP_WDT_SWD_CONFIG_REG - DR_REG_LP_WDT_BASE",
    "ESP32_LP_WDT_SWD_CONFIG_AUTO_FEED_EN": "LP_WDT_SWD_AUTO_FEED_EN",
    "ESP32_LP_WDT_SWD_WPROTECT": "LP_WDT_SWD_WPROTECT_REG - DR_REG_LP_WDT_BASE",
    "ESP32_USJ_EP1": "USB_SERIAL_JTAG_EP1_REG - DR_REG_USB_SERIAL_JTAG_BASE",
    "ESP32_USJ_EP1_CONF": "USB_SERIAL_JTAG_EP1_CONF_REG - DR_REG_USB_SERIAL_JTAG_BASE",
    "ESP32_USJ_EP1_CONF_WR_DONE": "USB_SERIAL_JTAG_WR_DONE",
    "ESP32_USJ_EP1_CONF_IN_EP_DATA_FREE": "USB_SERIAL_JTAG_SERIAL_IN_EP_DATA_FREE",
    "ESP32_USJ_EP1_CONF_OUT_EP_DATA_AVAIL": "USB_SERIAL_JTAG_SERIAL_OUT_EP_DATA_AVAIL",
    "ESP32_USJ_INT_RAW": "USB_SERIAL_JTAG_INT_RAW_REG - DR_REG_USB_SERIAL_JTAG_BASE",
    "ESP32_USJ_INT_ST": "USB_SERIAL_JTAG_INT_ST_REG - DR_REG_USB_SERIAL_JTAG_BASE",
    "ESP32_USJ_INT_ENA": "USB_SERIAL_JTAG_INT_ENA_REG - DR_REG_USB_SERIAL_JTAG_BASE",
    "ESP32_USJ_INT_CLR": "USB_SERIAL_JTAG_INT_CLR_REG - DR_REG_USB_SERIAL_JTAG_BASE",
    "ESP32_USJ_INT_OUT_RECV_PKT": "USB_SERIAL_JTAG_SERIAL_OUT_RECV_PKT_INT_ENA",
    "ESP32_USJ_INT_IN_EMPTY": "USB_SERIAL_JTAG_SERIAL_IN_EMPTY_INT_ENA",
    "ESP32_USJ_FRAM_NUM": "USB_SERIAL_JTAG_FRAM_NUM_REG - DR_REG_USB_SERIAL_JTAG_BASE",
    "ESP32_IO_MUX_BASE": "DR_REG_IO_MUX_BASE",
    "ESP32_GPIO_BASE": "DR_REG_GPIO_BASE",
    "ESP32_PCR_BASE": "DR_REG_PCR_BASE",
    "ESP32_GPIO_PINS": "SOC_GPIO_PIN_COUNT",
    "ESP32_IO_MUX_GPIO0": "IO_MUX_GPIO0_REG - DR_REG_IO_MUX_BASE",
    "ESP32_PLL_HZ": "CLK_LL_PLL_480M_FREQ_MHZ * 1000000",
    "ESP32_PCR_SYSCLK_CONF": "PCR_SYSCLK_CONF_REG - DR_REG_PCR_BASE",
    "ESP32_PCR_SYSCLK_LS_DIV_SHIFT": "PCR_LS_DIV_NUM_S",
    "ESP32_PCR_SYSCLK_HS_DIV_SHIFT": "PCR_HS_DIV_NUM_S",
    "ESP32_PCR_SYSCLK_SOC_CLK_SEL_SHIFT": "PCR_SOC_CLK_SEL_S",
    "ESP32_PCR_SYSCLK_SOC_CLK_PLL": "SOC_CPU_CLK_SRC_PLL",
    "ESP32_PCR_CPU_FREQ_CONF": "PCR_CPU_FREQ_CONF_REG - DR_REG_PCR_BASE",
    "ESP32_PCR_CPU_LS_DIV_SHIFT": "PCR_CPU_LS_DIV_NUM_S",
    "ESP32_PCR_CPU_HS_DIV_SHIFT": "PCR_CPU_HS_DIV_NUM_S",
    "ESP32_PCR_CPU_HS_120M_FORCE": "PCR_CPU_HS_120M_FORCE",
    "ESP32_PCR_CPU_HS_DIV_MASK": "PCR_CPU_HS_DIV_NUM_M",
    "ESP32_PCR_AHB_FREQ_CONF": "PCR_AHB_FREQ_CONF_REG - DR_REG_PCR_BASE",
    "ESP32_GPIO_OUT": "GPIO_OUT_REG - DR_REG_GPIO_BASE",
    "ESP32_GPIO_OUT_W1TS": "GPIO_OUT_W1TS_REG - DR_REG_GPIO_BASE",
    "ESP32_GPIO_OUT_W1TC": "GPIO_OUT_W1TC_REG - DR_REG_GPIO_BASE",
    "ESP32_GPIO_ENABLE": "GPIO_ENABLE_REG - DR_REG_GPIO_BASE",
    "ESP32_GPIO_ENABLE_W1TS": "GPIO_ENABLE_W1TS_REG - DR_REG_GPIO_BASE",
    "ESP32_GPIO_ENABLE_W1TC": "GPIO_ENABLE_W1TC_REG - DR_REG_GPIO_BASE",
    "ESP32_GPIO_FUNC0_OUT_SEL_CFG": "GPIO_FUNC0_OUT_SEL_CFG_REG - DR_REG_GPIO_BASE",
    "ESP32_GPIO_SIG_GPIO_OUT": "SIG_GPIO_OUT_IDX",
    "ESP32_IO_MUX_MCU_SEL_SHIFT": "MCU_SEL_S",
    "ESP32_IO_MUX_MCU_SEL_MASK": "MCU_SEL_V << MCU_SEL_S",
    "ESP32_IO_MUX_FUNC_GPIO": "PIN_FUNC_GPIO",
    "ESP32_IO_MUX_FUN_IE": "1 << FUN_IE_S",
    "ESP32_MODEM_SYSCON_BASE": "DR_REG_MODEM_SYSCON_BASE",
    "ESP32_MODEM_LPCON_BASE": "DR_REG_MODEM_LPCON_BASE",
    "ESP32_MODEM_SYSCON_CLK_CONF": "MODEM_SYSCON_CLK_CONF_REG - DR_REG_MODEM_SYSCON_BASE",
    "ESP32_MODEM_SYSCON_CLK_ETM_EN": "MODEM_SYSCON_CLK_ETM_EN",
    "ESP32_MODEM_SYSCON_CLK_ZB_APB_EN": "MODEM_SYSCON_CLK_ZB_APB_EN",
    "ESP32_MODEM_SYSCON_CLK_ZB_MAC_EN": "MODEM_SYSCON_CLK_ZB_MAC_EN",
    "ESP32_MODEM_SYSCON_CLK_CONF1": "MODEM_SYSCON_CLK_CONF1_REG - DR_REG_MODEM_SYSCON_BASE",
    "ESP32_MODEM_SYSCON_CLK_FE_80M_EN": "MODEM_SYSCON_CLK_FE_80M_EN",
    "ESP32_MODEM_SYSCON_CLK_FE_160M_EN": "MODEM_SYSCON_CLK_FE_160M_EN",
    "ESP32_MODEM_SYSCON_CLK_FE_CAL_160M_EN": "MODEM_SYSCON_CLK_FE_CAL_160M_EN",
    "ESP32_MODEM_SYSCON_CLK_FE_APB_EN": "MODEM_SYSCON_CLK_FE_APB_EN",
    "ESP32_MODEM_SYSCON_CLK_BT_APB_EN": "MODEM_SYSCON_CLK_BT_APB_EN",
    "ESP32_MODEM_SYSCON_CLK_BT_EN": "MODEM_SYSCON_CLK_BT_EN",
    "ESP32_MODEM_LPCON_CLK_CONF": "MODEM_LPCON_CLK_CONF_REG - DR_REG_MODEM_LPCON_BASE",
    "ESP32_MODEM_LPCON_CLK_COEX_EN": "MODEM_LPCON_CLK_COEX_EN",
    "ESP32_MODEM_LPCON_CLK_I2C_MST_EN": "MODEM_LPCON_CLK_I2C_MST_EN",
    "ESP32_MAC_BASE": "IEEE802154_REG_BASE",
    "ESP32_HP_APM_BASE": "DR_REG_HP_APM_BASE",
    "ESP32_LP_APM0_BASE": "DR_REG_LP_APM0_BASE",
    "ESP32_LP_APM_BASE": "DR_REG_LP_APM_BASE",
    "ESP32_APM_FUNC_CTRL": "HP_APM_FUNC_CTRL_REG - DR_REG_HP_APM_BASE",
    "ESP32_HP_APM_PATHS": "(1 << HP_APM_MAX_ACCESS_PATH) - 1",
    "ESP32_LP_APM0_PATHS": "(1 << LP_APM0_MAX_ACCESS_PATH) - 1",
    "ESP32_LP_APM_PATHS": "(1 << LP_APM_MAX_ACCESS_PATH) - 1",
    "ESP32_MODEM_SYSCON_CLK_CONF_POWER_ST": "MODEM_SYSCON_CLK_CONF_POWER_ST_REG - DR_REG_MODEM_SYSCON_BASE",
    "ESP32_MODEM_SYSCON_ICG_RUN": "(4 << MODEM_SYSCON_CLK_ZB_ST_MAP_S) | (6 << MODEM_SYSCON_CLK_FE_ST_MAP_S) | (4 << MODEM_SYSCON_CLK_BT_ST_MAP_S) | (6 << MODEM_SYSCON_CLK_WIFI_ST_MAP_S) | (4 << MODEM_SYSCON_CLK_MODEM_PERI_ST_MAP_S) | (6 << MODEM_SYSCON_CLK_MODEM_APB_ST_MAP_S)",
    "ESP32_MODEM_LPCON_CLK_CONF_POWER_ST": "MODEM_LPCON_CLK_CONF_POWER_ST_REG - DR_REG_MODEM_LPCON_BASE",
    "ESP32_MODEM_LPCON_ICG_RUN": "(6 << MODEM_LPCON_CLK_WIFIPWR_ST_MAP_S) | (6 << MODEM_LPCON_CLK_COEX_ST_MAP_S) | (6 << MODEM_LPCON_CLK_I2C_MST_ST_MAP_S) | (6 << MODEM_LPCON_CLK_LP_APB_ST_MAP_S)",
    "ESP32_MODEM_SYSCON_MODEM_RST_CONF": "MODEM_SYSCON_MODEM_RST_CONF_REG - DR_REG_MODEM_SYSCON_BASE",
    "ESP32_MODEM_SYSCON_RST_ZBMAC": "1 << MODEM_SYSCON_RST_ZBMAC_S",
    "ESP32_MODEM_SYSCON_RST_RADIO": "(1 << MODEM_SYSCON_RST_WIFIBB_S) | (1 << MODEM_SYSCON_RST_WIFIMAC_S) | (1 << MODEM_SYSCON_RST_FE_S) | (1 << MODEM_SYSCON_RST_BTMAC_APB_S) | (1 << MODEM_SYSCON_RST_BTMAC_S) | (1 << MODEM_SYSCON_RST_BTBB_APB_S) | (1 << MODEM_SYSCON_RST_BTBB_S) | (1 << MODEM_SYSCON_RST_ZBMAC_S)",
    "ESP32_MODEM_SYSCON_CLK_SEC_AND_BLE_TIMER": "(1 << MODEM_SYSCON_CLK_MODEM_SEC_ECB_EN_S) | (1 << MODEM_SYSCON_CLK_MODEM_SEC_CCM_EN_S) | (1 << MODEM_SYSCON_CLK_MODEM_SEC_BAH_EN_S) | (1 << MODEM_SYSCON_CLK_MODEM_SEC_APB_EN_S) | (1 << MODEM_SYSCON_CLK_MODEM_SEC_EN_S) | (1 << MODEM_SYSCON_CLK_BLE_TIMER_EN_S)",
    "ESP32_PHY_COMBO_MODULE": "SOC_PHY_COMBO_MODULE",
    "ESP32_MAC_COMMAND": "IEEE802154_COMMAND_REG - IEEE802154_REG_BASE",
    "ESP32_MAC_CMD_TX_START": "IEEE802154_CMD_TX_START",
    "ESP32_MAC_CMD_RX_START": "IEEE802154_CMD_RX_START",
    "ESP32_MAC_CMD_CCA_TX_START": "IEEE802154_CMD_CCA_TX_START",
    "ESP32_MAC_CMD_ED_START": "IEEE802154_CMD_ED_START",
    "ESP32_MAC_CMD_STOP": "IEEE802154_CMD_STOP",
    "ESP32_MAC_CTRL_CFG": "IEEE802154_CTRL_CFG_REG - IEEE802154_REG_BASE",
    "ESP32_MAC_CTRL_AUTO_ACK_TX": "1 << IEEE802154_HW_AUTO_ACK_TX_EN_S",
    "ESP32_MAC_CTRL_AUTO_ACK_RX": "1 << IEEE802154_HW_AUTO_ACK_RX_EN_S",
    "ESP32_MAC_CTRL_PAN_COORDINATOR": "1 << IEEE802154_PAN_COORDINATOR_S",
    "ESP32_MAC_CTRL_PROMISCUOUS": "1 << IEEE802154_PROMISCUOUS_MODE_S",
    "ESP32_MAC_CHANNEL": "IEEE802154_CHANNEL_REG - IEEE802154_REG_BASE",
    "ESP32_MAC_CHANNEL_HOP_MASK": "IEEE802154_HOP",
    "ESP32_MAC_EVENT_EN": "IEEE802154_EVENT_EN_REG - IEEE802154_REG_BASE",
    "ESP32_MAC_EVENT_STATUS": "IEEE802154_EVENT_STATUS_REG - IEEE802154_REG_BASE",
    "ESP32_MAC_EVENT_TX_DONE": "IEEE802154_EVENT_TX_DONE",
    "ESP32_MAC_EVENT_RX_DONE": "IEEE802154_EVENT_RX_DONE",
    "ESP32_MAC_EVENT_ACK_TX_DONE": "IEEE802154_EVENT_ACK_TX_DONE",
    "ESP32_MAC_EVENT_ACK_RX_DONE": "IEEE802154_EVENT_ACK_RX_DONE",
    "ESP32_MAC_EVENT_RX_ABORT": "IEEE802154_EVENT_RX_ABORT",
    "ESP32_MAC_EVENT_TX_ABORT": "IEEE802154_EVENT_TX_ABORT",
    "ESP32_MAC_EVENT_ED_DONE": "IEEE802154_EVENT_ED_DONE",
    "ESP32_MAC_EVENT_ALL": "IEEE802154_EVENT_EN",
    "ESP32_MAC_RX_ABORT_EN": "IEEE802154_RX_ABORT_INTR_CTRL_REG - IEEE802154_REG_BASE",
    "ESP32_MAC_RX_ABORT_COEX_BREAK": "IEEE802154_RX_ABORT_BY_COEX_BREAK",
    "ESP32_MAC_RX_ABORT_TX_ACK_TIMEOUT": "IEEE802154_RX_ABORT_BY_TX_ACK_TIMEOUT",
    "ESP32_MAC_RX_ABORT_TX_ACK_COEX_BREAK": "IEEE802154_RX_ABORT_BY_TX_ACK_COEX_BREAK",
    "ESP32_MAC_COEX_PTI": "IEEE802154_COEX_PTI_REG - IEEE802154_REG_BASE",
    "ESP32_MAC_COEX_PTI_SHIFT": "IEEE802154_COEX_PTI_S",
    "ESP32_MAC_COEX_ACK_PTI_SHIFT": "IEEE802154_COEX_ACK_PTI_S",
    "ESP32_MAC_RX_STATUS": "IEEE802154_RX_STATUS_REG - IEEE802154_REG_BASE",
    "ESP32_MAC_TXRX_STATUS": "IEEE802154_TXRX_STATUS_REG - IEEE802154_REG_BASE",
    "ESP32_MAC_RX_LENGTH": "IEEE802154_RX_LENGTH_REG - IEEE802154_REG_BASE",
    "ESP32_MAC_RXDMA_ADDR": "IEEE802154_RXDMA_ADDR_REG - IEEE802154_REG_BASE",
    "ESP32_MAC_RXON_DELAY": "IEEE802154_RXON_DELAY_REG - IEEE802154_REG_BASE",
    "ESP32_MAC_SFD_TIMEOUT_CNT": "IEEE802154_SFD_TIMEOUT_CNT_REG - IEEE802154_REG_BASE",
    "ESP32_MAC_CRC_ERROR_CNT": "IEEE802154_CRC_ERROR_CNT_REG - IEEE802154_REG_BASE",
    "ESP32_MAC_RX_FILTER_FAIL_CNT": "IEEE802154_RX_FILTER_FAIL_CNT_REG - IEEE802154_REG_BASE",
    "ESP32_MAC_NO_RSS_DETECT_CNT": "IEEE802154_NO_RSS_DETECT_CNT_REG - IEEE802154_REG_BASE",
    "ESP32_MAC_RX_ABORT_COEX_CNT": "IEEE802154_RX_ABORT_COEX_CNT_REG - IEEE802154_REG_BASE",
    "ESP32_MAC_CTRL_INF0_ENABLE": "1 << IEEE802154_MAC_INF0_ENABLE_S",
    "ESP32_MAC_INF0_SHORT_ADDR": "IEEE802154_INF0_SHORT_ADDR_REG - IEEE802154_REG_BASE",
    "ESP32_MAC_INF0_PAN_ID": "IEEE802154_INF0_PAN_ID_REG - IEEE802154_REG_BASE",
    "ESP32_MAC_INF0_EXT_ADDR0": "IEEE802154_INF0_EXTEND_ADDR0_REG - IEEE802154_REG_BASE",
    "ESP32_MAC_INF0_EXT_ADDR1": "IEEE802154_INF0_EXTEND_ADDR1_REG - IEEE802154_REG_BASE",
    "ESP32_MAC_TX_POWER": "IEEE802154_TX_POWER_REG - IEEE802154_REG_BASE",
    "ESP32_MAC_TX_POWER_MASK": "IEEE802154_TX_POWER",
    "ESP32_MAC_ED_SCAN_DURATION": "IEEE802154_ED_SCAN_DURATION_REG - IEEE802154_REG_BASE",
    "ESP32_MAC_ED_SCAN_CFG": "IEEE802154_ED_SCAN_CFG_REG - IEEE802154_REG_BASE",
    "ESP32_MAC_ED_CCA_THRESHOLD_MASK": "IEEE802154_CCA_ED_THRESHOLD",
    "ESP32_MAC_ED_SAMPLE_MODE_SHIFT": "IEEE802154_ED_SAMPLE_MODE_S",
    "ESP32_MAC_ED_SAMPLE_AVG": "IEEE802154_ED_SAMPLE_AVG",
    "ESP32_MAC_ED_CCA_MODE_SHIFT": "IEEE802154_CCA_MODE_S",
    "ESP32_MAC_ED_CCA_MODE_ED": "IEEE802154_CCA_MODE_ED",
    "ESP32_MAC_ED_RSS_SHIFT": "IEEE802154_ED_RSS_S",
    "ESP32_MAC_ED_CCA_BUSY": "IEEE802154_CCA_BUSY",
    "ESP32_MAC_ACK_TIMEOUT": "IEEE802154_ACK_TIMEOUT_REG - IEEE802154_REG_BASE",
    "ESP32_MAC_CMD_TIMER0_START": "IEEE802154_CMD_TIMER0_START",
    "ESP32_MAC_CTRL_AUTO_ENHACK": "HW_ENHANCE_ACK_TX_EN",
    "ESP32_MODEM_ETM_BASE": "ETM_REG_BASE",
    "ESP32_ETM_CHEN": "ETM_CHEN_AD0_REG - ETM_REG_BASE",
    "ESP32_ETM_CHENSET": "ETM_CHENSET_AD0_REG - ETM_REG_BASE",
    "ESP32_ETM_CHENCLR": "ETM_CHENCLR_AD0_REG - ETM_REG_BASE",
    "ESP32_ETM_CH0_EVT_ID": "ETM_CH0_EVT_ID_REG - ETM_REG_BASE",
    "ESP32_ETM_CH0_TASK_ID": "ETM_CH0_TASK_ID_REG - ETM_REG_BASE",
    "ESP32_ETM_CH_STRIDE": "ETM_CH_OFFSET",
    "ESP32_ETM_EVT_ZB_TIMER0_OVERFLOW": "ETM_EVENT_TIMER0_OVERFLOW",
    "ESP32_ETM_TASK_ZB_ED_TRIG_TX": "ETM_TASK_ED_TRIG_TX",
    "ESP32_ETM_TASK_ZB_TX_START": "ETM_TASK_TX_START",
    "ESP32_MAC_EVENT_TX_SFD_DONE": "IEEE802154_EVENT_TX_SFD_DONE",
    "ESP32_MAC_EVENT_RX_SFD_DONE": "IEEE802154_EVENT_RX_SFD_DONE",
    "ESP32_MAC_ENHACK_DONE": "IEEE802154_ENHANCE_ACK_CFG_REG - IEEE802154_REG_BASE",
    "ESP32_MAC_CMD_TIMER0_STOP": "IEEE802154_CMD_TIMER0_STOP",
    "ESP32_MAC_EVENT_TIMER0_OVERFLOW": "IEEE802154_EVENT_TIMER0_OVERFLOW",
    "ESP32_MAC_TIMER0_THRESHOLD": "IEEE802154_TIME0_THRESHOLD_REG - IEEE802154_REG_BASE",
    "ESP32_MAC_ACK_FRAME_PENDING_EN": "IEEE802154_ACK_FRAME_PENDING_EN_REG - IEEE802154_REG_BASE",
    "ESP32_MAC_ACK_FRAME_PENDING": "IEEE802154_ACK_FRAME_PENDING_EN",
    "ESP32_MAC_ACK_TX_ACK_TIMEOUT_SHIFT": "IEEE802154_ACK_TX_ACK_TIMEOUT_S",
    "ESP32_MAC_TXRX_STATE_MASK": "IEEE802154_TXRX_STATE",
    "ESP32_LPPERI_BASE": "DR_REG_LPPERI_BASE",
    "ESP32_LPPERI_RNG_DATA": "LPPERI_RNG_DATA_REG - DR_REG_LPPERI_BASE",
    "ESP32_MAC_TX_ABORT_EN": "IEEE802154_TX_ABORT_INTERRUPT_CONTROL_REG - IEEE802154_REG_BASE",
    "ESP32_MAC_TX_ABORT_RX_ACK_TIMEOUT": "IEEE802154_TX_ABORT_BY_RX_ACK_TIMEOUT",
    "ESP32_MAC_TX_ABORT_TX_COEX_BREAK": "IEEE802154_TX_ABORT_BY_TX_COEX_BREAK",
    "ESP32_MAC_TX_ABORT_TX_SECURITY_ERROR": "IEEE802154_TX_ABORT_BY_TX_SECURITY_ERROR",
    "ESP32_MAC_TX_ABORT_CCA_FAILED": "IEEE802154_TX_ABORT_BY_CCA_FAILED",
    "ESP32_MAC_TX_ABORT_CCA_BUSY": "IEEE802154_TX_ABORT_BY_CCA_BUSY",
    "ESP32_MAC_TX_STATUS": "IEEE802154_TX_STATUS_REG - IEEE802154_REG_BASE",
    "ESP32_MAC_TX_ABORT_STATUS_SHIFT": "IEEE802154_TX_ABORT_STATUS_S",
    "ESP32_MAC_TX_ABORT_STATUS_MASK": "IEEE802154_TX_ABORT_STATUS",
    "ESP32_MAC_TXDMA_ADDR": "IEEE802154_TXDMA_ADDR_REG - IEEE802154_REG_BASE",
    "ESP32_PLIC_ENABLE": "PLIC_MXINT_ENABLE_REG - DR_REG_PLIC_MX_BASE",
    "ESP32_PLIC_TYPE": "PLIC_MXINT_TYPE_REG - DR_REG_PLIC_MX_BASE",
    "ESP32_PLIC_CLEAR": "PLIC_MXINT_CLEAR_REG - DR_REG_PLIC_MX_BASE",
    "ESP32_PLIC_PRI0": "PLIC_MXINT0_PRI_REG - DR_REG_PLIC_MX_BASE",
    "ESP32_PLIC_THRESH": "PLIC_MXINT_THRESH_REG - DR_REG_PLIC_MX_BASE",
}

DEFINE = re.compile(r"^\s*#\s*define\s+(\w+)(\(\w+\))?[ \t]+(.+?)\s*(?:/\*.*|//.*)?$", re.M)


def idf_root():
    for root in (os.environ.get("ESP_IDF_PATH"), os.path.join(REPO, "refs", "esp-idf-v5.4.1")):
        if root and os.path.isdir(os.path.join(root, "components", "soc", CHIP)):
            return root
    return None


def collect(paths):
    objects, functions = {}, {}
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
        for name, param, body in DEFINE.findall(text):
            if param:
                functions.setdefault(name, (param.strip("()"), body))
            else:
                objects.setdefault(name, body)
        # Enumerators too: a number, a hex number or BIT(n), as ESP-IDF's enums initialise them.
        for name, value in re.findall(r"^\s*([A-Z][A-Z0-9_]*)\s*=\s*(0x[0-9A-Fa-f]+|\d+|BIT\(\d+\))\s*,",
                                      text, re.M):
            objects.setdefault(name, value)
    return objects, functions


def evaluate(expr, objects, functions, depth=0):
    if depth > 40:
        raise ValueError(f"too deep: {expr}")
    expr = re.sub(r"\bBIT\((\d+)\)", r"(1 << \1)", expr)
    previous = None
    while previous != expr:                    # until no function-like macro is left, nested ones included
        previous = expr
        for name, (param, body) in functions.items():
            expr = re.sub(r"\b%s\((\w+)\)" % name,
                          lambda m, p=param, b=body: "(" + re.sub(r"\b%s\b" % p, m.group(1), b) + ")", expr)
    expr = re.sub(r"\b(0x[0-9A-Fa-f]+|\d+)[uUlL]+\b", r"\1", expr)

    def name_value(m):
        word = m.group(0)
        if word not in objects:
            raise KeyError(word)
        return "(" + str(evaluate(objects[word], objects, functions, depth + 1)) + ")"

    expr = re.sub(r"\b[A-Za-z_]\w*\b", name_value, expr)
    return int(eval(expr, {"__builtins__": {}}))


def compare(ours, idf, idf_fn, names):
    bad = []
    for name in names:
        try:
            want = evaluate(COMPARE[name], idf, idf_fn)
        except KeyError as e:
            bad.append(f"{name}: ESP-IDF has no {e}")
            continue
        got = ours[name]
        if got != want:
            bad.append(f"{name}: 0x{got:x}, ESP-IDF says 0x{want:x} ({COMPARE[name]})")
    return bad


def main():
    root = idf_root()
    if root is None:
        print("esp32_regs: no ESP-IDF checkout (ESP_IDF_PATH or refs/esp-idf-v5.4.1): skipped")
        return SKIP
    soc = os.path.join(root, "components", "soc", CHIP)
    # The first definition of a name wins: soc.h's REG_TIMG_BASE(i), not the struct-based one a register
    # header may define after it. The watchdog key is in the LP watchdog's low-level driver.
    idf_paths = [os.path.join(soc, "include", "soc", "soc.h"),
                 os.path.join(root, "components", "hal", "include", "hal", "wdt_types.h"),
                 os.path.join(root, "components", "hal", CHIP, "include", "hal", "lpwdt_ll.h"),
                 os.path.join(root, "components", "hal", CHIP, "include", "hal", "clk_tree_ll.h"),
                 os.path.join(soc, "include", "soc", "soc_caps.h"),
                 os.path.join(soc, "include", "soc", "clk_tree_defs.h"),
                 os.path.join(soc, "include", "soc", "gpio_sig_map.h"),
                 os.path.join(root, "components", "hal", "include", "hal", "ieee802154_common_ll.h"),
                 os.path.join(root, "components", "hal", CHIP, "include", "hal", "apm_ll.h")]
    idf_paths += [os.path.join(soc, "register", "soc", f) for f in sorted(os.listdir(os.path.join(soc, "register", "soc")))
                  if f.endswith("_reg.h") or f == "reg_base.h"]
    modem = os.path.join(soc, "include", "modem")
    idf_paths += [os.path.join(modem, f) for f in sorted(os.listdir(modem)) if f.endswith(".h")]
    idf, idf_fn = collect(idf_paths)
    ld_dir = os.path.join(root, "components", "bootloader", "subproject", "main", "ld", CHIP)
    ld = os.path.join(ld_dir, "bootloader.memory.ld.in")      # its name from v5.5 on
    if not os.path.exists(ld):
        ld = os.path.join(ld_dir, "bootloader.ld")
    with open(ld) as f:
        m = re.search(r"ASSERT\(bootloader_iram_loader_seg_start == (0x[0-9A-Fa-f]+)", f.read())
    idf["BOOTLOADER_IRAM_LOADER_SEG_START"] = m.group(1)

    our_defs, our_fn = collect(OURS)
    ours = {}
    unlisted = []
    for name in our_defs:
        if not name.startswith("ESP32_") or name in NOT_COMPARED:
            continue
        if name not in COMPARE:
            unlisted.append(name)
            continue
        ours[name] = evaluate(our_defs[name], our_defs, our_fn)

    bad = compare(ours, idf, idf_fn, sorted(ours))
    bad += [f"{name}: neither compared nor listed in NOT_COMPARED" for name in unlisted]
    bad += [f"{name}: compared but not defined by the headers" for name in COMPARE if name not in ours]

    canary = dict(ours)
    canary["ESP32_SYSTIMER_INT_CLR"] ^= 4
    if not compare(canary, idf, idf_fn, ["ESP32_SYSTIMER_INT_CLR"]):
        bad.append("canary: a wrong offset passed the comparison")

    for line in bad:
        print("* FAIL* " + line)
    print(f"esp32_regs: {len(ours)} values compared with ESP-IDF ({os.path.relpath(root, REPO)}), "
          f"{len(bad)} failure(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
