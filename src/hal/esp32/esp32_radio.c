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

With timed transmit (RCP_TX_TIMING), for an OpenThread host:

  Enhanced Ack  the MAC answers a 2015 frame that asks for an Ack only from a buffer software fills: the
                vector builds it at RX_DONE, secured when the frame was (rcp/mac_sec.c), points the transmit
                DMA at it and says it is ready, and the MAC sends it in the turnaround. Without a host's
                keys a 2015 frame gets no Ack at all, where the AVR back-end sends an immediate one.
  stamps        a received frame's timestamp is the radio clock at its SFD, taken in the vector at the MAC's
                RX_SFD_DONE event.
  timed frames  Radio_TxAt's one attempt is started by the modem's event matrix at timer 0's end, in hardware,
                ahead of the moment by the ramp-up ESP-IDF allows and the preamble and SFD. The MAC keeps
                receiving until a guard before it, when the vector stops it and arms the channel. The Ack of
                each transmission is kept for the host.

The LQI the MAC reports barely moves with the signal, so the one handed up is the RSSI's: 0 at the board's
sensitivity, 255 from 20 dB above it, as the other boards' radios fall off near their sensitivity.

The machine's state changes only in the vector or with interrupts locked. The loop takes what the vector
left: received frames, and the end of a transmission. */
#include <stdint.h>

#include "board.h"
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
/* ESP-IDF tracks the PHY's PLL once a second (esp_phy's PHY_TRACK_PLL_PERIOD_IN_US). */
#define RADIO_PLL_TRACK_MS 1000u
/* The frame control field's bits this file reads. */
#define RADIO_FCF_FRAME_PENDING 0x10u
#define RADIO_FCF_ACK_REQUEST 0x20u
#define RADIO_FCF_VERSION_SHIFT 12u
#define RADIO_FCF_VERSION_2006 1u
#define RADIO_FCF_VERSION_2015 2u
/* What goes on air before the end of the SFD: four octets of preamble and the SFD, 32 us each. */
#define RADIO_OCTET_US 32u
#define RADIO_SHR_US (5u * RADIO_OCTET_US)
/* The ramp-up from the start to the first symbol: ESP-IDF's for a transmission (esp_ieee802154_dev.h); for a
 * CCA and the transmission after it, ESP-IDF allows 256 us and the sum below, measured with it, comes out
 * 43 us later, so the CCA's start is taken that much earlier. */
#define RADIO_TX_RAMPUP_US 98u
#define RADIO_CCA_TX_RAMPUP_US 299u
/* Against a ConBee II, this board's transmit and receive errors add up to 47 us, its stamps and its starts
 * late together (docs/hardware_esp32.md §12.6); the sum is what a CSL transmitter gets wrong, and it is taken
 * off the start. */
#define RADIO_TIMED_EARLY_US 47u
/* How long before a timed frame's start the vector stops receiving and arms the event matrix: room for the
 * vector's latency and the stop. */
#define RADIO_TIMED_GUARD_US 400u
#define ETM(offset) ESP32_REG(ESP32_MODEM_ETM_BASE, (offset))
/* A received SFD raises an event only where its stamp is needed. */
#ifdef RCP_TX_TIMING
#define RADIO_EVENT_SFD ESP32_MAC_EVENT_RX_SFD_DONE
#else
#define RADIO_EVENT_SFD 0u
#endif
/* The LQI handed up: linear over this many dB above the board's sensitivity. */
#define RADIO_LQI_SPAN_DB 20
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
/* The extended address as Spinel carries it, MSB first: an Enhanced Ack's nonce is built from it. */
static uint8_t radioExtAddr[8];

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
static uint32_t radioTrackDueMs;
static void *radioTxToken;
static uint8_t radioTxCca;
static uint8_t radioTxNb;
static uint8_t radioTxBe;
static int radioTxRetriesLeft;
static volatile uint8_t radioTxEnded;
static volatile uint8_t radioTxStatus;
static volatile uint8_t radioTxFramePending;

#ifdef RCP_TX_TIMING
/* The host's security state while it has keys: then a 2015 frame gets its Enhanced Ack, built here. */
static MacSec_State *volatile radioMacSec;
static uint8_t radioAckBuf[RADIO_BUF] __attribute__((aligned(4)));
/* The radio clock at the last received SFD, for the frame or Ack it starts. */
static uint64_t radioSfdUs;
static uint8_t radioSfdValid;
/* The transmission is a timed one: its single attempt starts from the timer, and a moment it cannot meet
 * ends it. The Ack it received, copied before receive is re-armed, for the host. */
static uint8_t radioTxTimed;
/* Its start on the radio clock's low half, when the command is due; 1 once the event matrix holds it. */
static uint32_t radioTxAtStartUs;
static uint8_t radioTxArmed;
static Radio_Frame radioTxAckFrame;
static volatile uint8_t radioTxAckHave;
#endif

static volatile int radioEdState = RADIO_ED_IDLE;
static int8_t radioSensDbm = -100;
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

/* The LQI of a frame received at this RSSI (see the top of this file). */
static uint8_t radioLqi(int8_t rssi)
{
    int d = (int)rssi - (int)radioSensDbm;

    if (d <= 0)
        return 0u;

    if (d >= RADIO_LQI_SPAN_DB)
        return 255u;

    return (uint8_t)((d * 255) / RADIO_LQI_SPAN_DB);
}

static int radioRingRoom(void)
{
    return (uint8_t)(radioRingHead - radioRingTail) < RADIO_RX_SLOTS;
}

/* The hardware Ack follows the mode, and in the filtered mode whether the ring has room; the Enhanced Ack
 * follows them too, and whether the host has keys. The register is written only when a bit changes: this
 * runs in the vector at a frame's end, inside the Ack's turnaround, and a write to the MAC's configuration
 * there loses the Ack. */
static void radioAckFollowsRoom(void)
{
    uint32_t ctrl = MAC(ESP32_MAC_CTRL_CFG);
    uint32_t want = ctrl & ~(ESP32_MAC_CTRL_AUTO_ACK_TX | ESP32_MAC_CTRL_AUTO_ENHACK);

    if (radioFilter == RADIO_RX_FILTERED && radioRingRoom())
    {
        want |= ESP32_MAC_CTRL_AUTO_ACK_TX;
#ifdef RCP_TX_TIMING
        if (radioMacSec != 0)
            want |= ESP32_MAC_CTRL_AUTO_ENHACK;
#endif
    }

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

/* Now, or once the Ack the MAC is sending is out: stopping it there would lose the Ack. */
static void radioTxStartSoon(void)
{
    if (radioState == RADIO_ST_SEND_ACK)
        radioTxDue = 1u;
    else
        radioTxStartNow();
}

/* One attempt: after its backoff when CCA is asked for, at once when not. The MAC keeps receiving while
 * the backoff runs. */
static void radioTxAttempt(void)
{
    uint32_t wait;

#ifdef RCP_TX_TIMING
    if (radioTxTimed)
        return;
#endif

    if (!radioTxCca)
    {
        radioTxStartSoon();
        return;
    }

    wait = radioBackoffSlots(radioTxBe) * RADIO_BACKOFF_US;

    if (wait == 0u)
    {
        radioTxStartSoon();
        return;
    }

    MAC(ESP32_MAC_TIMER0_THRESHOLD) = wait;
    MAC(ESP32_MAC_COMMAND) = ESP32_MAC_CMD_TIMER0_START;
}

/* The transmission's end. During an Ack going out the MAC is left to it: the Ack's end re-arms receive, and
 * a start it was to make is called off. */
static void radioTxEnd(uint8_t status, uint8_t framePending)
{
    radioTxPending = 0u;
    radioTxDue = 0u;
    radioTxStatus = status;
    radioTxFramePending = framePending;
    radioTxEnded = 1u;
#ifdef RCP_TX_TIMING
    ETM(ESP32_ETM_CHENCLR) = 1u;
    radioTxArmed = 0u;
#endif

    if (radioState != RADIO_ST_SEND_ACK)
        radioToWish();
}

/* A busy channel: back off again with a larger exponent, or give up after macMaxCSMABackoffs. */
static void radioTxChannelBusy(void)
{
#ifdef RCP_TX_TIMING
    /* A timed frame has its one moment: a busy channel ends it, as a window gone does. */
    if (radioTxTimed)
    {
        radioTxEnd(RADIO_TX_CCA_FAIL, 0u);
        return;
    }
#endif

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

#ifdef RCP_TX_TIMING
/* 1 when the MAC waits for this file's Enhanced Ack to the frame just received: a 2015 frame that asks for
 * an Ack, with the Enhanced Ack on (which also means the address filter passed it). */
static int radioEnhAckDue(void)
{
    uint16_t fcf = (uint16_t)(radioRxBuf[1] | ((uint16_t)radioRxBuf[2] << 8));

    return (fcf & RADIO_FCF_ACK_REQUEST) != 0u && ((fcf >> RADIO_FCF_VERSION_SHIFT) & 3u) == RADIO_FCF_VERSION_2015
           && (MAC(ESP32_MAC_CTRL_CFG) & ESP32_MAC_CTRL_AUTO_ENHACK) != 0u;
}

/* The Enhanced Ack of the frame just received, into the Ack buffer, and the MAC told it is ready. 1 = it goes
 * out; 0 = the frame must not be answered this way. */
static int radioEnhAckSend(uint8_t len, uint8_t *pending, MacSec_AckResult *result)
{
    MacSec_State *sec = radioMacSec;
    const uint8_t *psdu = &radioRxBuf[1];
    uint8_t avail = (uint8_t)(len - RADIO_FCS_LEN);
    uint8_t ie[MAC_SEC_PROBE_IE_MAX];
    uint8_t ieLen;
    uint8_t n;
    MacSec_AckRx rx;

    if (sec == 0 || len < 3u + RADIO_FCS_LEN || len > RADIO_PSDU_MAX || !MacSec_ParseAckRx(psdu, avail, &rx))
        return 0;

    *pending = (uint8_t)MacSec_AckPending(&rx, &radioSrcMatch);
    ieLen = MacSec_ProbeIe(sec, &rx, radioLqi((int8_t)radioRxBuf[len - 1u]), (int8_t)radioRxBuf[len - 1u], radioSensDbm, ie);
    n = MacSec_BuildEnhAck(sec, psdu, avail, radioExtAddr, *pending, ie, ieLen, &radioAckBuf[1], result);

    if (n == 0u)
        return 0;

    /* As a transmission: the length counts the FCS the MAC appends. */
    radioAckBuf[0] = n;
    MAC(ESP32_MAC_TXDMA_ADDR) = (uint32_t)radioAckBuf;
    MAC(ESP32_MAC_ENHACK_DONE) = 1u;
    return 1;
}
#endif

static void radioRxDone(void)
{
    uint8_t len = radioRxBuf[0];
    int acks = radioAcksItself();
    uint8_t pending = 0u;
    Radio_Frame *f;
    uint8_t i;
#ifdef RCP_TX_TIMING
    MacSec_AckResult sec = {0, 0u, 0u};

    if (!acks && radioEnhAckDue())
    {
        acks = radioEnhAckSend(len, &pending, &sec);

        /* The MAC waits for an Ack that will not come: stop it, and it receives again below. */
        if (!acks)
            radioStop();
    }
    else
#endif
    if (acks)
    {
        pending = radioPendingFor(&radioRxBuf[1], len);
        radioSetAckPending(pending);
    }

    if (acks && pending)
        radioCounters.ackFp++;

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
        f->lqi = radioLqi(f->rssi);
        f->channel = radioChannel;
#ifdef RCP_TX_TIMING
        /* The SFD's stamp, or one counted back from now when that event was not seen. */
        f->timestamp = radioSfdValid ? radioSfdUs
                                     : radioClockNow() - (uint64_t)(1u + len) * RADIO_OCTET_US;
        radioSfdValid = 0u;
        f->ackedSec = sec.secured;
        f->ackKeyIndex = sec.keyIndex;
        f->ackFrameCounter = sec.frameCounter;
#else
        f->timestamp = radioClockNow();
        f->ackedSec = 0;
        f->ackKeyIndex = 0u;
        f->ackFrameCounter = 0u;
#endif
        f->crcOk = 1;
        f->ackedFp = acks ? (int)pending : 0;
        radioRingHead = (uint8_t)(radioRingHead + 1u);
        radioAckFollowsRoom();
    }

    if (acks)
        radioState = RADIO_ST_SEND_ACK;
    else
        radioToWish();
}

#ifdef RCP_TX_TIMING
/* The Ack of the transmission, out of the receive buffer before receive is re-armed over it. */
static void radioTxAckKeep(void)
{
    uint8_t len = radioRxBuf[0];
    uint8_t i;

    if (len < 3u + RADIO_FCS_LEN || len > RADIO_PSDU_MAX)
        return;

    for (i = 0u; i < (uint8_t)(len - RADIO_FCS_LEN); ++i)
        radioTxAckFrame.psdu[i] = radioRxBuf[1u + i];

    radioTxAckFrame.len = len;
    radioTxAckFrame.rssi = (int8_t)radioRxBuf[len - 1u];
    radioTxAckFrame.lqi = radioLqi(radioTxAckFrame.rssi);
    radioTxAckFrame.channel = radioChannel;
    radioTxAckFrame.timestamp = radioSfdValid ? radioSfdUs
                                              : radioClockNow() - (uint64_t)(1u + len) * RADIO_OCTET_US;
    radioSfdValid = 0u;
    radioTxAckFrame.crcOk = 1;
    radioTxAckFrame.ackedFp = 0;
    radioTxAckFrame.ackedSec = 0;
    radioTxAckFrame.ackKeyIndex = 0u;
    radioTxAckFrame.ackFrameCounter = 0u;
    radioTxAckHave = 1u;
}
#endif

#ifdef RCP_TX_TIMING
/* The guard before a timed frame: stop receiving, point the MAC at the frame, and leave its start to the event
 * matrix at timer 0's next end. A moment the guard has already eaten completes as CCA_FAIL. */
static void radioTxArm(void)
{
    int32_t rest;

    radioStop();
    MAC(ESP32_MAC_TXDMA_ADDR) = (uint32_t)radioTxBuf;
    MAC(ESP32_MAC_RXDMA_ADDR) = (uint32_t)radioRxBuf;

    if (radioTxCca)
        MAC(ESP32_MAC_ED_SCAN_DURATION) = RADIO_CCA_SYMBOLS;

    rest = (int32_t)(radioTxAtStartUs - (uint32_t)radioClockNow());

    if (rest <= 0)
    {
        radioTxEnd(RADIO_TX_CCA_FAIL, 0u);
        return;
    }

    ETM(ESP32_ETM_CHENCLR) = 1u;
    ETM(ESP32_ETM_CH0_EVT_ID) = ESP32_ETM_EVT_ZB_TIMER0_OVERFLOW;
    ETM(ESP32_ETM_CH0_TASK_ID) = radioTxCca ? ESP32_ETM_TASK_ZB_ED_TRIG_TX : ESP32_ETM_TASK_ZB_TX_START;
    ETM(ESP32_ETM_CHENSET) = 1u;
    MAC(ESP32_MAC_TIMER0_THRESHOLD) = (uint32_t)rest;
    MAC(ESP32_MAC_COMMAND) = ESP32_MAC_CMD_TIMER0_START;
    radioTxArmed = 1u;
    radioState = RADIO_ST_TX;
}

/* Timer 0's end for a timed frame: the guard, or the moment itself, at which the hardware has started it. */
static void radioTxTimedStep(void)
{
    if (radioTxArmed)
    {
        ETM(ESP32_ETM_CHENCLR) = 1u;
        radioTxArmed = 0u;
        return;
    }

    /* An Ack still going out holds the air past the moment. */
    if (radioState == RADIO_ST_SEND_ACK)
        radioTxEnd(RADIO_TX_CCA_FAIL, 0u);
    else
        radioTxArm();
}
#endif

static void radioIsr(void);

__attribute__((section(".iram.text.esp32_radio"))) static void radioIsr(void)
{
    uint32_t events = MAC(ESP32_MAC_EVENT_STATUS);
    uint32_t reason;

    MAC(ESP32_MAC_EVENT_STATUS) = events;

#ifdef RCP_TX_TIMING
    /* The SFD of a frame or an Ack: its stamp, for the RX_DONE or ACK_RX_DONE that follows. */
    if ((events & ESP32_MAC_EVENT_RX_SFD_DONE) != 0u)
    {
        radioSfdUs = radioClockNow();
        radioSfdValid = 1u;
    }
#endif

    /* A backoff's end: the attempt starts now, or once the Ack going out is done. A timed frame's moment
     * cannot wait for an Ack: it is gone. */
    if ((events & ESP32_MAC_EVENT_TIMER0_OVERFLOW) != 0u && radioTxPending)
    {
        MAC(ESP32_MAC_COMMAND) = ESP32_MAC_CMD_TIMER0_STOP;

#ifdef RCP_TX_TIMING
        if (radioTxTimed)
            radioTxTimedStep();
        else
#endif
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
        {
#ifdef RCP_TX_TIMING
            radioTxAckKeep();
#endif
            radioTxEnd(RADIO_TX_OK, (uint8_t)((radioRxBuf[1] & RADIO_FCF_FRAME_PENDING) != 0u));
        }
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
                              | ESP32_MAC_EVENT_ED_DONE | ESP32_MAC_EVENT_TIMER0_OVERFLOW | RADIO_EVENT_SFD;
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
    radioSensDbm = Board_Get()->rxSensitivityDbm;
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
    uint8_t i;

    if (radioState == RADIO_ST_DOWN || ext8 == 0)
        return 0;

    for (i = 0u; i < 8u; ++i)
        radioExtAddr[i] = ext8[i];

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

    if (radioBusy() || radioState == RADIO_ST_ED || radioTxEnded)
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
#ifdef RCP_TX_TIMING
    radioTxTimed = 0u;
    radioTxAckHave = 0u;
#endif
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

#ifdef RCP_TX_TIMING
/* One attempt, started by the event matrix so that the end of the SFD is at the antenna at sfdEndUs: ahead of
 * it by the preamble and SFD, and by the ramp-up of the task that starts it. A moment already gone completes
 * as CCA_FAIL (hal/radio.h); the watchdog counts from the moment, not from now. */
int Radio_TxAt(const uint8_t *psdu, uint8_t len, int8_t dbm, int cca, uint32_t sfdEndUs, void *token)
{
    uint8_t i;
    uint32_t ms;
    int32_t lead;

    if (radioState == RADIO_ST_DOWN || psdu == 0 || len < 3u + RADIO_FCS_LEN || len > RADIO_TX_PSDU_MAX)
        return 0;

    HAL_IrqLock();

    if (radioBusy() || radioState == RADIO_ST_ED || radioTxEnded)
    {
        HAL_IrqUnlock();
        return 0;
    }

    radioTxBuf[0] = len;

    for (i = 0u; i < (uint8_t)(len - RADIO_FCS_LEN); ++i)
        radioTxBuf[1u + i] = psdu[i];

    (void)Radio_SetPower(dbm);
    radioTxPending = 1u;
    radioTxDue = 0u;
    radioTxTimed = 1u;
    radioTxAckHave = 0u;
    radioTxToken = token;
    radioTxCca = cca ? 1u : 0u;
    radioTxNb = 0u;
    radioTxBe = RADIO_MIN_BE;
    radioTxRetriesLeft = 0;
    radioTxArmed = 0u;
    radioTxAtStartUs = sfdEndUs - RADIO_SHR_US - (radioTxCca ? RADIO_CCA_TX_RAMPUP_US : RADIO_TX_RAMPUP_US)
                       - RADIO_TIMED_EARLY_US;
    lead = (int32_t)(radioTxAtStartUs - (uint32_t)radioClockNow());
    Tick_GetMs(&ms);
    radioTxStartMs = ms + (lead > 0 ? (uint32_t)lead / 1000u : 0u);

    if (lead > (int32_t)RADIO_TIMED_GUARD_US)
    {
        MAC(ESP32_MAC_TIMER0_THRESHOLD) = (uint32_t)lead - RADIO_TIMED_GUARD_US;
        MAC(ESP32_MAC_COMMAND) = ESP32_MAC_CMD_TIMER0_START;
    }
    else if (lead <= 0 || radioState == RADIO_ST_SEND_ACK)
    {
        radioTxEnd(RADIO_TX_CCA_FAIL, 0u);
    }
    else
    {
        radioTxArm();
    }

    HAL_IrqUnlock();
    return 1;
}

const Radio_Frame *Radio_TxAck(void)
{
    return radioTxAckHave ? &radioTxAckFrame : 0;
}

int Radio_SetMacSec(MacSec_State *state)
{
    HAL_IrqLock();
    radioMacSec = state;

    if (radioState != RADIO_ST_DOWN)
        radioAckFollowsRoom();

    HAL_IrqUnlock();
    return 1;
}
#endif

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
    uint32_t ms;

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

    Tick_GetMs(&ms);

    /* The PLL tracking, between transmissions and energy measurements, as ESP-IDF runs it beside its
     * driver. A round takes about 5 us. */
    if ((int32_t)(ms - radioTrackDueMs) >= 0 && !radioTxPending && radioState != RADIO_ST_ED)
    {
        radioTrackDueMs = ms + RADIO_PLL_TRACK_MS;
        Esp32Phy_Track();
    }

    if (radioTxPending && !radioTxEnded)
    {
        /* A transmission past its bound is ended here, and the radio goes back to the host's wish. Modular and
         * signed: a timed frame's bound starts at its moment, which may still be ahead. */
        if ((int32_t)(ms - radioTxStartMs) > (int32_t)RADIO_TX_BOUND_MS)
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
#ifdef RCP_TX_TIMING
        if (radioTxAckHave)
            radioPutFcs(&radioTxAckFrame);
#endif

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
