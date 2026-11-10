/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* esp32_radio.c — hal/radio.h on the 802.15.4 MAC of the ESP32 parts (docs/hardware_esp32.md §12.6).

Written from scratch after reading ESP-IDF v5.4.1's ieee802154 driver and its register headers; the TRM
documents none of it. Radio_Init brings the PHY up first (hal/esp32/phy/), and two things outside the MAC
must already be done by then: its clock domains running in the active power state (the PHY bring-up), and
the access filters open for DMA masters (hal_esp32.c).

What the MAC does by itself and what this file adds:

  receive       the MAC filters, receives by DMA and acknowledges a 2003/2006 frame that asks for an Ack,
                inside the turnaround. The vector copies the frame into the ring and decides the Ack's
                Frame Pending bit from the source-match table before the Ack goes out; it re-arms receive
                only once the Ack is out (ACK_TX_DONE), because a MAC told to receive while it acknowledges
                sends the Ack and then idles. While the ring is full the hardware Ack is off, so a frame
                the ring cannot take is not acknowledged and its sender retries (docs/architecture.md §3).
  FCS           the MAC checks the CRC and keeps none: the two octets' places carry RSSI and LQI. The loop
                computes the FCS again before it hands the frame up, so a host sees the PSDU it would see
                from any other board.
  transmit      the MAC does one CCA, sends, and with hardware Ack reception on waits for the Ack. CSMA-CA
                and retries are this file's: 802.15.4's unslotted backoff (macMinBE 3, macMaxBE 5,
                macMaxCSMABackoffs 4) on the MAC's own microsecond timer, each attempt started from the
                vector, and the next try after a missing Ack.
  energy        one measurement of 8 symbols at a time, on the channel the MAC holds.

The machine's state changes only in the vector or with interrupts locked. The loop takes what the vector
left: received frames, and the end of a transmission. */
#include <stdint.h>

#include "dbg.h"
#include "esp32_irq.h"
#include "esp32_phy.h"
#include "esp32_regs.h"
#include "hal.h"
#include "mac.h"
#include "radio.h"
#include "srcmatch.h"

#define MAC(offset) ESP32_REG(ESP32_MAC_BASE, (offset))

#define RADIO_PRIORITY 2u
#define RADIO_FCS_LEN 2u
/* The PHR's length octet and the largest PSDU, rounded up to whole words for the DMA. */
#define RADIO_BUF 132u
/* The receive-on and transmit-on delays ESP-IDF programs on silicon, in microseconds. */
#define RADIO_RXON_DELAY_US 50u
#define RADIO_TXON_DELAY_US 50u
/* The coexistence priority that leaves the radio to the MAC alone (ESP-IDF's ieee802154_ll_disable_coex). */
#define RADIO_PTI_ALONE 1u
/* ESP-IDF's defaults: energy-based CCA at -60 dBm over 8 symbols, 802.15.4's own 128 us. */
#define RADIO_CCA_DEFAULT_DBM (-60)
#define RADIO_CCA_SYMBOLS 8u
#define RADIO_ED_SYMBOLS 8u
/* 802.15.4's unslotted CSMA-CA: the backoff period is 20 symbols. */
#define RADIO_BACKOFF_US 320u
#define RADIO_MIN_BE 3u
#define RADIO_MAX_BE 5u
#define RADIO_MAX_BACKOFFS 4u
#define RADIO_MAX_RETRIES 15
/* A stop takes the MAC a few cycles; a command given before it is idle is ignored, and no event follows. */
#define RADIO_STOP_SPINS 2000u
/* The bound on a transmission, every try and backoff included: 15 retries of a 127-octet frame with the
 * longest backoffs and Ack waits stay below 400 ms. Past it the transmission ends as TIMEOUT. */
#define RADIO_TX_BOUND_MS 500u
/* The frame control field's bits this file reads. */
#define RADIO_FCF_FRAME_PENDING 0x10u
#define RADIO_FCF_ACK_REQUEST 0x20u
#define RADIO_FCF_VERSION_SHIFT 12u
#define RADIO_FCF_VERSION_2006 1u
/* The MAC's transmit power index: 15 is +20 dBm, else 3 + (dBm + 15) / 3 from -15 dBm (ESP-IDF's mapping). */
#define RADIO_POWER_MIN_DBM (-15)
#define RADIO_POWER_MAX_DBM 20

/* In the archive (libbtbb.a). */
void bt_bb_set_zb_tx_on_delay(uint16_t us);

typedef enum
{
    RADIO_ST_DOWN = 0,
    RADIO_ST_IDLE,      /* the receiver off */
    RADIO_ST_RX,        /* receiving */
    RADIO_ST_SEND_ACK,  /* the MAC acknowledges what it received */
    RADIO_ST_TX,        /* a transmission is on the air, CCA first when asked */
    RADIO_ST_WAIT_ACK,  /* the Ack of what was sent is awaited */
    RADIO_ST_ED         /* an energy measurement runs */
} Radio_State;

static uint8_t radioRxBuf[RADIO_BUF] __attribute__((aligned(4)));
static uint8_t radioTxBuf[RADIO_BUF] __attribute__((aligned(4)));

static volatile Radio_State radioState;
static int radioRxWanted;
static uint8_t radioChannel = RADIO_CHAN_MIN;
static int8_t radioPowerDbm;
static int radioFilter = RADIO_RX_FILTERED;
static SrcMatch_Table radioSrcMatch;

static Radio_Frame radioRing[RADIO_RX_SLOTS];
static volatile uint8_t radioRingHead;
static uint8_t radioRingTail;

/* The transmission in flight, from Radio_Tx to its end: during its backoff the MAC keeps receiving and
 * acknowledging with the timer armed, and a backoff that expires while an Ack goes out starts the attempt
 * once the Ack is out. Its token, CSMA and retry state, and its end for the loop to report. */
static volatile uint8_t radioTxPending;
static volatile uint8_t radioTxDue;
static uint32_t radioTxStartMs;
static volatile uint16_t radioTxStuck;
static void *radioTxToken;
static uint8_t radioTxCca;
static uint8_t radioTxNb;
static uint8_t radioTxBe;
static int radioTxRetriesLeft;
static volatile uint8_t radioTxEnded;
static volatile uint8_t radioTxStatus;
static volatile uint8_t radioTxFramePending;

static volatile int radioEdState = RADIO_ED_IDLE;
static volatile int8_t radioEdDbm;

static void (*radioRxHandler)(const Radio_Frame *frame);
static void (*radioTxDoneHandler)(void *token, int status, int framePending);

static volatile Radio_Counters radioCounters;

static uint64_t radioClockNow(void);

/* ---- the MAC's commands ---- */

/* Stop whatever the MAC does, and wait, bounded, until it is idle. */
static void radioStop(void)
{
    uint32_t spins;

    MAC(ESP32_MAC_COMMAND) = ESP32_MAC_CMD_STOP;

    for (spins = 0u; spins < RADIO_STOP_SPINS; ++spins)
    {
        if ((MAC(ESP32_MAC_TXRX_STATUS) & ESP32_MAC_TXRX_STATE_MASK) == 0u)
            break;
    }
}

static void radioRxArm(void)
{
    radioState = RADIO_ST_RX;
    MAC(ESP32_MAC_RXDMA_ADDR) = (uint32_t)radioRxBuf;
    MAC(ESP32_MAC_COMMAND) = ESP32_MAC_CMD_RX_START;
}

/* Back to what the host asked for: receiving, or the receiver off. A MAC that is receiving takes a new
 * channel only from a fresh receive command after a stop. */
static void radioToWish(void)
{
    radioStop();

    if (radioRxWanted)
        radioRxArm();
    else
        radioState = RADIO_ST_IDLE;
}

/* The Frame Pending bit of the next hardware Ack: bit 0 alone. The same register's upper half is the MAC's
 * Ack-transmit timeout, and a whole-word write that clears it makes every hardware Ack time out. */
static void radioSetAckPending(uint8_t pending)
{
    uint32_t cfg = MAC(ESP32_MAC_ACK_FRAME_PENDING_EN) & ~ESP32_MAC_ACK_FRAME_PENDING;

    MAC(ESP32_MAC_ACK_FRAME_PENDING_EN) = cfg | (pending ? ESP32_MAC_ACK_FRAME_PENDING : 0u);
}

static int radioRingRoom(void)
{
    return (uint8_t)(radioRingHead - radioRingTail) < RADIO_RX_SLOTS;
}

/* The hardware Ack follows the mode, and in the filtered mode whether the ring has room. The register is
 * written only when the bit changes: this runs in the vector at a frame's end, inside the Ack's turnaround,
 * and a write to the MAC's configuration there loses the Ack. */
static void radioAckFollowsRoom(void)
{
    uint32_t ctrl = MAC(ESP32_MAC_CTRL_CFG);
    uint32_t want = ctrl & ~ESP32_MAC_CTRL_AUTO_ACK_TX;

    if (radioFilter == RADIO_RX_FILTERED && radioRingRoom())
        want |= ESP32_MAC_CTRL_AUTO_ACK_TX;

    if (want != ctrl)
        MAC(ESP32_MAC_CTRL_CFG) = want;
}

/* A pseudo-random backoff count below 2^be, from the chip's random number generator. */
static uint32_t radioBackoffSlots(uint8_t be)
{
    return ESP32_REG(ESP32_LPPERI_BASE, ESP32_LPPERI_RNG_DATA) & ((1u << be) - 1u);
}

static void radioTxStartNow(void)
{
    radioStop();
    MAC(ESP32_MAC_TXDMA_ADDR) = (uint32_t)radioTxBuf;
    /* An Ack, if one comes, lands in the receive buffer. */
    MAC(ESP32_MAC_RXDMA_ADDR) = (uint32_t)radioRxBuf;
    radioState = RADIO_ST_TX;

    if (radioTxCca)
    {
        MAC(ESP32_MAC_ED_SCAN_DURATION) = RADIO_CCA_SYMBOLS;
        MAC(ESP32_MAC_COMMAND) = ESP32_MAC_CMD_CCA_TX_START;
    }
    else
    {
        MAC(ESP32_MAC_COMMAND) = ESP32_MAC_CMD_TX_START;
    }
}

/* One attempt: after its backoff when CCA is asked for, at once when not. The MAC keeps receiving while
 * the backoff runs. */
static void radioTxAttempt(void)
{
    uint32_t wait;

    if (!radioTxCca)
    {
        radioTxStartNow();
        return;
    }

    wait = radioBackoffSlots(radioTxBe) * RADIO_BACKOFF_US;

    if (wait == 0u)
    {
        radioTxStartNow();
        return;
    }

    MAC(ESP32_MAC_TIMER0_THRESHOLD) = wait;
    MAC(ESP32_MAC_COMMAND) = ESP32_MAC_CMD_TIMER0_START;
}

static void radioTxEnd(uint8_t status, uint8_t framePending)
{
    radioTxPending = 0u;
    radioTxStatus = status;
    radioTxFramePending = framePending;
    radioTxEnded = 1u;
    radioToWish();
}

/* A busy channel: back off again with a larger exponent, or give up after macMaxCSMABackoffs. */
static void radioTxChannelBusy(void)
{
    radioTxNb++;

    if (radioTxNb > RADIO_MAX_BACKOFFS)
    {
        radioTxEnd(RADIO_TX_CCA_FAIL, 0u);
        return;
    }

    if (radioTxBe < RADIO_MAX_BE)
        radioTxBe++;

    radioTxAttempt();
}

/* No Ack: the next try from a fresh CSMA, or NO_ACK once the retries are spent. */
static void radioTxNoAck(void)
{
    if (radioTxRetriesLeft <= 0)
    {
        radioTxEnd(RADIO_TX_NO_ACK, 0u);
        return;
    }

    radioTxRetriesLeft--;
    radioTxNb = 0u;
    radioTxBe = RADIO_MIN_BE;
    radioTxAttempt();
}

/* ---- receive, in the vector ---- */

/* The Ack's Frame Pending bit for the frame just received, decided before the MAC sends the Ack: a Data
 * Request gets the table's answer, or "pending" while the table is off; anything else gets 0
 * (docs/ack-pending-frame.md). */
static uint8_t radioPendingFor(const uint8_t *psdu, uint8_t len)
{
    Mac_Hdr hdr;

    if (!Mac_ParseHeader(psdu, len, &hdr) || !Mac_IsDataRequest(&hdr, psdu, len))
        return 0u;

    if (!radioSrcMatch.enabled)
        return 1u;

    if (hdr.srcMode == 2u)
        return (uint8_t)SrcMatch_HasShort(&radioSrcMatch, hdr.srcShort);

    if (hdr.srcMode == 3u)
        return (uint8_t)SrcMatch_HasExt(&radioSrcMatch, hdr.srcExt);

    return 0u;
}

/* 1 when the MAC acknowledges the frame just received by itself: it asks for an Ack, is a 2003 or 2006
 * frame, and hardware Ack transmission is on (which also means the address filter passed it). */
static int radioAcksItself(void)
{
    uint16_t fcf = (uint16_t)(radioRxBuf[1] | ((uint16_t)radioRxBuf[2] << 8));

    return (fcf & RADIO_FCF_ACK_REQUEST) != 0u && ((fcf >> RADIO_FCF_VERSION_SHIFT) & 3u) <= RADIO_FCF_VERSION_2006
           && (MAC(ESP32_MAC_CTRL_CFG) & ESP32_MAC_CTRL_AUTO_ACK_TX) != 0u;
}

static void radioRxDone(void)
{
    uint8_t len = radioRxBuf[0];
    int acks = radioAcksItself();
    uint8_t pending = 0u;
    Radio_Frame *f;
    uint8_t i;

    if (acks)
    {
        pending = radioPendingFor(&radioRxBuf[1], len);
        radioSetAckPending(pending);

        if (pending)
            radioCounters.ackFp++;
    }

    if (len < 3u || len > RADIO_PSDU_MAX)
    {
        radioCounters.rxFiltered++;
    }
    else if (!radioRingRoom())
    {
        radioCounters.rxDropped++;
        radioCounters.rxRingFull++;
    }
    else
    {
        f = &radioRing[radioRingHead % RADIO_RX_SLOTS];

        for (i = 0u; i < (uint8_t)(len - RADIO_FCS_LEN); ++i)
            f->psdu[i] = radioRxBuf[1u + i];

        f->len = len;
        f->rssi = (int8_t)radioRxBuf[len - 1u];
        f->lqi = radioRxBuf[len];
        f->channel = radioChannel;
        f->timestamp = radioClockNow();
        f->crcOk = 1;
        f->ackedFp = (int)pending;
        f->ackedSec = 0;
        f->ackKeyIndex = 0u;
        f->ackFrameCounter = 0u;
        radioRingHead = (uint8_t)(radioRingHead + 1u);
        radioAckFollowsRoom();
    }

    if (acks)
        radioState = RADIO_ST_SEND_ACK;
    else
        radioToWish();
}

static void radioIsr(void);

__attribute__((section(".iram.text.esp32_radio"))) static void radioIsr(void)
{
    uint32_t events = MAC(ESP32_MAC_EVENT_STATUS);
    uint32_t reason;

    MAC(ESP32_MAC_EVENT_STATUS) = events;

    /* A backoff's end: the attempt starts now, or once the Ack going out is done. */
    if ((events & ESP32_MAC_EVENT_TIMER0_OVERFLOW) != 0u && radioTxPending)
    {
        MAC(ESP32_MAC_COMMAND) = ESP32_MAC_CMD_TIMER0_STOP;

        if (radioState == RADIO_ST_SEND_ACK)
            radioTxDue = 1u;
        else
            radioTxStartNow();

        return;
    }

    switch (radioState)
    {
    case RADIO_ST_RX:
        if ((events & ESP32_MAC_EVENT_RX_DONE) != 0u)
            radioRxDone();
        else if ((events & ESP32_MAC_EVENT_RX_ABORT) != 0u)
            radioToWish();
        break;

    case RADIO_ST_SEND_ACK:
        if ((events & (ESP32_MAC_EVENT_ACK_TX_DONE | ESP32_MAC_EVENT_RX_ABORT)) != 0u)
        {
            if ((events & ESP32_MAC_EVENT_ACK_TX_DONE) != 0u)
                radioCounters.ackSent++;
            else
                radioCounters.ackLate++;

            if (radioTxDue)
            {
                radioTxDue = 0u;
                radioTxStartNow();
            }
            else
            {
                radioToWish();
            }
        }
        break;

    case RADIO_ST_TX:
        /* One pass can carry the frame's end and the Ack's, or the frame's end and the Ack's timeout: the
         * events are taken in the order they happened. */
        if ((events & ESP32_MAC_EVENT_TX_DONE) == 0u)
        {
            if ((events & ESP32_MAC_EVENT_TX_ABORT) != 0u)
            {
                reason = (MAC(ESP32_MAC_TX_STATUS) >> ESP32_MAC_TX_ABORT_STATUS_SHIFT) & ESP32_MAC_TX_ABORT_STATUS_MASK;

                if (reason == ESP32_MAC_TX_ABORT_CCA_BUSY || reason == ESP32_MAC_TX_ABORT_CCA_FAILED)
                    radioTxChannelBusy();
                else
                    radioTxEnd(RADIO_TX_TIMEOUT, 0u);
            }
            break;
        }

        if ((radioTxBuf[1] & RADIO_FCF_ACK_REQUEST) == 0u)
        {
            radioTxEnd(RADIO_TX_OK, 0u);
            break;
        }

        /* The Ack, or its timeout, may be in this pass too. */
        radioState = RADIO_ST_WAIT_ACK;
        /* fall through */
    case RADIO_ST_WAIT_ACK:
        if ((events & ESP32_MAC_EVENT_ACK_RX_DONE) != 0u)
            radioTxEnd(RADIO_TX_OK, (uint8_t)((radioRxBuf[1] & RADIO_FCF_FRAME_PENDING) != 0u));
        else if ((events & ESP32_MAC_EVENT_TX_ABORT) != 0u)
            radioTxNoAck();
        break;

    case RADIO_ST_ED:
        if ((events & ESP32_MAC_EVENT_ED_DONE) != 0u)
        {
            radioEdDbm = (int8_t)(MAC(ESP32_MAC_ED_SCAN_CFG) >> ESP32_MAC_ED_RSS_SHIFT);
            radioEdState = RADIO_ED_READY;
            radioToWish();
        }
        break;

    default:
        radioCounters.isrSpurious++;
        break;
    }
}

/* ---- lifecycle ---- */

static void radioApplyPower(void)
{
    uint32_t index;

    if (radioPowerDbm >= RADIO_POWER_MAX_DBM)
        index = 15u;
    else
        index = 3u + (uint32_t)(radioPowerDbm - RADIO_POWER_MIN_DBM) / 3u;

    MAC(ESP32_MAC_TX_POWER) = index & ESP32_MAC_TX_POWER_MASK;
}

static void radioApplyFilter(void)
{
    uint32_t ctrl = MAC(ESP32_MAC_CTRL_CFG) | ESP32_MAC_CTRL_INF0_ENABLE;

    if (radioFilter == RADIO_RX_FILTERED)
        ctrl = (ctrl & ~ESP32_MAC_CTRL_PROMISCUOUS) | ESP32_MAC_CTRL_AUTO_ACK_RX;
    else
        ctrl = (ctrl | ESP32_MAC_CTRL_PROMISCUOUS) & ~ESP32_MAC_CTRL_AUTO_ACK_TX;

    MAC(ESP32_MAC_CTRL_CFG) = ctrl;
    radioAckFollowsRoom();
}

int Radio_Init(void)
{
    if (!Esp32Phy_Init())
        return 0;

    HAL_IrqLock();
    ESP32_REG(ESP32_MODEM_SYSCON_BASE, ESP32_MODEM_SYSCON_MODEM_RST_CONF) |= ESP32_MODEM_SYSCON_RST_ZBMAC;
    ESP32_REG(ESP32_MODEM_SYSCON_BASE, ESP32_MODEM_SYSCON_MODEM_RST_CONF) &= ~ESP32_MODEM_SYSCON_RST_ZBMAC;
    HAL_Dsb();

    MAC(ESP32_MAC_EVENT_EN) = ESP32_MAC_EVENT_TX_DONE | ESP32_MAC_EVENT_RX_DONE | ESP32_MAC_EVENT_ACK_TX_DONE
                              | ESP32_MAC_EVENT_ACK_RX_DONE | ESP32_MAC_EVENT_RX_ABORT | ESP32_MAC_EVENT_TX_ABORT
                              | ESP32_MAC_EVENT_ED_DONE | ESP32_MAC_EVENT_TIMER0_OVERFLOW;
    MAC(ESP32_MAC_EVENT_STATUS) = ESP32_MAC_EVENT_ALL;
    /* Receive aborts that raise an event: the two an Ack sequence ends with, and the coexistence break,
     * after which the MAC stays idle. A bad CRC or a preamble with no frame it recovers from itself. */
    MAC(ESP32_MAC_RX_ABORT_EN) = (1u << (ESP32_MAC_RX_ABORT_TX_ACK_TIMEOUT - 1u))
                                 | (1u << (ESP32_MAC_RX_ABORT_TX_ACK_COEX_BREAK - 1u))
                                 | (1u << (ESP32_MAC_RX_ABORT_COEX_BREAK - 1u));
    MAC(ESP32_MAC_TX_ABORT_EN) = (1u << (ESP32_MAC_TX_ABORT_RX_ACK_TIMEOUT - 1u))
                                 | (1u << (ESP32_MAC_TX_ABORT_TX_COEX_BREAK - 1u))
                                 | (1u << (ESP32_MAC_TX_ABORT_TX_SECURITY_ERROR - 1u))
                                 | (1u << (ESP32_MAC_TX_ABORT_CCA_FAILED - 1u))
                                 | (1u << (ESP32_MAC_TX_ABORT_CCA_BUSY - 1u));
    MAC(ESP32_MAC_COEX_PTI) = (RADIO_PTI_ALONE << ESP32_MAC_COEX_PTI_SHIFT) | (RADIO_PTI_ALONE << ESP32_MAC_COEX_ACK_PTI_SHIFT);
    MAC(ESP32_MAC_RXON_DELAY) = RADIO_RXON_DELAY_US;
    bt_bb_set_zb_tx_on_delay(RADIO_TXON_DELAY_US);
    MAC(ESP32_MAC_ED_SCAN_CFG) = ((uint32_t)(uint8_t)(int8_t)RADIO_CCA_DEFAULT_DBM & ESP32_MAC_ED_CCA_THRESHOLD_MASK)
                                 | (ESP32_MAC_ED_SAMPLE_AVG << ESP32_MAC_ED_SAMPLE_MODE_SHIFT)
                                 | (ESP32_MAC_ED_CCA_MODE_ED << ESP32_MAC_ED_CCA_MODE_SHIFT);
    MAC(ESP32_MAC_CHANNEL) = 3u;
    radioSetAckPending(0u);

    SrcMatch_Init(&radioSrcMatch);
    radioPowerDbm = 0;
    radioApplyPower();
    radioApplyFilter();
    radioState = RADIO_ST_IDLE;

    if (!Esp32Irq_Route(ESP32_SRC_ZB_MAC, ESP32_LINE_RADIO, RADIO_PRIORITY, radioIsr))
    {
        radioState = RADIO_ST_DOWN;
        HAL_IrqUnlock();
        return 0;
    }

    HAL_IrqUnlock();
    return 1;
}

/* ---- channel, power, filter, addresses: applied with interrupts masked, never into a transmission ---- */

static int radioBusy(void)
{
    return radioTxPending != 0u;
}

int Radio_SetChannel(uint8_t channel)
{
    if (radioState == RADIO_ST_DOWN || channel < RADIO_CHAN_MIN || channel > RADIO_CHAN_MAX)
        return 0;

    HAL_IrqLock();
    radioChannel = channel;
    MAC(ESP32_MAC_CHANNEL) = (3u + 5u * (uint32_t)(channel - RADIO_CHAN_MIN)) & ESP32_MAC_CHANNEL_HOP_MASK;

    /* A new channel takes effect at the next receive command; a transmission uses it on its next try. */
    if (!radioBusy() && (radioState == RADIO_ST_RX || radioState == RADIO_ST_IDLE))
        radioToWish();

    HAL_IrqUnlock();
    return 1;
}

int Radio_GetChannel(uint8_t *channel)
{
    if (channel == 0)
        return 0;

    *channel = radioChannel;
    return 1;
}

int Radio_SetPower(int8_t dbm)
{
    if (radioState == RADIO_ST_DOWN)
        return 0;

    if (dbm < RADIO_POWER_MIN_DBM)
        dbm = RADIO_POWER_MIN_DBM;
    else if (dbm > RADIO_POWER_MAX_DBM)
        dbm = RADIO_POWER_MAX_DBM;

    radioPowerDbm = dbm;
    radioApplyPower();
    return 1;
}

int Radio_GetPower(int8_t *dbm)
{
    if (dbm == 0)
        return 0;

    /* What the index really gives: the 3 dB step below the setting. */
    if (radioPowerDbm >= RADIO_POWER_MAX_DBM)
        *dbm = RADIO_POWER_MAX_DBM;
    else
        *dbm = (int8_t)(RADIO_POWER_MIN_DBM + ((radioPowerDbm - RADIO_POWER_MIN_DBM) / 3) * 3);

    return 1;
}

int Radio_SetCcaThreshold(int8_t dbm)
{
    uint32_t cfg;

    if (radioState == RADIO_ST_DOWN)
        return 0;

    HAL_IrqLock();
    cfg = MAC(ESP32_MAC_ED_SCAN_CFG) & ~ESP32_MAC_ED_CCA_THRESHOLD_MASK;
    MAC(ESP32_MAC_ED_SCAN_CFG) = cfg | ((uint32_t)(uint8_t)dbm & ESP32_MAC_ED_CCA_THRESHOLD_MASK);
    HAL_IrqUnlock();
    return 1;
}

int Radio_SetRxFilter(int mode)
{
    if (radioState == RADIO_ST_DOWN || mode < RADIO_RX_FILTERED || mode > RADIO_RX_ALL)
        return 0;

    HAL_IrqLock();
    radioFilter = mode;
    radioApplyFilter();
    HAL_IrqUnlock();
    return 1;
}

void Radio_Witnesses(void)
{
    Debug_Emit(DBG_COMP_RADIO, (uint16_t)MAC(ESP32_MAC_TXRX_STATUS), DBG_RADIO_RSN_STATE);
}

int Radio_RxEnable(int on)
{
    if (radioState == RADIO_ST_DOWN)
        return 0;

    HAL_IrqLock();
    radioRxWanted = on ? 1 : 0;

    /* During a transmission, an Ack or a measurement only the wish is recorded; its end applies it. */
    if (radioState == RADIO_ST_IDLE || radioState == RADIO_ST_RX)
        radioToWish();

    HAL_IrqUnlock();
    return 1;
}

/* A pair that is off gets the permissive default: PAN and short 0xFFFF, extended all ones. */
int Radio_SetPanId(uint16_t panid, int on)
{
    if (radioState == RADIO_ST_DOWN)
        return 0;

    MAC(ESP32_MAC_INF0_PAN_ID) = on ? panid : 0xFFFFu;
    return 1;
}

int Radio_SetShortAddr(uint16_t addr, int on)
{
    if (radioState == RADIO_ST_DOWN)
        return 0;

    MAC(ESP32_MAC_INF0_SHORT_ADDR) = on ? addr : 0xFFFFu;
    return 1;
}

/* ext8 is MSB first, as Spinel carries it; the registers hold it least significant octet first. */
int Radio_SetExtAddr(const uint8_t ext8[8], int on)
{
    if (radioState == RADIO_ST_DOWN || ext8 == 0)
        return 0;

    if (!on)
    {
        MAC(ESP32_MAC_INF0_EXT_ADDR0) = 0xFFFFFFFFu;
        MAC(ESP32_MAC_INF0_EXT_ADDR1) = 0xFFFFFFFFu;
        return 1;
    }

    MAC(ESP32_MAC_INF0_EXT_ADDR0) = (uint32_t)ext8[7] | ((uint32_t)ext8[6] << 8) | ((uint32_t)ext8[5] << 16)
                                    | ((uint32_t)ext8[4] << 24);
    MAC(ESP32_MAC_INF0_EXT_ADDR1) = (uint32_t)ext8[3] | ((uint32_t)ext8[2] << 8) | ((uint32_t)ext8[1] << 16)
                                    | ((uint32_t)ext8[0] << 24);
    return 1;
}

/* The vector reads the table; the copy is made with interrupts masked so it never reads a half-written one. */
int Radio_SetSrcMatch(const SrcMatch_Table *table)
{
    if (radioState == RADIO_ST_DOWN || table == 0)
        return 0;

    HAL_IrqLock();
    SrcMatch_CopyOnAir(&radioSrcMatch, table);
    HAL_IrqUnlock();
    return 1;
}

/* ---- transmit ---- */

int Radio_Tx(const uint8_t *psdu, uint8_t len, int8_t dbm, int cca, int maxRetries, void *token)
{
    uint8_t i;

    if (radioState == RADIO_ST_DOWN || psdu == 0 || len < 3u + RADIO_FCS_LEN || len > RADIO_TX_PSDU_MAX)
        return 0;

    HAL_IrqLock();

    if (radioBusy() || radioState == RADIO_ST_SEND_ACK || radioState == RADIO_ST_ED || radioTxEnded)
    {
        HAL_IrqUnlock();
        return 0;
    }

    /* The MAC appends the FCS: the buffer holds the PSDU without it, the length counts it. */
    radioTxBuf[0] = len;

    for (i = 0u; i < (uint8_t)(len - RADIO_FCS_LEN); ++i)
        radioTxBuf[1u + i] = psdu[i];

    (void)Radio_SetPower(dbm);
    radioTxPending = 1u;
    radioTxDue = 0u;
    Tick_GetMs(&radioTxStartMs);
    radioTxToken = token;
    radioTxCca = cca ? 1u : 0u;
    radioTxNb = 0u;
    radioTxBe = RADIO_MIN_BE;
    radioTxRetriesLeft = (maxRetries < 0) ? 0 : (maxRetries > RADIO_MAX_RETRIES) ? RADIO_MAX_RETRIES : maxRetries;
    radioTxAttempt();
    HAL_IrqUnlock();
    return 1;
}

/* ---- energy detection ---- */

int Radio_StartEd(void)
{
    if (radioState != RADIO_ST_RX || radioTxPending)
    {
        radioEdState = RADIO_ED_FAILED;
        return 0;
    }

    HAL_IrqLock();

    if (radioState != RADIO_ST_RX || radioTxPending)
    {
        HAL_IrqUnlock();
        radioEdState = RADIO_ED_FAILED;
        return 0;
    }

    radioCounters.edRequests++;
    radioEdState = RADIO_ED_MEASURING;
    radioStop();
    radioState = RADIO_ST_ED;
    MAC(ESP32_MAC_ED_SCAN_DURATION) = RADIO_ED_SYMBOLS;
    MAC(ESP32_MAC_COMMAND) = ESP32_MAC_CMD_ED_START;
    HAL_IrqUnlock();
    return 1;
}

int Radio_TakeEd(int8_t *dbm, int *fresh)
{
    int st = radioEdState;

    if (st == RADIO_ED_MEASURING)
        return 0;

    radioEdState = RADIO_ED_IDLE;

    if (st != RADIO_ED_READY)
        return -1;

    if (dbm != 0)
        *dbm = radioEdDbm;

    if (fresh != 0)
        *fresh = 1;

    return 1;
}

void Radio_AbortEd(void)
{
    HAL_IrqLock();

    if (radioState == RADIO_ST_ED)
        radioToWish();

    radioEdState = RADIO_ED_IDLE;
    HAL_IrqUnlock();
}

/* ---- housekeeping ---- */

void Radio_SetRxHandler(void (*fn)(const Radio_Frame *frame))
{
    radioRxHandler = fn;
}

void Radio_SetTxDoneHandler(void (*fn)(void *token, int status, int framePending))
{
    radioTxDoneHandler = fn;
}

/* SYSTIMER's unit 0 in microseconds: 52 bits at 16 MHz never wrap in practice. The snapshot is taken with
 * interrupts masked, so its two halves are one reading. */
static uint64_t radioClockNow(void)
{
    uint32_t hi;
    uint32_t lo;
    uint32_t spins;

    ESP32_REG(ESP32_SYSTIMER_BASE, ESP32_SYSTIMER_UNIT0_OP) = ESP32_SYSTIMER_UNIT0_OP_UPDATE;

    for (spins = 0u; spins < 1000u; ++spins)
    {
        if ((ESP32_REG(ESP32_SYSTIMER_BASE, ESP32_SYSTIMER_UNIT0_OP) & ESP32_SYSTIMER_UNIT0_OP_VALUE_VALID) != 0u)
            break;
    }

    hi = ESP32_REG(ESP32_SYSTIMER_BASE, ESP32_SYSTIMER_UNIT0_VALUE_HI);
    lo = ESP32_REG(ESP32_SYSTIMER_BASE, ESP32_SYSTIMER_UNIT0_VALUE_LO);
    return (((uint64_t)hi << 32) | lo) / ESP32_SYSTIMER_TICKS_PER_US;
}

uint64_t Radio_ClockUs(void)
{
    uint64_t now;

    HAL_IrqLock();
    now = radioClockNow();
    HAL_IrqUnlock();
    return now;
}

/* 802.15.4's FCS, CRC-16 with polynomial x^16 + x^12 + x^5 + 1, reflected, low octet first. */
static void radioPutFcs(Radio_Frame *f)
{
    uint16_t crc = 0u;
    uint8_t i;
    uint8_t bit;
    uint8_t n = (uint8_t)(f->len - RADIO_FCS_LEN);

    for (i = 0u; i < n; ++i)
    {
        crc ^= f->psdu[i];

        for (bit = 0u; bit < 8u; ++bit)
            crc = (uint16_t)((crc & 1u) ? ((crc >> 1) ^ 0x8408u) : (crc >> 1));
    }

    f->psdu[n] = (uint8_t)crc;
    f->psdu[n + 1u] = (uint8_t)(crc >> 8);
}

void Radio_Poll(void)
{
    Radio_Frame *f;
    void *token;

    if (radioState == RADIO_ST_DOWN)
        return;

    while (radioRingTail != radioRingHead)
    {
        f = &radioRing[radioRingTail % RADIO_RX_SLOTS];
        radioPutFcs(f);
        radioCounters.rxFrames++;

        if (radioRxHandler != 0)
            radioRxHandler(f);

        radioRingTail = (uint8_t)(radioRingTail + 1u);
        HAL_IrqLock();
        radioAckFollowsRoom();
        HAL_IrqUnlock();
    }

    if (radioTxPending && !radioTxEnded)
    {
        uint32_t ms;

        Tick_GetMs(&ms);

        /* A transmission past its bound is ended here, and the radio goes back to the host's wish. */
        if ((uint32_t)(ms - radioTxStartMs) > RADIO_TX_BOUND_MS)
        {
            HAL_IrqLock();

            if (radioTxPending && !radioTxEnded)
            {
                MAC(ESP32_MAC_COMMAND) = ESP32_MAC_CMD_TIMER0_STOP;
                radioTxStuck++;
                Debug_Emit(DBG_COMP_RADIO, (uint16_t)radioState, DBG_RADIO_RSN_OFF_TIMEOUT);
                radioTxEnd(RADIO_TX_TIMEOUT, 0u);
            }

            HAL_IrqUnlock();
        }
    }

    if (radioTxEnded)
    {
        token = radioTxToken;

        if (radioTxStatus == RADIO_TX_OK || radioTxStatus == RADIO_TX_NO_ACK)
            radioCounters.txFrames++;
        else
            radioCounters.txFailed++;

        radioTxEnded = 0u;

        if (radioTxDoneHandler != 0)
            radioTxDoneHandler(token, radioTxStatus, radioTxFramePending);
    }
}

/* Every step waits on the MAC's own interrupt; the loop's usual wake-ups serve the rest. */
int Radio_PollAgain(void)
{
    return 0;
}

void Radio_GetCounters(Radio_Counters *out)
{
    if (out == 0)
        return;

    HAL_IrqLock();
    out->rxFrames = radioCounters.rxFrames;
    out->rxDropped = radioCounters.rxDropped;
    out->rxCrcBad = 0u;
    out->rxUnfiltered = 0u;
    out->txFrames = radioCounters.txFrames;
    out->txFailed = radioCounters.txFailed;
    out->edRequests = radioCounters.edRequests;
    out->rxRingFull = radioCounters.rxRingFull;
    out->isrSpurious = radioCounters.isrSpurious;
    out->ackSent = radioCounters.ackSent;
    out->ackFp = radioCounters.ackFp;
    out->ackLate = radioCounters.ackLate;
    out->ackNotPllOn = 0u;
    out->rxFiltered = radioCounters.rxFiltered;
    HAL_IrqUnlock();
}

/* The contract's microsecond stamp: this board's millisecond (docs/protocol.md §7.2). */
uint32_t Radio_Now(void)
{
    uint32_t ms;

    Tick_GetMs(&ms);
    return ms * 1000u;
}
