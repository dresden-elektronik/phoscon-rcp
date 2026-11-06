/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mg21_rail.c — the ConBee III radio back-end on Silicon Labs' RAIL library (hal/radio.h).
 *
 * The other back-end of this board (hal/mg21/own/mg21_radio.c) drives the radio controller with this tree's
 * own sequencer program. This one hands the radio to RAIL, the vendor's prebuilt library for the xG21
 * (docs/hardware_mg21.md §10.6): RAIL owns the radio blocks, the sequencer, the calibration and the
 * interrupts of the radio; this file owns the contract of hal/radio.h on top of it. It is the rcp image's
 * back-end unless -DDS_MG21_RADIO=own is given (docs/build-and-flash.md §10), and the core above it cannot
 * tell the two apart.
 *
 * The map from the contract to RAIL:
 *   Radio_Init        the TrustZone attribution RAIL's sequencer needs (closed again before a reset),
 *                     RAIL_Init, the 2.4 GHz 802.15.4 PHY RAIL carries, RAIL_IEEE802154_Init (auto Ack,
 *                     the turnaround timings), the PA, the antenna path the stock firmware uses
 *   channel           RAIL channel numbers are the 802.15.4 channel numbers
 *   filter modes      RADIO_RX_FILTERED is RAIL's hardware address filter with auto Ack; PAN and ALL
 *                     are RAIL's promiscuous mode (no Ack), ALL also delivers frames with a bad FCS
 *   addresses         RAIL_IEEE802154_Set{PanId,ShortAddress,LongAddress}, slot 0
 *   source match      the data-request event of RAIL_IEEE802154: the Ack's Frame Pending bit is
 *                     decided from the table while the Ack is still being built
 *   Radio_Tx          the PSDU into RAIL's transmit FIFO behind its PHY length octet, then
 *                     RAIL_StartTx or RAIL_StartCcaCsmaTx; an acknowledged frame waits for the Ack in
 *                     RAIL and is retried here, up to the host's count
 *   energy detection  the instantaneous RSSI of the receiver
 *
 * Context: RAIL calls railEvents() from the radio interrupts. That function does the work that has a
 * deadline (copying a frame out of RAIL's FIFO, deciding Frame Pending) and records the rest in a flag
 * word; Radio_Poll, in the main loop, reports transmissions, recovers overflows, runs the calibrations
 * RAIL asks for and delivers the receive ring.
 *
 * This is the only translation unit that includes RAIL's headers, which are written for a hosted
 * environment: they bring <stdbool.h>, <stddef.h> and, through the stub in rail/compat, the three string
 * functions this tree defines itself. */
#include <stdint.h>

#include "board.h"
#include "dbg.h"
#include "hal.h"
#include "mac.h"
#include "mg21_clock.h"
#include "mg21_radio.h"
#include "mg21_rail_pa.h"
#include "mg21_rail_shims.h"
#include "mg21_regs.h"
#include "radio.h"
#include "srcmatch.h"

#include "rail.h"
#include "rail_ieee802154.h"

/* Line fields of DBG_RADIO_RSN_REFUSED (hal/dbg.h). */
enum
{
    RAIL_REFUSE_NO_HFXO = 1,   /* the clock bring-up did not end on the crystal RAIL's PHY is built for */
    RAIL_REFUSE_INIT,          /* RAIL_Init returned no handle */
    RAIL_REFUSE_CONFIG,        /* a configuration call was refused; the reason field follows in the next record */
    RAIL_REFUSE_FIFO           /* RAIL did not accept this tree's FIFO memory */
};

typedef enum
{
    RAIL_BE_DOWN = 0,
    RAIL_BE_IDLE,
    RAIL_BE_RX
} RailBe_State;

typedef enum
{
    RAIL_TX_IDLE = 0,
    RAIL_TX_STARTED,  /* the frame is with RAIL: waiting for its sent, busy or failed event */
    RAIL_TX_ACK_WAIT  /* sent, an Ack requested: RAIL is waiting for it */
} RailBe_TxPhase;

/* What the interrupt tells the main loop (railEvents -> railTakeEvents). */
#define EV_TX_SENT 0x01u
#define EV_TX_BUSY 0x02u
#define EV_TX_FAIL 0x04u
#define EV_ACK_RX 0x08u
#define EV_ACK_TIMEOUT 0x10u
#define EV_RX_OVERFLOW 0x20u
#define EV_CAL_NEEDED 0x40u

/* The events this back-end subscribes to; everything else RAIL can raise stays off. */
#define RAIL_EVENT_MASK                                                                          \
    (RAIL_EVENT_RX_PACKET_RECEIVED | RAIL_EVENT_RX_FIFO_OVERFLOW | RAIL_EVENT_RX_FRAME_ERROR |   \
     RAIL_EVENT_RX_ADDRESS_FILTERED | RAIL_EVENT_RX_ACK_TIMEOUT |                                \
     RAIL_EVENT_IEEE802154_DATA_REQUEST_COMMAND | RAIL_EVENT_TX_PACKET_SENT |                    \
     RAIL_EVENT_TX_CHANNEL_BUSY | RAIL_EVENT_TX_BLOCKED | RAIL_EVENT_TX_UNDERFLOW |              \
     RAIL_EVENT_TX_ABORTED | RAIL_EVENT_TXACK_PACKET_SENT | RAIL_EVENT_TXACK_ABORTED |          \
     RAIL_EVENT_TXACK_BLOCKED | RAIL_EVENT_TXACK_UNDERFLOW | RAIL_EVENT_CAL_NEEDED |            \
     RAIL_EVENT_TX_SCHEDULED_TX_MISSED)

/* FIFO memory handed to RAIL. Both are circular buffers whose size RAIL wants a power of two: the receive
 * FIFO holds a few full frames, the transmit FIFO one frame and its length octet with room. */
#define RAIL_RX_FIFO_BYTES 512u
#define RAIL_TX_FIFO_BYTES 256u

/* 802.15.4: macAckWaitDuration is 54 symbols; RAIL's timeout covers the sync word only, so it is that
 * minus the 12 symbols of the Ack's own PHY header and payload, 16 us each. */
#define RAIL_ACK_TIMEOUT_US 672u
#define RAIL_TURNAROUND_US 192u /* 12 symbols: Ack turnaround, and the wait before a frame after an Ack */
#define RAIL_IDLE_TO_RADIO_US 100u

/* Bounds on a transmission in the main loop's millisecond: a 127-octet frame is 4.3 ms on air; a CSMA
 * run of five tries can spend 50 ms in backoff; an Ack is due within 0.9 ms of the frame's end. */
#define RAIL_TX_DONE_MS 20u
#define RAIL_TX_CSMA_MS 120u
#define RAIL_TX_ACK_MS 5u

/* A timed frame: RAIL's schedule names the start of the preamble, which is the SHR's 160 us before the
 * end of the SFD; with CCA it names the start of a fixed backoff, then the single CCA. The two leads
 * are what RAIL's own report of the sync word's end measures beyond that, 1 us apart at most: its
 * transition without CCA, and the 128 us CCA on top with it. */
#define RAIL_SHR_US 160u
#define RAIL_TIMED_CCA_BACKOFF_US 150u
#define RAIL_TIMED_LEAD_US 19u
#define RAIL_TIMED_CCA_LEAD_US 148u

/* An Enhanced Ack leaves later than an Immediate Ack, for the time to build it: the turnaround Silicon
 * Labs' own OpenThread port gives it. */
#define RAIL_ENH_ACK_TURNAROUND_US 256u

#define RAIL_FCF_TYPE_MASK 0x07u
#define RAIL_FCF_TYPE_COMMAND 0x03u
#define RAIL_FCF_VERSION_2015 2u
#define RAIL_ADDR_MODE_SHORT 2u
#define RAIL_ADDR_MODE_EXT 3u

#define RAIL_DEFAULT_CHANNEL 11u
#define RAIL_DEFAULT_DBM 10
#define RAIL_VOLTAGE_MV 3300u /* the supply the stock firmware's PA is configured for */
#define RAIL_RAMP_US 10u

#define RAIL_FCF_TYPE_MASK 0x07u
#define RAIL_FCF_TYPE_ACK 0x02u
#define RAIL_FCF_FRAME_PENDING 0x10u

/* The core's coprocessor access register: the archive is built for the hardware FPU's calling
 * convention, and a floating-point register is not accessible until CP10 and CP11 are enabled. */
#define RAIL_SCB_CPACR 0xE000ED88u
#define RAIL_CPACR_CP10_CP11 0x00F00000u

/* The radio interrupts' slots in NVIC (docs/hardware_mg21.md §10.1): AGC 31 through SYNTH 40. */
#define RAIL_IRQ_FIRST 31u
#define RAIL_IRQ_LAST 40u

typedef struct
{
    RailBe_State state;
    RAIL_Handle_t handle;
    uint8_t channel;
    int8_t power;
    int8_t appliedPower;
    uint8_t paMode;
    uint8_t rxFilter;
    uint8_t panOn;
    uint8_t shortOn;
    uint8_t extOn;
    uint16_t panId;
    uint16_t shortAddr;
    uint8_t extAddr[8]; /* Spinel order, MSB first */
    SrcMatch_Table srcMatch;

    Radio_Frame rxRing[RADIO_RX_SLOTS];
    volatile uint8_t rxHead;
    volatile uint8_t rxTail;
    void (*rxHandler)(const Radio_Frame *frame);
    void (*txDoneHandler)(void *token, int status, int framePending);

    volatile uint32_t events;
    volatile int ackFpPending; /* the data request just seen was answered with Frame Pending */

    int edState;
    int8_t edDbm;

    volatile RailBe_TxPhase txPhase;
    uint8_t txBuf[1u + RADIO_TX_PSDU_MAX]; /* the PHY length octet, then the PSDU without its FCS */
    uint8_t txLen;                         /* PHR value: the PSDU's length with its FCS */
    uint8_t txSeq;
    uint8_t txTriesLeft;
    int txAck;
    int txCca;
    volatile int txAckFp;
    uint32_t txSince;
    void *txToken;
    int txTimed;
    uint32_t txSfdEndUs;

    MacSec_State *macSec; /* set while a host's keys are: the Enhanced Ack is on */
    volatile uint8_t lastLqi; /* of the frame before: a probed Ack has no metrics of its own yet */
    volatile int8_t lastRssi;
    volatile int ackSecPending;
    volatile uint8_t ackKeyIndex;
    volatile uint32_t ackFrameCounter;
    Radio_Frame txAckFrame;
    int txAckHave;

    uint32_t clockHigh; /* extends RAIL's 32-bit clock, main loop only */
    uint32_t clockLast;

    volatile Radio_Counters counters;
} RailBe;

static RailBe s_rail;

/* RAIL's memory, named and sized here rather than left to its defaults. */
static uint32_t s_railRxFifo[RAIL_RX_FIFO_BYTES / 4u];
static uint32_t s_railTxFifo[RAIL_TX_FIFO_BYTES / 4u];

static const RAIL_PtiConfig_t s_railPti = {.mode = RAIL_PTI_MODE_DISABLED};

/* Not const: the host's PHY_CCA_THRESHOLD replaces the -75 dBm of RAIL's initializer. */
static RAIL_CsmaConfig_t s_railCsma = RAIL_CSMA_CONFIG_802_15_4_2003_2p4_GHz_OQPSK_CSMA;

static const RAIL_IEEE802154_Config_t s_rail154 = {
    .addresses = NULL,
    .ackConfig =
        {
            .enable = true,
            .ackTimeout = RAIL_ACK_TIMEOUT_US,
            .rxTransitions = {.success = RAIL_RF_STATE_RX, .error = RAIL_RF_STATE_RX},
            .txTransitions = {.success = RAIL_RF_STATE_RX, .error = RAIL_RF_STATE_RX},
        },
    .timings =
        {
            .idleToRx = RAIL_IDLE_TO_RADIO_US,
            .idleToTx = RAIL_IDLE_TO_RADIO_US,
            .rxToTx = RAIL_TURNAROUND_US,
            .txToRx = RAIL_TURNAROUND_US,
            .rxSearchTimeout = 0,
            .txToRxSearchTimeout = 0,
        },
    .framesMask = RAIL_IEEE802154_ACCEPT_STANDARD_FRAMES,
    .promiscuousMode = false,
    .isPanCoordinator = false,
    .defaultFramePendingInOutgoingAcks = false,
};

static void railEvents(RAIL_Handle_t handle, RAIL_Events_t events);

static const RAIL_Config_t s_railConfig = {.eventsCallback = railEvents};

/* ---- small helpers ---- */

static uint32_t railTakeEvents(void)
{
    uint32_t ev;

    HAL_IrqLock();
    ev = s_rail.events;
    s_rail.events = 0u;
    HAL_IrqUnlock();
    return ev;
}

static void railRaise(uint32_t ev)
{
    s_rail.events |= ev;
}

/* ---- the receive ring ---- */

static int railRxRoom(void)
{
    return (uint8_t)(s_rail.rxHead - s_rail.rxTail) < RADIO_RX_SLOTS;
}

/* Called from railEvents, once per RAIL_EVENT_RX_PACKET_RECEIVED: one packet is the callback's. The
 * PHY length octet leads the data; with RAIL_RX_OPTION_STORE_CRC the FCS follows the payload, so the
 * record is exactly the contract's PSDU. */
static void railRxPacket(RAIL_Handle_t handle)
{
    RAIL_RxPacketInfo_t info;
    RAIL_RxPacketDetails_t det;
    uint8_t buf[1u + RADIO_PSDU_MAX];
    RAIL_RxPacketHandle_t ph;
    Radio_Frame *dst;
    uint8_t phr;
    uint8_t i;

    ph = RAIL_GetRxPacketInfo(handle, RAIL_RX_PACKET_HANDLE_OLDEST_COMPLETE, &info);

    if (ph == RAIL_RX_PACKET_HANDLE_INVALID || info.packetBytes < 1u + 3u ||
        info.packetBytes > sizeof(buf))
        return;

    det.timeReceived.timePosition = RAIL_PACKET_TIME_DEFAULT;
    det.timeReceived.totalPacketBytes = 0u;

    if (RAIL_GetRxPacketDetails(handle, ph, &det) != RAIL_STATUS_NO_ERROR)
        return;

    RAIL_CopyRxPacket(buf, &info);
    phr = buf[0];

    if ((uint16_t)phr + 1u != info.packetBytes || phr > RADIO_PSDU_MAX)
        return;

    /* The Ack of the frame in flight is the transmission's answer, not a frame for the host. */
    if (det.isAck)
    {
        if (s_rail.txPhase != RAIL_TX_IDLE && s_rail.txAck && phr >= 3u && buf[3] == s_rail.txSeq)
        {
            s_rail.txAckFp = (buf[1] & RAIL_FCF_FRAME_PENDING) ? 1 : 0;
            det.timeReceived.totalPacketBytes = (uint32_t)phr + 1u;
            s_rail.txAckFrame.timestamp = RAIL_GetRxTimeSyncWordEndAlt(handle, &det) == RAIL_STATUS_NO_ERROR
                                              ? det.timeReceived.packetTime
                                              : RAIL_GetTime();
            s_rail.txAckFrame.len = phr;
            s_rail.txAckFrame.rssi = det.rssi;
            s_rail.txAckFrame.lqi = det.lqi;
            s_rail.txAckFrame.channel = s_rail.channel;
            s_rail.txAckFrame.crcOk = 1;

            for (i = 0u; i < phr; ++i)
                s_rail.txAckFrame.psdu[i] = buf[1u + i];

            s_rail.txAckHave = 1;
            railRaise(EV_ACK_RX);
        }

        return;
    }

    if (!railRxRoom())
    {
        s_rail.counters.rxDropped++;
        s_rail.counters.rxRingFull++;
        return;
    }

    dst = &s_rail.rxRing[s_rail.rxHead % RADIO_RX_SLOTS];

    for (i = 0u; i < phr; ++i)
        dst->psdu[i] = buf[1u + i];

    dst->len = phr;
    dst->rssi = det.rssi;
    dst->lqi = det.lqi;
    s_rail.lastLqi = det.lqi;
    s_rail.lastRssi = det.rssi;
    dst->channel = s_rail.channel;
    /* The low half of the clock at the end of the SFD; railDeliver, in the main loop, extends it. The
     * Ack path above needs no stamp, and an unknown one falls back to the moment of the callback. */
    det.timeReceived.totalPacketBytes = (uint32_t)phr + 1u;

    if (RAIL_GetRxTimeSyncWordEndAlt(handle, &det) == RAIL_STATUS_NO_ERROR)
        dst->timestamp = det.timeReceived.packetTime;
    else
        dst->timestamp = RAIL_GetTime();

    dst->crcOk = det.crcPassed ? 1 : 0;
    dst->ackedFp = s_rail.ackFpPending;
    s_rail.ackFpPending = 0;
    dst->ackedSec = s_rail.ackSecPending;
    dst->ackKeyIndex = s_rail.ackKeyIndex;
    dst->ackFrameCounter = s_rail.ackFrameCounter;
    s_rail.ackSecPending = 0;
    s_rail.rxHead = (uint8_t)(s_rail.rxHead + 1u);
}

/* An Enhanced Ack, from what has arrived of a version 2 frame. RAIL raises this event for an
 * Ack-requesting command or data frame right behind the auxiliary security header. 1 = the frame was
 * version 2, and this was its answer however it went: a frame that gets none is retried by its sender. */
static int railEnhancedAck(RAIL_Handle_t handle)
{
    /* The longest MAC header up to there: FCF, sequence, two PANs, two extended addresses, security. */
    enum { RAIL_EARLY_OCTETS = 1u + 2u + 1u + 2u + 8u + 2u + 8u + 6u };
    RAIL_RxPacketInfo_t info;
    uint8_t rx[RAIL_EARLY_OCTETS];
    uint8_t ack[1u + MAC_SEC_ACK_MAX];
    uint8_t ie[MAC_SEC_PROBE_IE_MAX];
    uint8_t ieLen;
    MacSec_AckRx parsed;
    MacSec_AckResult result;
    uint8_t avail;
    uint8_t n;
    int fp;

    if (s_rail.macSec == 0)
        return 0;

    RAIL_GetRxIncomingPacketInfo(handle, &info);

    if (info.packetBytes < 3u)
        return 0;

    if (info.packetBytes > RAIL_EARLY_OCTETS)
    {
        info.packetBytes = RAIL_EARLY_OCTETS;

        if (info.firstPortionBytes >= RAIL_EARLY_OCTETS)
        {
            info.firstPortionBytes = RAIL_EARLY_OCTETS;
            info.lastPortionData = NULL;
        }
    }

    RAIL_CopyRxPacket(rx, &info);
    avail = (uint8_t)(info.packetBytes - 1u); /* behind the length octet */

    if (!MacSec_ParseAckRx(&rx[1], avail, &parsed))
        return 0;

    if (parsed.version != RAIL_FCF_VERSION_2015)
        return 0;

    fp = MacSec_AckPending(&parsed, &s_rail.srcMatch);
    ieLen = MacSec_ProbeIe(s_rail.macSec, &parsed, s_rail.lastLqi, s_rail.lastRssi, Board_Get()->rxSensitivityDbm, ie);
    n = MacSec_BuildEnhAck(s_rail.macSec, &rx[1], avail, s_rail.extAddr, fp, ie, ieLen, &ack[1], &result);

    if (n == 0u)
    {
        s_rail.counters.ackLate++;
        return 1;
    }

    ack[0] = n;

    if (RAIL_IEEE802154_WriteEnhAck(handle, ack, n) != RAIL_STATUS_NO_ERROR)
    {
        s_rail.counters.ackLate++;
        return 1;
    }

    s_rail.ackFpPending = fp;
    s_rail.ackSecPending = result.secured;
    s_rail.ackKeyIndex = result.keyIndex;
    s_rail.ackFrameCounter = result.frameCounter;

    if (fp)
        s_rail.counters.ackFp++;

    return 1;
}

/* The Frame Pending decision of an Immediate Ack, taken while RAIL is still building it
 * (docs/ack-pending-frame.md): an empty table's "every request claims pending", otherwise the table. */
static void railDataRequest(RAIL_Handle_t handle)
{
    RAIL_IEEE802154_Address_t addr;
    int pending;

    if (railEnhancedAck(handle))
        return;

    /* With the Enhanced Ack on, this event also comes for a data frame, which claims nothing. */
    if (s_rail.macSec != 0)
    {
        RAIL_RxPacketInfo_t info;
        uint8_t head[3];

        RAIL_GetRxIncomingPacketInfo(handle, &info);

        if (info.packetBytes >= sizeof(head))
        {
            info.packetBytes = sizeof(head);

            if (info.firstPortionBytes >= sizeof(head))
            {
                info.firstPortionBytes = sizeof(head);
                info.lastPortionData = NULL;
            }

            RAIL_CopyRxPacket(head, &info);

            if ((head[1] & RAIL_FCF_TYPE_MASK) != RAIL_FCF_TYPE_COMMAND)
                return;
        }
    }

    if (RAIL_IEEE802154_GetAddress(handle, &addr) != RAIL_STATUS_NO_ERROR)
    {
        s_rail.counters.ackLate++;
        return;
    }

    if (!s_rail.srcMatch.enabled)
        pending = 1;
    else if (addr.length == RAIL_IEEE802154_ShortAddress)
        pending = SrcMatch_HasShort(&s_rail.srcMatch, addr.shortAddress);
    else
        pending = SrcMatch_HasExt(&s_rail.srcMatch, addr.longAddress);

    if (!pending)
        return;

    if (RAIL_IEEE802154_SetFramePending(handle) == RAIL_STATUS_NO_ERROR)
    {
        s_rail.ackFpPending = 1;
        s_rail.counters.ackFp++;
    }
    else
    {
        s_rail.counters.ackLate++;
    }
}

static void railEvents(RAIL_Handle_t handle, RAIL_Events_t events)
{
    if (events & RAIL_EVENT_IEEE802154_DATA_REQUEST_COMMAND)
        railDataRequest(handle);

    if (events & RAIL_EVENT_RX_PACKET_RECEIVED)
        railRxPacket(handle);

    if (events & RAIL_EVENT_RX_FIFO_OVERFLOW)
    {
        s_rail.counters.rxDropped++;
        railRaise(EV_RX_OVERFLOW);
    }

    if (events & RAIL_EVENT_RX_FRAME_ERROR)
        s_rail.counters.rxUnfiltered++;

    if (events & RAIL_EVENT_RX_ADDRESS_FILTERED)
        s_rail.counters.rxFiltered++;

    if (events & RAIL_EVENT_TXACK_PACKET_SENT)
        s_rail.counters.ackSent++;

    if (events & (RAIL_EVENT_TXACK_ABORTED | RAIL_EVENT_TXACK_BLOCKED | RAIL_EVENT_TXACK_UNDERFLOW))
        s_rail.counters.ackLate++;

    if (events & RAIL_EVENT_TX_PACKET_SENT)
        railRaise(EV_TX_SENT);

    if (events & (RAIL_EVENT_TX_CHANNEL_BUSY | RAIL_EVENT_TX_SCHEDULED_TX_MISSED))
        railRaise(EV_TX_BUSY);

    if (events & (RAIL_EVENT_TX_BLOCKED | RAIL_EVENT_TX_UNDERFLOW | RAIL_EVENT_TX_ABORTED))
        railRaise(EV_TX_FAIL);

    if (events & RAIL_EVENT_RX_ACK_TIMEOUT)
        railRaise(EV_ACK_TIMEOUT);

    if (events & RAIL_EVENT_CAL_NEEDED)
        railRaise(EV_CAL_NEEDED);
}

/* ---- configuration pieces ---- */

/* The receive options that follow the mode: the FCS is always kept (the contract's PSDU carries it),
 * and a bad one is delivered only in the sniffer mode. */
static int railApplyFilter(void)
{
    uint8_t frames = RAIL_IEEE802154_ACCEPT_STANDARD_FRAMES;
    int promiscuous = s_rail.rxFilter != RADIO_RX_FILTERED;
    RAIL_RxOptions_t ignoreCrc = (s_rail.rxFilter == RADIO_RX_ALL) ? RAIL_RX_OPTION_IGNORE_CRC_ERRORS : 0u;

    if (promiscuous)
        frames |= RAIL_IEEE802154_ACCEPT_ACK_FRAMES;

    return RAIL_IEEE802154_SetPromiscuousMode(s_rail.handle, promiscuous) == RAIL_STATUS_NO_ERROR &&
           RAIL_IEEE802154_AcceptFrames(s_rail.handle, frames) == RAIL_STATUS_NO_ERROR &&
           RAIL_ConfigRxOptions(s_rail.handle, RAIL_RX_OPTION_IGNORE_CRC_ERRORS, ignoreCrc) ==
               RAIL_STATUS_NO_ERROR;
}

/* The address filter, slot 0. A pair that is off is RAIL's disabled value: 0xFFFF, and all zeros for the
 * extended address, which RAIL reads in on-air (LSB first) order. */
static int railApplyAddresses(void)
{
    RAIL_IEEE802154_AddrConfig_t cfg;
    uint8_t slot;
    uint8_t k;

    for (slot = 0u; slot < RAIL_IEEE802154_MAX_ADDRESSES; ++slot)
    {
        cfg.panId[slot] = 0xFFFFu;
        cfg.shortAddr[slot] = 0xFFFFu;

        for (k = 0u; k < 8u; ++k)
            cfg.longAddr[slot][k] = 0u;
    }

    if (s_rail.panOn)
        cfg.panId[0] = s_rail.panId;

    if (s_rail.shortOn)
        cfg.shortAddr[0] = s_rail.shortAddr;

    if (s_rail.extOn)
    {
        for (k = 0u; k < 8u; ++k)
            cfg.longAddr[0][k] = s_rail.extAddr[7u - k];
    }

    return RAIL_IEEE802154_SetAddresses(s_rail.handle, &cfg) == RAIL_STATUS_NO_ERROR;
}

static int railApplyPower(int8_t dbm)
{
    uint8_t level = Mg21RailPa_Level(s_rail.paMode, (int16_t)(dbm * 10));

    if (RAIL_SetTxPower(s_rail.handle, level) != RAIL_STATUS_NO_ERROR)
        return 0;

    s_rail.appliedPower = dbm;
    return 1;
}

static void railRadioToWish(void)
{
    RAIL_RadioState_t st = RAIL_GetRadioState(s_rail.handle);

    if (s_rail.state == RAIL_BE_RX)
    {
        if ((st & RAIL_RF_STATE_RX) == 0u)
            (void)RAIL_StartRx(s_rail.handle, s_rail.channel, NULL);
    }
    else if ((st & (RAIL_RF_STATE_RX | RAIL_RF_STATE_TX)) != 0u)
    {
        (void)RAIL_Idle(s_rail.handle, RAIL_IDLE_ABORT, true);
    }
}

/* ---- lifecycle ---- */

static int railRefuse(uint16_t why)
{
    Debug_Emit(DBG_COMP_RADIO, why, DBG_RADIO_RSN_REFUSED);
    return 0;
}

/* RAIL's sequencer program reads EMU and writes BUFC through their non-secure aliases, with non-secure
 * transactions, at the end of every received frame: the 8-byte record in BUFC buffer 2 is how RAIL learns
 * of a frame. Out of reset every peripheral is secure and the PPU rejects those accesses, so the record
 * never lands and RAIL never hears a frame (docs/hardware_mg21.md §10.6). The SDK's SystemInit opens the
 * attribution the way this does: every peripheral but SMU and the SE mailbox non-secure, and the SAU's
 * ALLNS, which makes the CPU's accesses through the same aliases non-secure too - without it the PPU
 * rejects the CPU instead. The core stays Secure, and code, SRAM and SMU keep their secure aliases. */
static void railOpenAttribution(void)
{
    *(volatile uint32_t *)(MG21_SMU_S_BASE + MG21_SMU_CLR_ALIAS + MG21_SMU_PPUSATD0_OFFSET) =
        MG21_SMU_PPUSATD0_MASK;
    *(volatile uint32_t *)(MG21_SMU_S_BASE + MG21_SMU_CLR_ALIAS + MG21_SMU_PPUSATD1_OFFSET) =
        MG21_SMU_PPUSATD1_MASK & ~(MG21_SMU_PPUSATD1_SMU | MG21_SMU_PPUSATD1_SEMAILBOX);
    *(volatile uint32_t *)MG21_SAU_CTRL = MG21_SAU_CTRL_ALLNS;
    HAL_Dsb();
}

/* The reset state again, before any reset this image asks for, as with everything else the image
 * changed: the bootloader reaches its peripherals through the secure aliases. The watchdog reset
 * clears the attribution by itself (bench: a reset with it left open reaches the bootloader and the
 * image again); the SYSRESETREQ fallback is not measured, so the hand-back does not rely on it. */
static void railCloseAttribution(void)
{
    *(volatile uint32_t *)(MG21_SMU_S_BASE + MG21_SMU_SET_ALIAS + MG21_SMU_PPUSATD0_OFFSET) =
        MG21_SMU_PPUSATD0_MASK;
    *(volatile uint32_t *)(MG21_SMU_S_BASE + MG21_SMU_SET_ALIAS + MG21_SMU_PPUSATD1_OFFSET) =
        MG21_SMU_PPUSATD1_MASK;
    *(volatile uint32_t *)MG21_SAU_CTRL = 0u;
    HAL_Dsb();
}

int Radio_Init(void)
{
    RAIL_TxPowerConfig_t pa;
    RAIL_TxPowerMode_t resolved;
    RAIL_AntennaConfig_t ant;
    RAIL_Handle_t h;
    uint16_t fifoBytes;

    if (s_rail.state != RAIL_BE_DOWN)
        return 1;

    if (Clock_Regime() != MG21_CLOCK_HFXO)
        return railRefuse(RAIL_REFUSE_NO_HFXO);

    /* The archive passes floating-point arguments in FPU registers (the toolchain asks for the hard-float
     * ABI), so the FPU must be on before the first call into it. */
    *(volatile uint32_t *)RAIL_SCB_CPACR |= RAIL_CPACR_CP10_CP11;
    HAL_Dsb();

    railOpenAttribution();
    h = RAIL_Init(&s_railConfig, NULL);

    if (h == NULL)
        return railRefuse(RAIL_REFUSE_INIT);

    s_rail.handle = h;

    /* The stock firmware's radio set-up (docs/hardware_mg21.md §10.4): no PTI, antenna path 1 with no
     * antenna pins, the highest PA the part has at 3.3 V with a 10 us ramp, PA calibration on. */

    ant.ant0PinEn = false;
    ant.ant1PinEn = false;
    ant.defaultPath = RAIL_ANTENNA_1;
    ant.ant0Port = 0u;
    ant.ant0Pin = 0u;
    ant.ant1Loc = 0u;
    ant.ant1Port = 0u;
    ant.ant1Pin = 0u;

    pa.mode = RAIL_TX_POWER_MODE_2P4GIG_HIGHEST;
    pa.voltage = RAIL_VOLTAGE_MV;
    pa.rampTime = RAIL_RAMP_US;

    if (RAIL_ConfigPti(h, &s_railPti) != RAIL_STATUS_NO_ERROR ||
        RAIL_ConfigCal(h, RAIL_CAL_ALL) != RAIL_STATUS_NO_ERROR ||
        RAIL_ConfigAntenna(h, &ant) != RAIL_STATUS_NO_ERROR ||
        RAIL_IEEE802154_Config2p4GHzRadio(h) != RAIL_STATUS_NO_ERROR ||
        RAIL_IEEE802154_Init(h, &s_rail154) != RAIL_STATUS_NO_ERROR ||
        /* Without this the data-request event waits for the command byte, which a secured poll
         * hides, and a sleepy child would never be told its frame is pending. */
        RAIL_IEEE802154_EnableEarlyFramePending(h, true) != RAIL_STATUS_NO_ERROR ||
        RAIL_ConfigEvents(h, RAIL_EVENTS_ALL, RAIL_EVENT_MASK) != RAIL_STATUS_NO_ERROR ||
        RAIL_ConfigRxOptions(h, RAIL_RX_OPTION_STORE_CRC | RAIL_RX_OPTION_IGNORE_CRC_ERRORS,
                             RAIL_RX_OPTION_STORE_CRC) != RAIL_STATUS_NO_ERROR ||
        RAIL_ConfigTxPower(h, &pa) != RAIL_STATUS_NO_ERROR)
        return railRefuse(RAIL_REFUSE_CONFIG);

    RAIL_EnablePaCal(true);

    /* "Highest" is whichever PA the part has (the 10 dBm one on the A010 this board carries); the dBm
     * lookup needs to know which, and RAIL resolves it. */
    resolved = RAIL_TX_POWER_MODE_2P4GIG_HIGHEST;

    if (!RAIL_SupportsTxPowerModeAlt(h, &resolved, NULL, NULL))
        return railRefuse(RAIL_REFUSE_CONFIG);

    s_rail.paMode = (uint8_t)resolved;

    fifoBytes = RAIL_RX_FIFO_BYTES;

    if (RAIL_SetRxFifo(h, (uint8_t *)s_railRxFifo, &fifoBytes) != RAIL_STATUS_NO_ERROR ||
        RAIL_SetTxFifo(h, (uint8_t *)s_railTxFifo, 0u, RAIL_TX_FIFO_BYTES) != RAIL_TX_FIFO_BYTES)
        return railRefuse(RAIL_REFUSE_FIFO);

    s_rail.channel = RAIL_DEFAULT_CHANNEL;
    s_rail.power = RAIL_DEFAULT_DBM;
    s_rail.rxFilter = RADIO_RX_FILTERED;
    SrcMatch_Init(&s_rail.srcMatch);

    if (!railApplyFilter() || !railApplyAddresses() || !railApplyPower(s_rail.power))
        return railRefuse(RAIL_REFUSE_CONFIG);

    /* The calibrations RAIL has pending after the configuration (image rejection, temperature). */
    (void)RAIL_Calibrate(h, NULL, RAIL_CAL_ALL_PENDING);

    s_rail.state = RAIL_BE_IDLE;
    Debug_Emit(DBG_COMP_RADIO, (uint16_t)s_rail.paMode, DBG_RADIO_RSN_UP);
    return 1;
}

/* ---- channel and power ---- */

int Radio_SetChannel(uint8_t channel)
{
    if (s_rail.state == RAIL_BE_DOWN || channel < RADIO_CHAN_MIN || channel > RADIO_CHAN_MAX)
        return 0;

    if (channel == s_rail.channel)
        return 1;

    if (s_rail.txPhase != RAIL_TX_IDLE)
        return 0; /* retried by the caller once the transmission is reported */

    s_rail.channel = channel;

    /* A receiver is retuned by starting it again on the new channel. */
    if (s_rail.state == RAIL_BE_RX)
        return RAIL_StartRx(s_rail.handle, channel, NULL) == RAIL_STATUS_NO_ERROR;

    return 1;
}

int Radio_GetChannel(uint8_t *channel)
{
    if (s_rail.state == RAIL_BE_DOWN || channel == 0)
        return 0;

    *channel = s_rail.channel;
    return 1;
}

int Radio_SetPower(int8_t dbm)
{
    if (s_rail.state == RAIL_BE_DOWN)
        return 0;

    s_rail.power = dbm;
    return railApplyPower(dbm);
}

int Radio_GetPower(int8_t *dbm)
{
    if (s_rail.state == RAIL_BE_DOWN || dbm == 0)
        return 0;

    *dbm = s_rail.power;
    return 1;
}

/* RAIL reads the threshold from the CSMA configuration every transmission is started with. */
int Radio_SetCcaThreshold(int8_t dbm)
{
    if (s_rail.state == RAIL_BE_DOWN)
        return 0;

    s_railCsma.ccaThreshold = dbm;
    return 1;
}

/* ---- receive ---- */

int Radio_SetRxFilter(int mode)
{
    if (s_rail.state == RAIL_BE_DOWN || mode < RADIO_RX_FILTERED || mode > RADIO_RX_ALL)
        return 0;

    s_rail.rxFilter = (uint8_t)mode;
    return railApplyFilter();
}

void Radio_Witnesses(void)
{
    if (s_rail.state == RAIL_BE_DOWN)
        return;

    Debug_Emit(DBG_COMP_RADIO, (uint16_t)RAIL_GetRadioState(s_rail.handle), DBG_RADIO_RSN_STATE);
}

int Radio_RxEnable(int on)
{
    if (s_rail.state == RAIL_BE_DOWN)
        return 0;

    /* During a transmission only the wish is recorded; the transmission's end applies it. */
    if (s_rail.txPhase != RAIL_TX_IDLE)
    {
        s_rail.state = on ? RAIL_BE_RX : RAIL_BE_IDLE;
        return 1;
    }

    if (on)
    {
        if (s_rail.state != RAIL_BE_RX)
        {
            if (RAIL_StartRx(s_rail.handle, s_rail.channel, NULL) != RAIL_STATUS_NO_ERROR)
                return 0;

            s_rail.state = RAIL_BE_RX;
        }

        return 1;
    }

    if (s_rail.state == RAIL_BE_RX)
    {
        s_rail.state = RAIL_BE_IDLE;
        return RAIL_Idle(s_rail.handle, RAIL_IDLE_ABORT, true) == RAIL_STATUS_NO_ERROR;
    }

    return 1;
}

int Radio_SetPanId(uint16_t panid, int on)
{
    if (s_rail.state == RAIL_BE_DOWN)
        return 0;

    s_rail.panId = panid;
    s_rail.panOn = on ? 1u : 0u;
    return railApplyAddresses();
}

int Radio_SetShortAddr(uint16_t addr, int on)
{
    if (s_rail.state == RAIL_BE_DOWN)
        return 0;

    s_rail.shortAddr = addr;
    s_rail.shortOn = on ? 1u : 0u;
    return railApplyAddresses();
}

int Radio_SetExtAddr(const uint8_t ext8[8], int on)
{
    uint8_t i;

    if (s_rail.state == RAIL_BE_DOWN || ext8 == 0)
        return 0;

    for (i = 0u; i < 8u; ++i)
        s_rail.extAddr[i] = ext8[i];

    s_rail.extOn = on ? 1u : 0u;
    return railApplyAddresses();
}

/* The data-request event reads the table; the copy is made with interrupts masked so it never reads a
 * half-written one. */
int Radio_SetSrcMatch(const SrcMatch_Table *table)
{
    if (s_rail.state == RAIL_BE_DOWN || table == 0)
        return 0;

    HAL_IrqLock();
    SrcMatch_CopyOnAir(&s_rail.srcMatch, table);
    HAL_IrqUnlock();
    return 1;
}

/* ---- transmit ---- */

/* The frame into RAIL's transmit FIFO and on its way. 1 = RAIL accepted it. */
static int railTxStart(void)
{
    RAIL_TxOptions_t opts = s_rail.txAck ? RAIL_TX_OPTION_WAIT_FOR_ACK : RAIL_TX_OPTIONS_DEFAULT;
    uint16_t bytes = (uint16_t)s_rail.txLen - 1u; /* the length octet and the PSDU without its FCS */
    RAIL_Status_t st;

    if (RAIL_WriteTxFifo(s_rail.handle, s_rail.txBuf, bytes, true) != bytes)
        return 0;

    s_rail.txAckFp = 0;
    HAL_IrqLock();
    s_rail.events &= ~(uint32_t)(EV_TX_SENT | EV_TX_BUSY | EV_TX_FAIL | EV_ACK_RX | EV_ACK_TIMEOUT);
    HAL_IrqUnlock();

    /* The phase is set before RAIL starts: the Ack arrives within 192 us of the frame, long before the
     * main loop looks, and the receive callback only takes an Ack while a transmission is under way. */
    s_rail.txPhase = RAIL_TX_STARTED;
    s_rail.txSince = Radio_Now() / 1000u;

    if (s_rail.txTimed)
    {
        RAIL_ScheduleTxConfig_t when = {.when = s_rail.txSfdEndUs - RAIL_SHR_US - RAIL_TIMED_LEAD_US,
                                        .mode = RAIL_TIME_ABSOLUTE,
                                        .txDuringRx = RAIL_SCHEDULED_TX_DURING_RX_POSTPONE_TX};

        /* The main loop's bound starts when the frame is due, not now. */
        int32_t ahead = (int32_t)(when.when - RAIL_GetTime());

        if (ahead > 0)
            s_rail.txSince += (uint32_t)ahead / 1000u;

        if (s_rail.txCca)
        {
            static RAIL_CsmaConfig_t single = RAIL_CSMA_CONFIG_SINGLE_CCA;

            single.ccaBackoff = RAIL_TIMED_CCA_BACKOFF_US;
            single.ccaThreshold = s_railCsma.ccaThreshold;
            when.when += RAIL_TIMED_LEAD_US;
            when.when -= RAIL_TIMED_CCA_BACKOFF_US + RAIL_TIMED_CCA_LEAD_US;
            st = RAIL_StartScheduledCcaCsmaTx(s_rail.handle, s_rail.channel, opts, &when, &single, NULL);
        }
        else
        {
            st = RAIL_StartScheduledTx(s_rail.handle, s_rail.channel, opts, &when, NULL);
        }
    }
    else if (s_rail.txCca)
    {
        st = RAIL_StartCcaCsmaTx(s_rail.handle, s_rail.channel, opts, &s_railCsma, NULL);
    }
    else
    {
        st = RAIL_StartTx(s_rail.handle, s_rail.channel, opts, NULL);
    }

    if (st != RAIL_STATUS_NO_ERROR)
    {
        s_rail.txPhase = RAIL_TX_IDLE;
        return 0;
    }

    return 1;
}

static int railTxLoad(const uint8_t *psdu, uint8_t len, int8_t dbm, int cca, void *token)
{
    uint8_t i;

    if (s_rail.state == RAIL_BE_DOWN || s_rail.txPhase != RAIL_TX_IDLE || psdu == 0 ||
        len < 3u || len > RADIO_TX_PSDU_MAX)
        return 0;

    if (dbm != s_rail.appliedPower && !railApplyPower(dbm))
        return 0;

    /* RAIL appends the FCS: the PSDU's last two octets are not loaded. */
    s_rail.txBuf[0] = len;

    for (i = 0u; i < (uint8_t)(len - 2u); ++i)
        s_rail.txBuf[1u + i] = psdu[i];

    s_rail.txLen = len;
    s_rail.txSeq = psdu[2];
    s_rail.txAck = Mac_AckRequested(psdu, len);
    s_rail.txCca = cca ? 1 : 0;
    s_rail.txToken = token;
    return 1;
}

int Radio_Tx(const uint8_t *psdu, uint8_t len, int8_t dbm, int cca, int maxRetries, void *token)
{
    if (!railTxLoad(psdu, len, dbm, cca, token))
        return 0;

    s_rail.txTimed = 0;
    s_rail.txTriesLeft = (uint8_t)(s_rail.txAck ? (maxRetries < 0 ? 0 : (maxRetries > 15 ? 15 : maxRetries)) : 0);
    return railTxStart();
}

int Radio_TxAt(const uint8_t *psdu, uint8_t len, int8_t dbm, int cca, uint32_t sfdEndUs, void *token)
{
    if (!railTxLoad(psdu, len, dbm, cca, token))
        return 0;

    s_rail.txTimed = 1;
    s_rail.txSfdEndUs = sfdEndUs;
    s_rail.txTriesLeft = 0u;

    /* RAIL refuses a moment already gone. That is a missed window, not a refusal: radio.h says how a
     * host must hear of it. */
    if (!railTxStart())
    {
        s_rail.txPhase = RAIL_TX_STARTED;
        s_rail.txSince = Radio_Now() / 1000u;
        railRaise(EV_TX_BUSY);
    }

    return 1;
}

const Radio_Frame *Radio_TxAck(void)
{
    return s_rail.txAckHave ? &s_rail.txAckFrame : 0;
}

int Radio_SetMacSec(MacSec_State *state)
{
    RAIL_TransitionTime_t turnaround = RAIL_ENH_ACK_TURNAROUND_US;
    RAIL_IEEE802154_EOptions_t mask = RAIL_IEEE802154_E_OPTION_GB868 | RAIL_IEEE802154_E_OPTION_ENH_ACK;
    bool on = state != 0;

    if (s_rail.state == RAIL_BE_DOWN)
        return 0;

    if (RAIL_IEEE802154_ConfigEOptions(s_rail.handle, mask, on ? mask : 0u) != RAIL_STATUS_NO_ERROR ||
        RAIL_IEEE802154_EnableDataFramePending(s_rail.handle, on) != RAIL_STATUS_NO_ERROR)
        return 0;

    if (on && RAIL_IEEE802154_SetRxToEnhAckTx(s_rail.handle, &turnaround) != RAIL_STATUS_NO_ERROR)
        return 0;

    HAL_IrqLock();
    s_rail.macSec = state;
    HAL_IrqUnlock();
    return 1;
}

static void railTxFinish(int status, int framePending)
{
    void *token = s_rail.txToken;

    s_rail.txPhase = RAIL_TX_IDLE;

    if (status == RADIO_TX_OK || status == RADIO_TX_NO_ACK)
        s_rail.counters.txFrames++;
    else
        s_rail.counters.txFailed++;

    railRadioToWish();

    if (s_rail.txDoneHandler != 0)
    {
        if (s_rail.txAckHave)
        {
            uint64_t now = Radio_ClockUs();

            s_rail.txAckFrame.timestamp = now + (uint64_t)(int64_t)(int32_t)((uint32_t)s_rail.txAckFrame.timestamp - (uint32_t)now);
        }

        s_rail.txDoneHandler(token, status, framePending);
    }

    s_rail.txAckHave = 0;
}

static void railPollTx(uint32_t ev)
{
    uint32_t nowMs = Radio_Now() / 1000u;

    if (s_rail.txPhase == RAIL_TX_STARTED)
    {
        if (ev & EV_TX_BUSY)
        {
            railTxFinish(RADIO_TX_CCA_FAIL, 0);
            return;
        }

        if (ev & EV_TX_FAIL)
        {
            /* radio.h: the one failure a timed frame's host expects. */
            railTxFinish(s_rail.txTimed ? RADIO_TX_CCA_FAIL : RADIO_TX_TIMEOUT, 0);
            return;
        }

        if (ev & EV_TX_SENT)
        {
            if (!s_rail.txAck)
            {
                railTxFinish(RADIO_TX_OK, 0);
                return;
            }

            s_rail.txPhase = RAIL_TX_ACK_WAIT;
            s_rail.txSince = nowMs;
        }
        else if ((int32_t)(nowMs - s_rail.txSince) > (int32_t)(s_rail.txCca ? RAIL_TX_CSMA_MS : RAIL_TX_DONE_MS))
        {
            (void)RAIL_Idle(s_rail.handle, RAIL_IDLE_ABORT, true);
            railTxFinish(s_rail.txTimed ? RADIO_TX_CCA_FAIL : RADIO_TX_TIMEOUT, 0);
            return;
        }
    }

    if (s_rail.txPhase == RAIL_TX_ACK_WAIT)
    {
        if (ev & EV_ACK_RX)
        {
            railTxFinish(RADIO_TX_OK, s_rail.txAckFp);
        }
        else if ((ev & EV_ACK_TIMEOUT) || nowMs - s_rail.txSince > RAIL_TX_ACK_MS)
        {
            if (s_rail.txTriesLeft != 0u)
            {
                s_rail.txTriesLeft--;

                if (!railTxStart())
                    railTxFinish(RADIO_TX_TIMEOUT, 0);
            }
            else
            {
                railTxFinish(RADIO_TX_NO_ACK, 0);
            }
        }
    }
}

/* ---- energy detection ----
 * The receiver's instantaneous RSSI, as the other back-end reads the AGC's: legal while receiving. */

int Radio_StartEd(void)
{
    int16_t q;

    if (s_rail.state != RAIL_BE_RX || s_rail.txPhase != RAIL_TX_IDLE)
    {
        s_rail.edState = RADIO_ED_FAILED;
        return 0;
    }

    s_rail.counters.edRequests++;
    q = RAIL_GetRssiAlt(s_rail.handle, RAIL_GET_RSSI_NO_WAIT);

    if (q == RAIL_RSSI_INVALID)
    {
        s_rail.edState = RADIO_ED_FAILED;
        return 0;
    }

    q = (int16_t)(q / 4); /* quarter-dBm to dBm */

    if (q < -128)
        q = -128;
    else if (q > 127)
        q = 127;

    s_rail.edDbm = (int8_t)q;
    s_rail.edState = RADIO_ED_READY;
    return 1;
}

int Radio_TakeEd(int8_t *dbm, int *fresh)
{
    int st = s_rail.edState;

    s_rail.edState = RADIO_ED_IDLE;

    if (st != RADIO_ED_READY)
        return -1;

    if (dbm != 0)
        *dbm = s_rail.edDbm;

    if (fresh != 0)
        *fresh = 1;

    return 1;
}

void Radio_AbortEd(void)
{
    s_rail.edState = RADIO_ED_IDLE;
}

/* ---- housekeeping ---- */

void Radio_SetRxHandler(void (*fn)(const Radio_Frame *frame))
{
    s_rail.rxHandler = fn;
}

void Radio_SetTxDoneHandler(void (*fn)(void *token, int status, int framePending))
{
    s_rail.txDoneHandler = fn;
}

uint64_t Radio_ClockUs(void)
{
    uint32_t now = RAIL_GetTime();

    if (now < s_rail.clockLast)
        s_rail.clockHigh++;

    s_rail.clockLast = now;
    return ((uint64_t)s_rail.clockHigh << 32) | now;
}

static void railDeliver(void)
{
    Radio_Frame *f;

    while (s_rail.rxHead != s_rail.rxTail && s_rail.rxHandler != 0)
    {
        f = &s_rail.rxRing[s_rail.rxTail % RADIO_RX_SLOTS];
        {
            /* The stamp is in the recent past, so its low half places it against the clock. */
            uint64_t now = Radio_ClockUs();

            f->timestamp = now + (uint64_t)(int64_t)(int32_t)((uint32_t)f->timestamp - (uint32_t)now);
        }
        s_rail.counters.rxFrames++;

        if (!f->crcOk)
            s_rail.counters.rxCrcBad++;

        s_rail.rxHandler(f);
        s_rail.rxTail = (uint8_t)(s_rail.rxTail + 1u);
    }
}

void Radio_Poll(void)
{
    uint32_t ev;

    if (s_rail.state == RAIL_BE_DOWN)
        return;

    (void)Radio_ClockUs(); /* often enough never to miss the 32-bit clock's wrap */
    ev = railTakeEvents();

    if (ev & EV_RX_OVERFLOW)
    {
        /* RAIL stops receiving on an overflow: empty the FIFO and start again. */
        Debug_Emit(DBG_COMP_RADIO, (uint16_t)s_rail.counters.rxDropped, DBG_RADIO_RSN_RX_OVERFLOW);
        (void)RAIL_ResetFifo(s_rail.handle, false, true);

        if (s_rail.state == RAIL_BE_RX && s_rail.txPhase == RAIL_TX_IDLE)
            (void)RAIL_StartRx(s_rail.handle, s_rail.channel, NULL);
    }

    if (s_rail.txPhase != RAIL_TX_IDLE)
    {
        railPollTx(ev);
    }
    else if (ev & EV_CAL_NEEDED)
    {
        (void)RAIL_Calibrate(s_rail.handle, NULL, RAIL_CAL_ALL_PENDING);
    }

    railDeliver();
}

void Radio_GetCounters(Radio_Counters *out)
{
    if (out == 0)
        return;

    out->rxFrames = s_rail.counters.rxFrames;
    out->rxDropped = s_rail.counters.rxDropped;
    out->rxCrcBad = s_rail.counters.rxCrcBad;
    out->rxUnfiltered = s_rail.counters.rxUnfiltered;
    out->txFrames = s_rail.counters.txFrames;
    out->txFailed = s_rail.counters.txFailed;
    out->edRequests = s_rail.counters.edRequests;
    out->rxRingFull = s_rail.counters.rxRingFull;
    out->isrSpurious = 0u;
    out->ackSent = s_rail.counters.ackSent;
    out->ackFp = s_rail.counters.ackFp;
    out->ackLate = s_rail.counters.ackLate;
    out->ackNotPllOn = 0u;
    out->rxFiltered = s_rail.counters.rxFiltered;
}

/* The contract's microsecond stamp: this board's millisecond (docs/protocol.md §7.2). */
uint32_t Radio_Now(void)
{
    uint32_t ms;

    Tick_GetMs(&ms);
    return ms * 1000u;
}

/* ---- hand-back before a reset (mg21_radio.h) ---- */

void Mg21Radio_Release(void)
{
    if ((MG21_CMU->RADIOCLKCTRL & MG21_CMU_RADIOCLKCTRL_EN) == 0u)
    {
        railCloseAttribution();
        return;
    }

    /* Interrupts are masked already, and RAIL's critical sections must not unmask them on their way out. */
    Mg21RailShim_StayMasked();

    if (s_rail.handle != NULL)
        (void)RAIL_Idle(s_rail.handle, RAIL_IDLE_ABORT, true);

    *(volatile uint32_t *)MG21_NVIC_ICER0 = MG21_NVIC_BIT(RAIL_IRQ_FIRST);
    *(volatile uint32_t *)MG21_NVIC_ICER1 = (1u << (RAIL_IRQ_LAST - 32u + 1u)) - 1u;
    HAL_Dsb();

    Mg21Radio_Off();
    railCloseAttribution();
    s_rail.state = RAIL_BE_DOWN;
}

/* ---- the radio's vectors (startup/startup_mg21.S) ----
 * The table names AGC_Handler and its nine neighbours, weak, defaulting to the unexpected-interrupt
 * handler. RAIL's own handlers carry the SDK's names; these definitions are the strong ones that route
 * each slot to its RAIL handler, with the interrupt-depth bookkeeping every handler of this tree does. */
extern void AGC_IRQHandler(void);
extern void BUFC_IRQHandler(void);
extern void FRC_PRI_IRQHandler(void);
extern void FRC_IRQHandler(void);
extern void MODEM_IRQHandler(void);
extern void PROTIMER_IRQHandler(void);
extern void RAC_RSM_IRQHandler(void);
extern void RAC_SEQ_IRQHandler(void);
extern void PRORTC_IRQHandler(void);
extern void SYNTH_IRQHandler(void);

#define RAIL_VECTOR(name)         \
    void name##_Handler(void)     \
    {                             \
        HAL_IrqEnter();           \
        name##_IRQHandler();      \
        HAL_IrqExit();            \
    }

RAIL_VECTOR(AGC)
RAIL_VECTOR(BUFC)
RAIL_VECTOR(FRC_PRI)
RAIL_VECTOR(FRC)
RAIL_VECTOR(MODEM)
RAIL_VECTOR(PROTIMER)
RAIL_VECTOR(RAC_RSM)
RAIL_VECTOR(RAC_SEQ)
RAIL_VECTOR(PRORTC)
RAIL_VECTOR(SYNTH)
