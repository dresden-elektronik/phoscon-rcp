/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* esp32_mac.c — the 802.15.4 MAC of the ESP32 parts: bring-up, receive, transmit, Acks and energy
 * detection (esp32_mac.h).

Written from scratch after reading ESP-IDF v5.4.1's ieee802154 driver and its register headers; the TRM
documents none of it (docs/hardware_esp32.md §12.6). Two things outside the MAC must already be done: its
clock domains running in the active power state (hal/esp32/phy/esp32_phy.c), and the access filters open
for DMA masters (hal_esp32.c). Without the first the coexistence arbiter breaks the first receive off and
the MAC hears nothing after it; without the second it takes frames in and writes none of them to RAM.

Bring-up, in the order the driver does it: the MAC's reset in the modem domain; the events it may raise;
which aborts raise one - on receive the two an Ack sequence ends with and the coexistence break, after
which the MAC stays idle (a bad CRC or a preamble with no frame it recovers from by itself and counts); on
transmit a missing Ack, a coexistence break, a security error and a busy or failed CCA; the coexistence
priorities set as if no other radio were there; the receive-on and transmit-on delays; energy-based CCA
at ESP-IDF's default threshold.

A buffer the MAC's DMA reads or writes holds the PHR's length octet, then the PSDU. On receive the two FCS
octets' places carry the frame's RSSI (signed dBm) and LQI: the MAC checks the CRC itself and keeps none.
On transmit the MAC appends the FCS, so the length counts two octets that are not in the buffer.

The machine is one of five states, changed only here, in the vector or with interrupts locked:
receiving; transmitting; waiting for the Ack of what was sent, which the MAC receives by itself into the
receive buffer when its hardware Ack reception is on; sending the Ack of what was received; measuring
energy. Every end goes back to receiving. With its hardware Ack transmission on, the MAC acknowledges a
2003 or 2006 frame that passes its address filter and asks for one, by itself, inside the turnaround; it
must not be told to receive again until the Ack is out - re-armed at the frame's end, it sends the Ack and
then idles - so the vector waits for ACK_TX_DONE, or for the two aborts that end an Ack. */
#include <stdint.h>

#include "dbg.h"
#include "esp32_irq.h"
#include "esp32_mac.h"
#include "esp32_regs.h"
#include "hal.h"

#define MAC(offset) ESP32_REG(ESP32_MAC_BASE, (offset))

#define MAC_PRIORITY 2u
#define MAC_PSDU_MAX 127u
#define MAC_FCS_LEN 2u
/* The PHR's length octet and the largest PSDU, rounded up to whole words for the DMA. */
#define MAC_BUF 132u
/* The receive-on and transmit-on delays ESP-IDF programs on silicon, in microseconds. */
#define MAC_RXON_DELAY_US 50u
#define MAC_TXON_DELAY_US 50u
/* The coexistence priority that leaves the radio to the MAC alone (ESP-IDF's ieee802154_ll_disable_coex). */
#define MAC_PTI_ALONE 1u
/* ESP-IDF's defaults: energy-based CCA at -60 dBm, sampled over 8 symbols (128 us, 802.15.4's own). */
#define MAC_CCA_THRESHOLD_DBM (-60)
#define MAC_CCA_SYMBOLS 8u
/* The frame control field's Ack Request and Frame Pending bits, and the Ack's frame type. */
#define MAC_FCF_FRAME_PENDING 0x10u
#define MAC_FCF_ACK_REQUEST 0x20u
/* The frame version, FCF [13:12]: the MAC acknowledges 2003 and 2006 frames itself (0 and 1), not 2015. */
#define MAC_FCF_VERSION_SHIFT 12u
#define MAC_FCF_VERSION_2006 1u

/* In the archive (libbtbb.a). */
void bt_bb_set_zb_tx_on_delay(uint16_t us);

typedef enum
{
    MAC_RX = 0,
    MAC_TX,
    MAC_TX_WAIT_ACK,
    MAC_RX_SEND_ACK,
    MAC_ED
} Mac_State;

static uint8_t macRxBuf[MAC_BUF] __attribute__((aligned(4)));
static uint8_t macTxBuf[MAC_BUF] __attribute__((aligned(4)));
static volatile Mac_State macState;
static volatile uint16_t macFrames;
static volatile uint16_t macAborts;
static volatile uint16_t macTxDone;
static volatile uint16_t macAcksSent;

static void macRxArm(void)
{
    macState = MAC_RX;
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

/* 1 when the MAC is acknowledging the frame just received by itself: it asks for an Ack, is a 2003 or 2006
 * frame, and hardware Ack transmission is on (which also means the address filter passed it). */
static int macAcksItself(void)
{
    uint16_t fcf = (uint16_t)(macRxBuf[1] | ((uint16_t)macRxBuf[2] << 8));

    return (fcf & MAC_FCF_ACK_REQUEST) != 0u && ((fcf >> MAC_FCF_VERSION_SHIFT) & 3u) <= MAC_FCF_VERSION_2006
           && (MAC(ESP32_MAC_CTRL_CFG) & ESP32_MAC_CTRL_AUTO_ACK_TX) != 0u;
}

/* How a transmission ended, with the sequence number of what was sent. */
static void macTxEnded(uint8_t how)
{
    macTxDone++;
    Debug_Emit(DBG_COMP_RADIO, (uint16_t)(macTxBuf[3] | ((uint16_t)how << 8)), DBG_RADIO_RSN_ESP32_TX_DONE);
}

static void macIsr(void);

__attribute__((section(".iram.text.esp32_mac"))) static void macIsr(void)
{
    uint32_t events = MAC(ESP32_MAC_EVENT_STATUS);

    MAC(ESP32_MAC_EVENT_STATUS) = events;

    switch (macState)
    {
    case MAC_RX_SEND_ACK:
        if ((events & ESP32_MAC_EVENT_ACK_TX_DONE) != 0u)
        {
            macAcksSent++;
            Debug_Emit(DBG_COMP_RADIO, macRxBuf[3], DBG_RADIO_RSN_ESP32_ACK_SENT);
            macRxArm();
        }
        else if ((events & ESP32_MAC_EVENT_RX_ABORT) != 0u)
        {
            macAborts++;
            macRxArm();
        }
        break;

    case MAC_TX:
        if ((events & ESP32_MAC_EVENT_TX_ABORT) != 0u)
        {
            Debug_Emit(DBG_COMP_RADIO,
                       (uint16_t)((MAC(ESP32_MAC_TX_STATUS) >> ESP32_MAC_TX_ABORT_STATUS_SHIFT)
                                  & ESP32_MAC_TX_ABORT_STATUS_MASK),
                       DBG_RADIO_RSN_ESP32_TX_ABORT);
            macTxEnded(3u);
            macRxArm();
        }
        else if ((events & ESP32_MAC_EVENT_TX_DONE) != 0u)
        {
            if ((macTxBuf[1] & MAC_FCF_ACK_REQUEST) != 0u)
            {
                macState = MAC_TX_WAIT_ACK;
            }
            else
            {
                macTxEnded(0u);
                macRxArm();
            }
        }
        break;

    case MAC_TX_WAIT_ACK:
        if ((events & ESP32_MAC_EVENT_ACK_RX_DONE) != 0u)
        {
            macTxEnded(((macRxBuf[1] & MAC_FCF_FRAME_PENDING) != 0u) ? 2u : 1u);
            macRxArm();
        }
        else if ((events & ESP32_MAC_EVENT_TX_ABORT) != 0u)
        {
            Debug_Emit(DBG_COMP_RADIO,
                       (uint16_t)((MAC(ESP32_MAC_TX_STATUS) >> ESP32_MAC_TX_ABORT_STATUS_SHIFT)
                                  & ESP32_MAC_TX_ABORT_STATUS_MASK),
                       DBG_RADIO_RSN_ESP32_TX_ABORT);
            macTxEnded(3u);
            macRxArm();
        }
        break;

    case MAC_ED:
        if ((events & ESP32_MAC_EVENT_ED_DONE) != 0u)
        {
            uint32_t cfg = MAC(ESP32_MAC_ED_SCAN_CFG);

            Debug_Emit(DBG_COMP_RADIO, (uint16_t)(int16_t)(int8_t)(cfg >> ESP32_MAC_ED_RSS_SHIFT),
                       DBG_RADIO_RSN_ESP32_ED);
            Debug_Emit(DBG_COMP_RADIO, (uint16_t)((cfg & ESP32_MAC_ED_CCA_BUSY) != 0u),
                       DBG_RADIO_RSN_ESP32_CCA_BUSY);
            macRxArm();
        }
        break;

    case MAC_RX:
    default:
        if ((events & ESP32_MAC_EVENT_RX_DONE) != 0u)
        {
            macFrames++;
            macReport();

            if (macAcksItself())
                macState = MAC_RX_SEND_ACK;
            else
                macRxArm();
        }
        else if ((events & ESP32_MAC_EVENT_RX_ABORT) != 0u)
        {
            macAborts++;
            macRxArm();
        }
        break;
    }
}

int Esp32Mac_Init(void)
{
    ESP32_REG(ESP32_MODEM_SYSCON_BASE, ESP32_MODEM_SYSCON_MODEM_RST_CONF) |= ESP32_MODEM_SYSCON_RST_ZBMAC;
    ESP32_REG(ESP32_MODEM_SYSCON_BASE, ESP32_MODEM_SYSCON_MODEM_RST_CONF) &= ~ESP32_MODEM_SYSCON_RST_ZBMAC;
    HAL_Dsb();

    MAC(ESP32_MAC_EVENT_EN) = ESP32_MAC_EVENT_TX_DONE | ESP32_MAC_EVENT_RX_DONE | ESP32_MAC_EVENT_ACK_TX_DONE
                              | ESP32_MAC_EVENT_ACK_RX_DONE | ESP32_MAC_EVENT_RX_ABORT | ESP32_MAC_EVENT_TX_ABORT
                              | ESP32_MAC_EVENT_ED_DONE;
    MAC(ESP32_MAC_EVENT_STATUS) = ESP32_MAC_EVENT_ALL;
    MAC(ESP32_MAC_RX_ABORT_EN) = (1u << (ESP32_MAC_RX_ABORT_TX_ACK_TIMEOUT - 1u))
                                 | (1u << (ESP32_MAC_RX_ABORT_TX_ACK_COEX_BREAK - 1u))
                                 | (1u << (ESP32_MAC_RX_ABORT_COEX_BREAK - 1u));
    MAC(ESP32_MAC_TX_ABORT_EN) = (1u << (ESP32_MAC_TX_ABORT_RX_ACK_TIMEOUT - 1u))
                                 | (1u << (ESP32_MAC_TX_ABORT_TX_COEX_BREAK - 1u))
                                 | (1u << (ESP32_MAC_TX_ABORT_TX_SECURITY_ERROR - 1u))
                                 | (1u << (ESP32_MAC_TX_ABORT_CCA_FAILED - 1u))
                                 | (1u << (ESP32_MAC_TX_ABORT_CCA_BUSY - 1u));
    MAC(ESP32_MAC_COEX_PTI) = (MAC_PTI_ALONE << ESP32_MAC_COEX_PTI_SHIFT) | (MAC_PTI_ALONE << ESP32_MAC_COEX_ACK_PTI_SHIFT);
    MAC(ESP32_MAC_RXON_DELAY) = MAC_RXON_DELAY_US;
    bt_bb_set_zb_tx_on_delay(MAC_TXON_DELAY_US);

    MAC(ESP32_MAC_ED_SCAN_CFG) = ((uint32_t)(uint8_t)(int8_t)MAC_CCA_THRESHOLD_DBM & ESP32_MAC_ED_CCA_THRESHOLD_MASK)
                                 | (ESP32_MAC_ED_SAMPLE_AVG << ESP32_MAC_ED_SAMPLE_MODE_SHIFT)
                                 | (ESP32_MAC_ED_CCA_MODE_ED << ESP32_MAC_ED_CCA_MODE_SHIFT);

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

void Esp32Mac_SetAddress(uint16_t panId, uint16_t shortAddr, int filtered)
{
    uint32_t ctrl;

    MAC(ESP32_MAC_INF0_PAN_ID) = panId;
    MAC(ESP32_MAC_INF0_SHORT_ADDR) = shortAddr;

    ctrl = MAC(ESP32_MAC_CTRL_CFG) | ESP32_MAC_CTRL_INF0_ENABLE;

    if (filtered)
        ctrl = (ctrl & ~ESP32_MAC_CTRL_PROMISCUOUS) | ESP32_MAC_CTRL_AUTO_ACK_TX | ESP32_MAC_CTRL_AUTO_ACK_RX;
    else
        ctrl = (ctrl | ESP32_MAC_CTRL_PROMISCUOUS) & ~(ESP32_MAC_CTRL_AUTO_ACK_TX | ESP32_MAC_CTRL_AUTO_ACK_RX);

    MAC(ESP32_MAC_CTRL_CFG) = ctrl;
}

void Esp32Mac_SetPower(int8_t dbm)
{
    uint32_t index;

    if (dbm >= 20)
        index = 15u;
    else if (dbm <= -15)
        index = 3u;
    else
        index = 3u + (uint32_t)(dbm + 15) / 3u;

    MAC(ESP32_MAC_TX_POWER) = index & ESP32_MAC_TX_POWER_MASK;
}

int Esp32Mac_Tx(const uint8_t *psdu, uint8_t len, int cca)
{
    uint8_t i;

    if (psdu == 0 || len < 3u || len + MAC_FCS_LEN > MAC_PSDU_MAX || macState != MAC_RX)
        return 0;

    HAL_IrqLock();
    MAC(ESP32_MAC_COMMAND) = ESP32_MAC_CMD_STOP;
    macTxBuf[0] = (uint8_t)(len + MAC_FCS_LEN);

    for (i = 0u; i < len; ++i)
        macTxBuf[1u + i] = psdu[i];

    MAC(ESP32_MAC_TXDMA_ADDR) = (uint32_t)macTxBuf;
    /* An Ack, if one comes, lands in the receive buffer. */
    MAC(ESP32_MAC_RXDMA_ADDR) = (uint32_t)macRxBuf;
    macState = MAC_TX;

    if (cca)
    {
        MAC(ESP32_MAC_ED_SCAN_DURATION) = MAC_CCA_SYMBOLS;
        MAC(ESP32_MAC_COMMAND) = ESP32_MAC_CMD_CCA_TX_START;
    }
    else
    {
        MAC(ESP32_MAC_COMMAND) = ESP32_MAC_CMD_TX_START;
    }

    HAL_IrqUnlock();
    return 1;
}

int Esp32Mac_EdStart(uint32_t symbols)
{
    if (macState != MAC_RX)
        return 0;

    HAL_IrqLock();
    MAC(ESP32_MAC_COMMAND) = ESP32_MAC_CMD_STOP;
    macState = MAC_ED;
    MAC(ESP32_MAC_ED_SCAN_DURATION) = symbols;
    MAC(ESP32_MAC_COMMAND) = ESP32_MAC_CMD_ED_START;
    HAL_IrqUnlock();
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
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)MAC(ESP32_MAC_RX_ABORT_COEX_CNT), DBG_WITNESS_RSN_ESP32_MAC_COEX_ABORTS);
}

#ifdef DS_ESP32_TX_PROBE
/* The bench step of a transmit probe image, once per heartbeat: an Ack-requesting data frame with CCA to
 * the peer on even beats, an energy measurement of 8 symbols on odd ones. */
#define MAC_PROBE_PAN 0xBEE5u
#define MAC_PROBE_SELF 0x5C6Cu
#define MAC_PROBE_PEER 0x1234u

void Esp32Mac_ProbeStep(void)
{
    static uint8_t beat;
    static uint8_t seq;
    uint8_t frame[16];
    uint8_t n = 0u;

    if ((beat++ & 1u) != 0u)
    {
        (void)Esp32Mac_EdStart(MAC_CCA_SYMBOLS);
        return;
    }

    /* Data, Ack Request, PAN ID compression, 2006 frame version, short addresses both ways. */
    frame[n++] = 0x61u;
    frame[n++] = 0x98u;
    frame[n++] = seq++;
    frame[n++] = (uint8_t)MAC_PROBE_PAN;
    frame[n++] = (uint8_t)(MAC_PROBE_PAN >> 8);
    frame[n++] = (uint8_t)MAC_PROBE_PEER;
    frame[n++] = (uint8_t)(MAC_PROBE_PEER >> 8);
    frame[n++] = (uint8_t)MAC_PROBE_SELF;
    frame[n++] = (uint8_t)(MAC_PROBE_SELF >> 8);
    frame[n++] = 'x';
    frame[n++] = 'c';
    frame[n++] = '6';
    (void)Esp32Mac_Tx(frame, n, 1);
}

void Esp32Mac_ProbeInit(void)
{
    Esp32Mac_SetAddress(MAC_PROBE_PAN, MAC_PROBE_SELF, 1);
    Esp32Mac_SetPower(0);
}
#endif
