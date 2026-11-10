/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* esp32_mac.c — the 802.15.4 MAC of the ESP32 parts: bring-up and receive (esp32_mac.h).

Written from scratch after reading ESP-IDF v5.4.1's ieee802154 driver and its register headers; the TRM
documents none of it (docs/hardware_esp32.md §12.6). Two things outside the MAC must already be done: its
clock domains running in the active power state (hal/esp32/phy/esp32_phy.c), and the access filters open
for DMA masters (hal_esp32.c). Without the first, the coexistence arbiter breaks the first receive off and
the MAC hears nothing after it; without the second, it takes frames in and writes none of them to RAM. What the MAC needs, in the order the driver does it:
its own reset in the modem domain, the events it may raise, which receive aborts raise one (the two an Ack
sequence ends with, and the coexistence arbiter's break, after which the MAC stays idle: every other abort
- a bad CRC, a preamble with no frame - it recovers from by itself and counts), the coexistence arbiter's priority set as if no other radio were there, the
receive-on delay and the baseband's transmit-on delay, then the channel, the DMA buffer and the command.

The receive buffer is written by the MAC's DMA: the PHR's length octet, then the PSDU, whose two FCS
octets are replaced by the frame's RSSI (signed dBm) and LQI - the MAC checks the CRC and does not keep
it. One buffer serves for now: the vector reports the frame and re-arms receive before it returns. */
#include <stdint.h>

#include "dbg.h"
#include "esp32_irq.h"
#include "esp32_mac.h"
#include "esp32_regs.h"
#include "hal.h"

#define MAC(offset) ESP32_REG(ESP32_MAC_BASE, (offset))

#define MAC_PRIORITY 2u
#define MAC_PSDU_MAX 127u
/* The PHR's length octet and the largest PSDU, rounded up to whole words for the DMA. */
#define MAC_RX_BUF 132u
/* The receive-on and transmit-on delays ESP-IDF programs on silicon, in microseconds. */
#define MAC_RXON_DELAY_US 50u
#define MAC_TXON_DELAY_US 50u
/* The coexistence priority that leaves the radio to the MAC alone (ESP-IDF's ieee802154_ll_disable_coex). */
#define MAC_PTI_ALONE 1u

/* In the archive (libbtbb.a). */
void bt_bb_set_zb_tx_on_delay(uint16_t us);

static uint8_t macRxBuf[MAC_RX_BUF] __attribute__((aligned(4)));
static volatile uint16_t macFrames;
static volatile uint16_t macAborts;

static void macRxArm(void)
{
    MAC(ESP32_MAC_RXDMA_ADDR) = (uint32_t)macRxBuf;
    MAC(ESP32_MAC_COMMAND) = ESP32_MAC_CMD_RX_START;
}

static void macReport(void)
{
    uint8_t len = macRxBuf[0];

    if (len < 3u || len > MAC_PSDU_MAX)
        return;

    /* The FCS octets' place: RSSI at len - 1, LQI at len, counted from the length octet. */
    Debug_Emit(DBG_COMP_RADIO, (uint16_t)(len | ((uint16_t)macRxBuf[len] << 8)), DBG_RADIO_RSN_ESP32_RX_LEN);
    Debug_Emit(DBG_COMP_RADIO, (uint16_t)(int16_t)(int8_t)macRxBuf[len - 1u], DBG_RADIO_RSN_ESP32_RX_RSSI);
    Debug_Emit(DBG_COMP_RADIO, (uint16_t)(macRxBuf[1] | ((uint16_t)macRxBuf[2] << 8)), DBG_RADIO_RSN_ESP32_RX_FCF);
    Debug_Emit(DBG_COMP_RADIO, macRxBuf[3], DBG_RADIO_RSN_ESP32_RX_SEQ);
}

static void macIsr(void);

__attribute__((section(".iram.text.esp32_mac"))) static void macIsr(void)
{
    uint32_t events = MAC(ESP32_MAC_EVENT_STATUS);

    MAC(ESP32_MAC_EVENT_STATUS) = events;

    if ((events & ESP32_MAC_EVENT_RX_DONE) != 0u)
    {
        macFrames++;
        macReport();
        macRxArm();
    }
    else if ((events & ESP32_MAC_EVENT_RX_ABORT) != 0u)
    {
        macAborts++;
        macRxArm();
    }
}

int Esp32Mac_Init(void)
{
    ESP32_REG(ESP32_MODEM_SYSCON_BASE, ESP32_MODEM_SYSCON_MODEM_RST_CONF) |= ESP32_MODEM_SYSCON_RST_ZBMAC;
    ESP32_REG(ESP32_MODEM_SYSCON_BASE, ESP32_MODEM_SYSCON_MODEM_RST_CONF) &= ~ESP32_MODEM_SYSCON_RST_ZBMAC;
    HAL_Dsb();

    MAC(ESP32_MAC_EVENT_EN) = ESP32_MAC_EVENT_RX_DONE | ESP32_MAC_EVENT_RX_ABORT;
    MAC(ESP32_MAC_EVENT_STATUS) = ESP32_MAC_EVENT_ALL;
    MAC(ESP32_MAC_RX_ABORT_EN) = (1u << (ESP32_MAC_RX_ABORT_TX_ACK_TIMEOUT - 1u))
                                 | (1u << (ESP32_MAC_RX_ABORT_TX_ACK_COEX_BREAK - 1u))
                                 | (1u << (ESP32_MAC_RX_ABORT_COEX_BREAK - 1u));
    MAC(ESP32_MAC_COEX_PTI) = (MAC_PTI_ALONE << ESP32_MAC_COEX_PTI_SHIFT) | (MAC_PTI_ALONE << ESP32_MAC_COEX_ACK_PTI_SHIFT);
    MAC(ESP32_MAC_RXON_DELAY) = MAC_RXON_DELAY_US;
    bt_bb_set_zb_tx_on_delay(MAC_TXON_DELAY_US);

    MAC(ESP32_MAC_CTRL_CFG) = (MAC(ESP32_MAC_CTRL_CFG) & ~(ESP32_MAC_CTRL_AUTO_ACK_TX | ESP32_MAC_CTRL_AUTO_ACK_RX))
                              | ESP32_MAC_CTRL_PROMISCUOUS;

    return Esp32Irq_Route(ESP32_SRC_ZB_MAC, ESP32_LINE_RADIO, MAC_PRIORITY, macIsr);
}

int Esp32Mac_RxStart(uint8_t channel)
{
    if (channel < 11u || channel > 26u)
        return 0;

    MAC(ESP32_MAC_COMMAND) = ESP32_MAC_CMD_STOP;
    MAC(ESP32_MAC_CHANNEL) = (3u + 5u * (uint32_t)(channel - 11u)) & ESP32_MAC_CHANNEL_HOP_MASK;
    macRxArm();
    Debug_Emit(DBG_COMP_RADIO, channel, DBG_RADIO_RSN_ESP32_RX_ON);
    return 1;
}

void Esp32Mac_Witnesses(void)
{
    Debug_Emit(DBG_COMP_WITNESS, macFrames, DBG_WITNESS_RSN_ESP32_MAC_FRAMES);
    Debug_Emit(DBG_COMP_WITNESS, macAborts, DBG_WITNESS_RSN_ESP32_MAC_ABORTS);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)MAC(ESP32_MAC_CRC_ERROR_CNT), DBG_WITNESS_RSN_ESP32_MAC_CRC_ERRORS);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)MAC(ESP32_MAC_SFD_TIMEOUT_CNT), DBG_WITNESS_RSN_ESP32_MAC_SFD_TIMEOUTS);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)MAC(ESP32_MAC_NO_RSS_DETECT_CNT), DBG_WITNESS_RSN_ESP32_MAC_NO_RSS);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)MAC(ESP32_MAC_RX_STATUS), DBG_WITNESS_RSN_ESP32_MAC_RX_STATUS);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)(MAC(ESP32_MAC_RX_STATUS) >> 16), DBG_WITNESS_RSN_ESP32_MAC_RX_STATE);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)MAC(ESP32_MAC_TXRX_STATUS), DBG_WITNESS_RSN_ESP32_MAC_TXRX_STATUS);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)MAC(ESP32_MAC_RX_ABORT_COEX_CNT), DBG_WITNESS_RSN_ESP32_MAC_COEX_ABORTS);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)MAC(ESP32_MAC_CTRL_CFG), DBG_WITNESS_RSN_ESP32_MAC_CTRL);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)(MAC(ESP32_MAC_CTRL_CFG) >> 16), DBG_WITNESS_RSN_ESP32_MAC_CTRL_HI);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)MAC(ESP32_MAC_RX_LENGTH), DBG_WITNESS_RSN_ESP32_MAC_RX_LENGTH);
}
