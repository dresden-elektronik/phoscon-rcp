/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* avr_radio.c — the ATmega256RFR2's on-die transceiver behind hal/radio.h (docs/hardware_avr.md §11.1,
 * §11.11).
 *
 * The register set is the AT86RF233's at 0x140 + n, so the contract's semantics are the ConBee II's:
 * reception in basic RX_ON in every mode, the third level filter and the immediate Ack composed by
 * this firmware in the radio vector with a Frame Pending bit from the host's source-match table
 * (rcp/mac.c, docs/ack-pending-frame.md), and transmission as a polled state machine through
 * TX_ARET_ON. What differs is all access, and it is written here from the RFR2 datasheet:
 *
 *   - Registers and the frame buffer are memory: a load or a store, no bus and no bus lock. Thread
 *     code holds the vectors off with the SREG lock around every sequence a vector must not see half
 *     done.
 *   - Eight vectors instead of one pin. RX_END is a received frame, TX_END the end of a transmission
 *     (an Ack of this firmware's, or a TX_ARET transaction), CCA_ED_DONE an energy measurement. A
 *     vector clears its own flag on entry.
 *   - The receive buffer holds the PSDU and then the LQI; the length is in TST_RX_LENGTH, the frame's
 *     validity in PHY_RSSI.RX_CRC_VALID and its energy in PHY_ED_LEVEL, all read in the vector.
 *   - RX_SAFE_MODE keeps a received frame in the buffer until the controller clears the bit, not until
 *     the buffer is read as on the AT86RF233 (datasheet 9.8.7): the vector toggles it on its way out,
 *     on every path.
 *   - At 16 MHz the vector has a third of the ConBee II's cycles, so the header is parsed and the Ack
 *     decided in place in the frame buffer, before the copy; the copy then runs inside the wait for
 *     the Ack's slot.
 *
 * The output power is capped at the vendor's own setting (AVR_RADIO_PWR_IDX_MIN): the module's front
 * end amplifies the transceiver by some 19 dB, and the setting it puts out its rated power at has not
 * been measured on this board (docs/hardware_avr.md §11.5). */
#include <stdint.h>

#include "avr_regs.h"
#include "board.h"
#include "dbg.h"
#include "hal.h"
#include "mac.h"
#include "radio.h"
#include "u_bstream.h"
#include "mac_sec.h"

#define REG(addr) AVR_REG8(addr)
#define FB ((volatile uint8_t *)(uint16_t)AVR_TRXFBST)

#define ST_MASK 0x1Fu
#define ST_BUSY_RX 0x01u
#define ST_BUSY_TX 0x02u
#define ST_RX_ON 0x06u
#define ST_TRX_OFF 0x08u
#define ST_PLL_ON 0x09u
#define ST_BUSY_TX_ARET 0x12u
#define ST_TX_ARET_ON 0x19u
#define ST_TRANSITION 0x1Fu

#define CMD_TX_START 0x02u
#define CMD_FORCE_TRX_OFF 0x03u
#define CMD_FORCE_PLL_ON 0x04u
#define CMD_RX_ON 0x06u
#define CMD_TRX_OFF 0x08u
#define CMD_TX_ARET_ON 0x19u

#define TRAC_SUCCESS 0u
#define TRAC_SUCCESS_PD 1u
#define TRAC_CHAN_FAIL 3u
#define TRAC_NO_ACK 5u

/* The RFR2 identity (docs/hardware_avr.md §11.4). */
#define AVR_RADIO_PART_NUM 0x94u

/* RSSI_BASE_VAL: the ED level is dBm above -90 in 1 dB steps, 0x00..0x53, 0xFF before the first one. */
#define ED_BASE_DBM (-90)
#define ED_LEVEL_MAX 0x53u
#define ED_LEVEL_INVALID 0xFFu
/* Written to request a measurement: no measurement can leave it, so a completion that finds it there
 * had nothing behind it (the ConBee II's rule, docs/hardware_samr21.md §6; not measured here). */
#define ED_TRIGGER_SENTINEL 0x55u

/* CCA_MODE = 1, energy above threshold. */
#define CCA_MODE_ENERGY (1u << AVR_CCA_MODE0)

/* ANT_DIV as the vendor firmware sets it: the external switch under ANT_CTRL, antenna 1, no
 * diversity (docs/hardware_avr.md §11.5). */
#define ANT_DIV_VENDOR (AVR_BIT(AVR_ANT_EXT_SW_EN) | 0x02u)

/* XAH_CTRL_0 retry fields. MAX_CSMA_RETRIES = 7 transmits at once, without CCA. */
#define XAH_FRAME_RETRIES_MASK 0xF0u
#define XAH_CSMA_RETRIES_MASK 0x0Eu
#define XAH_CSMA_NONE 7u
#define XAH_CSMA_DEFAULT 4u

/* The immediate Ack's schedule from the RX_END stamp: aTurnaroundTime is 192 us after the frame's last
 * symbol, and the first preamble symbol leaves 16 us after TX_START (PLL_ON to BUSY_TX), so TX_START is
 * due 176 us after the frame. The vector's stamp is taken after the frame ends, so counting 176 us from
 * it can only make the Ack late, never early. Abandoned past 441 us, as on the ConBee II
 * (docs/ack-pending-frame.md §3). */
#define ACK_TX_START_US 176u
#define ACK_LATEST_US 441u
#define CYCLES_PER_US 16u
#define ACK_STUCK_MS 3u

/* One attempt's bound: CSMA-CA at the default settings 37 ms, a 127-octet frame 4.25 ms and the Ack's wait
 * 1 ms. ARET runs all its attempts inside one wait for TX_END, so that wait gets one bound per attempt. */
#define TX_DEADLINE_MS 50u
#define STATE_DEADLINE_MS 5u
#define ED_CONFIRM_MS 3u

/* A state change on this transceiver settles in microseconds (TRX_OFF to RX_ON, the slowest used here,
 * 110 us); this many polls is far beyond that at 16 MHz. */
#define STATE_WAIT_LIMIT 4000u

/* A frame's clock: RX_END is raised tIRQ = 9 us after its last symbol, the PHR and PSDU take 32 us an octet
 * behind the SFD, and TX_START puts the end of the SFD on air 176 us later: 16 us of PA ramp and 160 us of
 * SHR (datasheet 9.4.2, Digital Interface Timing Characteristics). */
#define RX_END_TIRQ_US 9u
#define OCTET_US 32u
#define TX_TO_SFD_END_US 176u

/* The longer a timed frame's lead, the longer the main loop is not held: this is how long ahead it starts to
 * wait in earnest, longer than a pass of the loop. TIMED_PLL_US is the room for FORCE_PLL_ON before TX_START. */
#define TIMED_SPIN_US 3000u
#define TIMED_PLL_US 60u
/* CCA takes 8 symbols, 128 us, from the end of the request's store. */
#define CCA_US 180u

/* CSMA-CA with IEEE 802.15.4's defaults: macMinBE, macMaxBE, macMaxCSMABackoffs, aUnitBackoffPeriod. */
#define CSMA_MIN_BE 3u
#define CSMA_MAX_BE 5u
#define CSMA_MAX_BACKOFFS 4u
#define CSMA_UNIT_US 320u

/* An Ack starts within macAckWaitDuration of the frame's end, 864 us, and the rest of a reception that has
 * begun by then is bounded by the longest Ack. */
#define ACK_WAIT_US 1000u
#define ACK_RX_US 1500u
/* A secured Ack ends about when the wait does. Past its last octet TRX_STATUS has left BUSY_RX and the vector
 * has yet to take it: this is the time that needs. */
#define ACK_END_US 150u

#define FCS_LEN 2u
#define FCF_VERSION_2015 2u
#define FCF_TYPE_MASK 0x07u
#define FCF_TYPE_ACK 0x02u
#define FCF_FRAME_PENDING 0x10u

/* What an Enhanced Ack overwrites in the frame buffer, its PHR included. The received frame's octets behind it
 * stay where they are, so only these are copied ahead of the Ack's slot and the rest behind TX_START. */
#define ENH_ACK_HEAD (1u + MAC_SEC_ACK_MAX)

/* Output power, tenths of a dBm at the transceiver per TX_PWR index (datasheet Table 9-40). */
static const int16_t radioPowerTenths[16] = {
    35, 33, 28, 23, 18, 12, 5, -5, -15, -25, -35, -45, -65, -85, -115, -165};
/* The highest power this image may set: the vendor's PHY_TX_PWR = 0x0E with the front end on. */
#define AVR_RADIO_PWR_IDX_MIN 14u

enum
{
    ISR_OFF = 0, /* RX_END and TX_END belong to the thread: mirror them */
    ISR_RX,      /* RX_ON: RX_END is a received frame */
    ISR_ACK,     /* an Ack is on air: TX_END ends it */
    ISR_TX,      /* our own frame is on air: TX_END ends it */
    ISR_TX_ACK   /* RX_ON behind it: RX_END is its Ack, or some frame to drop */
};

enum
{
    TX_IDLE = 0,
    TX_TO_ARET,
    TX_WAIT_ARET,
    TX_WAIT_END,
    /* A frame whose Ack the host must see, or one due at a set moment: ARET keeps the Ack to itself and
     * starts when it likes, so the steps are this file's own, on RX_ON and PLL_ON. */
    TX_BACKOFF,
    TX_TIMED,
    TX_SENDING,
    TX_ACK_WAIT
};

enum
{
    TXEV_NONE = 0,
    TXEV_SENT, /* the frame is out, and nothing more comes for it */
    TXEV_WAIT, /* the frame is out, and its Ack is awaited */
    TXEV_ACKED
};

typedef struct
{
    uint8_t inited;
    uint8_t channel;
    uint8_t powerIdx;
    uint8_t rxOn;
    uint8_t rxFilter;
    uint16_t panId;
    uint8_t panOn;
    uint16_t shortAddr;
    uint8_t shortOn;
    uint8_t extAddr[8]; /* MSB first, as the contract takes it */
    uint8_t extOn;

    Radio_Counters counters;

    /* Single producer (the RX_END vector), single consumer (the loop); both indices run free. */
    Radio_Frame rxRing[RADIO_RX_SLOTS];
    volatile uint8_t rxHead;
    volatile uint8_t rxTail;
    void (*rxHandler)(const Radio_Frame *frame);

    volatile uint8_t isrPhase;
    volatile uint8_t micOpen;       /* the RX_END vector lets the interrupts in for the MIC; its frame is not stored yet */
    volatile uint8_t ackEndedInMic; /* the Ack's TX_END came meanwhile and left the receiver off */
    volatile uint32_t ackSince;
    volatile uint8_t irqMirror; /* IRQ causes the vectors left for the thread */
    Mac_AckCfg ackCfg;          /* the vector's filter view, written under the lock */
    SrcMatch_Table srcMatch;

    /* The vector's statistics; it emits no records itself. */
    volatile uint16_t isrFrames;
    volatile uint16_t isrAcks;
    volatile uint16_t isrAcksPending;
    volatile uint16_t isrAcksLate;
    volatile uint16_t isrAckNoRoom;
    volatile uint16_t isrStateFail;
    volatile uint16_t isrAckStuck;
    volatile uint16_t isrCrcBad;
    volatile uint16_t isrReadFail;
    volatile uint16_t lastAckCycles;
    volatile uint16_t lastReadCycles;

    uint8_t txPhase;
    uint8_t txPsdu[RADIO_PSDU_MAX];
    uint8_t txLen;
    uint8_t txAckWait;
    uint8_t txRetries;
    uint8_t txRetriesSet;
    void *txToken;
    uint32_t txDeadline;
    void (*txDoneHandler)(void *token, int status, int framePending);

    uint8_t edState;
    uint32_t edDeadline;
    int8_t edDbm;
    uint8_t edFresh;

    uint8_t irqBits; /* this pass's share of the mirror */

    MacSec_State *macSec; /* set while a host's keys are: the Enhanced Ack is on */
    volatile uint16_t maxMicCycles; /* RX_END to the Enhanced Ack's MIC being in the buffer, the longest since the last witness */
    uint32_t clockHigh;   /* Radio_ClockUs */
    uint32_t clockLast;
    uint32_t rng;
    uint8_t txSeq;
    uint8_t txTriesLeft;
    uint8_t txCca;
    uint8_t txBe;
    uint8_t txBackoffs;
    uint32_t txAtUs; /* the end of a backoff, or the moment a timed frame's SFD ends */
    /* The vector's word on a transmission; TXEV_*, and the frame's end for the Ack wait. */
    volatile uint8_t txEvent;
    volatile uint32_t txEndUs;
    uint16_t txAckExtraUs; /* what the wait grew by when it ran out; 0 until then */
    Radio_Frame txAckFrame;
    uint8_t txAckHave;
} AvrRadio;

static AvrRadio radio;

/* ---- small helpers ---- */

static uint32_t radioNowMs(void)
{
    uint32_t ms;

    Tick_GetMs(&ms);
    return ms;
}

static uint8_t radioState(void)
{
    return (uint8_t)(REG(AVR_TRX_STATUS) & ST_MASK);
}

static void radioCmd(uint8_t cmd)
{
    REG(AVR_TRX_STATE) = cmd;
}

static int radioWaitState(uint8_t want, uint8_t alt)
{
    uint16_t n;

    for (n = 0u; n < STATE_WAIT_LIMIT; ++n)
    {
        uint8_t st = radioState();

        if (st == want || st == alt)
            return 1;
    }

    return 0;
}

/* The vector's view of the addresses and the mode, extended address in on-air order. */
static void radioSyncAckCfg(void)
{
    uint8_t sreg = AvrOwn_LockSave();
    uint8_t k;

    radio.ackCfg.filter.mode = (radio.rxFilter == RADIO_RX_FILTERED)
                                   ? MAC_FILTER_OFF
                                   : (radio.rxFilter == RADIO_RX_PAN ? MAC_FILTER_PAN : MAC_FILTER_ALL);
    radio.ackCfg.filter.panId = radio.panOn ? radio.panId : 0xFFFFu;
    radio.ackCfg.filter.shortAddr = radio.shortOn ? radio.shortAddr : 0xFFFFu;

    for (k = 0u; k < SRCMATCH_EXT_LEN; ++k)
        radio.ackCfg.filter.extAddr[k] = radio.extOn ? radio.extAddr[7u - k] : 0xFFu;

    radio.ackCfg.isCoordinator = 0u;
    AvrOwn_LockRestore(sreg);
}

/* The registers are kept as the host set them, though RX_ON does not filter on them: they read back
 * what was asked, and TX_ARET matches an Ack's sequence number, not these. */
static void radioWriteAddresses(void)
{
    uint16_t pan = radio.panOn ? radio.panId : 0xFFFFu;
    uint16_t shortAddr = radio.shortOn ? radio.shortAddr : 0xFFFFu;
    uint8_t i;

    REG(AVR_PAN_ID_0) = (uint8_t)pan;
    REG(AVR_PAN_ID_1) = (uint8_t)(pan >> 8);
    REG(AVR_SHORT_ADDR_0) = (uint8_t)shortAddr;
    REG(AVR_SHORT_ADDR_1) = (uint8_t)(shortAddr >> 8);

    for (i = 0u; i < 8u; ++i)
        REG(AVR_IEEE_ADDR_0 + i) = radio.extOn ? radio.extAddr[7u - i] : 0xFFu;

    radioSyncAckCfg();
}

/* Into RX_ON through a forced TRX_OFF, with the vectors held off so they never see a half-made state.
 * A cause left over from the previous state is dropped: it described a frame or a transmission that is
 * no longer there. */
static int radioEnterRx(void)
{
    uint8_t sreg = AvrOwn_LockSave();
    int ok;

    radio.isrPhase = ISR_OFF;
    radioCmd(CMD_FORCE_TRX_OFF);
    ok = radioWaitState(ST_TRX_OFF, ST_TRX_OFF);

    if (ok)
    {
        REG(AVR_IRQ_STATUS) = (uint8_t)(AVR_BIT(AVR_RX_END) | AVR_BIT(AVR_TX_END));
        radioCmd(CMD_RX_ON);
        ok = radioWaitState(ST_RX_ON, ST_BUSY_RX);
    }

    if (ok)
        radio.isrPhase = ISR_RX;

    AvrOwn_LockRestore(sreg);
    Debug_Emit(DBG_COMP_RF, radioState(), ok ? DBG_RF_RSN_RX_MODE : DBG_RF_RSN_RX_MODE_FAIL);
    return ok;
}

/* ---- lifecycle ---- */

int Radio_Init(void)
{
    uint8_t part;

    if (radio.inited)
        return 1;

    /* The transceiver is clocked only while PRTRX24 is clear; then a register reset, which keeps the
     * crystal the CPU runs on (datasheet 9.4.1, TRXRST), and SLPTR low so nothing sleeps it. */
    REG(AVR_PRR1) = (uint8_t)(REG(AVR_PRR1) & (uint8_t)~AVR_BIT(AVR_PRTRX24));
    REG(AVR_TRXPR) = AVR_BIT(AVR_TRXRST);
    REG(AVR_TRXPR) = 0u;

    if (!radioWaitState(ST_TRX_OFF, ST_TRX_OFF))
    {
        Debug_Emit(DBG_COMP_RF, radioState(), DBG_RF_RSN_RADIO_INIT_FAIL);
        return 0;
    }

    part = REG(AVR_PART_NUM);

    if (part != AVR_RADIO_PART_NUM)
    {
        Debug_Emit(DBG_COMP_RF, part, DBG_RF_RSN_RADIO_INIT_FAIL);
        return 0;
    }

    Tick_ClockStart();

    REG(AVR_IRQ_MASK) = 0u;
    REG(AVR_IRQ_STATUS) = 0xFFu;

    /* The FCS is the transceiver's on transmit; PA_EXT_EN hands DIG3/DIG4 (PA_CTX on PG0) to it, so the
     * front end follows RX and TX by itself (§11.2). */
    REG(AVR_TRX_CTRL_1) = (uint8_t)(REG(AVR_TRX_CTRL_1) | AVR_BIT(AVR_TX_AUTO_CRC_ON) | AVR_BIT(AVR_PA_EXT_EN));
    REG(AVR_TRX_CTRL_2) = (uint8_t)(REG(AVR_TRX_CTRL_2) | AVR_BIT(AVR_RX_SAFE_MODE));
    REG(AVR_ANT_DIV) = ANT_DIV_VENDOR;

    /* PA_CSD on PD6 is a plain pin: high wakes the front end for both directions. */
    REG(AVR_PORTD) = (uint8_t)(REG(AVR_PORTD) | AVR_BIT(AVR_PORTD6));
    REG(AVR_DDRD) = (uint8_t)(REG(AVR_DDRD) | AVR_BIT(AVR_PORTD6));

    radio.channel = 11u;
    REG(AVR_PHY_CC_CCA) = (uint8_t)(CCA_MODE_ENERGY | radio.channel);

    radio.powerIdx = AVR_RADIO_PWR_IDX_MIN;
    REG(AVR_PHY_TX_PWR) = (uint8_t)((REG(AVR_PHY_TX_PWR) & 0xF0u) | radio.powerIdx);

    radio.rxFilter = RADIO_RX_FILTERED;
    radioWriteAddresses();

    radio.txPhase = TX_IDLE;
    radio.isrPhase = ISR_OFF;
    REG(AVR_IRQ_STATUS) = 0xFFu;
    REG(AVR_IRQ_MASK) = (uint8_t)(AVR_BIT(AVR_RX_END_EN) | AVR_BIT(AVR_TX_END_EN) | AVR_BIT(AVR_CCA_ED_DONE_EN));

    radio.inited = 1u;
    Debug_Emit(DBG_COMP_RF, part, DBG_RF_RSN_RX_MODE);
    return 1;
}

void Radio_Witnesses(void)
{
    uint16_t mic;
    uint16_t v[11];
    uint8_t sreg;

    if (!radio.inited)
        return;

    /* Two bytes each that the vectors write (docs/architecture.md §7). */
    sreg = AvrOwn_LockSave();
    v[0] = radio.isrFrames;
    v[1] = radio.isrCrcBad;
    v[2] = radio.isrAcks;
    v[3] = radio.isrAcksPending;
    v[4] = radio.isrAcksLate;
    v[5] = radio.isrAckNoRoom;
    v[6] = radio.isrStateFail;
    v[7] = radio.isrReadFail;
    v[8] = radio.isrAckStuck;
    v[9] = radio.lastAckCycles;
    v[10] = radio.lastReadCycles;
    mic = radio.maxMicCycles;
    radio.maxMicCycles = 0u;
    AvrOwn_LockRestore(sreg);

    Debug_Emit(DBG_COMP_RF, v[0], DBG_RF_RSN_ISR_FRAMES);
    Debug_Emit(DBG_COMP_RF, v[1], DBG_RF_RSN_ISR_CRC_BAD);
    Debug_Emit(DBG_COMP_RF, v[2], DBG_RF_RSN_ISR_ACKS);
    Debug_Emit(DBG_COMP_RF, v[3], DBG_RF_RSN_ISR_ACKS_PENDING);
    Debug_Emit(DBG_COMP_RF, v[4], DBG_RF_RSN_ISR_ACKS_LATE);
    Debug_Emit(DBG_COMP_RF, v[5], DBG_RF_RSN_ISR_ACK_NO_ROOM);
    Debug_Emit(DBG_COMP_RF, v[6], DBG_RF_RSN_ISR_SPI_FAIL); /* here: an Ack's state change failed */
    Debug_Emit(DBG_COMP_RF, v[7], DBG_RF_RSN_ISR_READ_FAIL);
    Debug_Emit(DBG_COMP_RF, v[8], DBG_RF_RSN_ISR_ACK_STUCK);
    Debug_Emit(DBG_COMP_RF, v[9], DBG_RF_RSN_ISR_ACK_CYCLES);
    Debug_Emit(DBG_COMP_RF, v[10], DBG_RF_RSN_ISR_READ_CYCLES);
    Debug_Emit(DBG_COMP_RF, mic, DBG_RF_RSN_ISR_MIC_CYCLES);
    Debug_Emit(DBG_COMP_RF, (uint16_t)((uint16_t)radioState() << 8 | radioState()), DBG_RF_RSN_RX_MODE);
}

/* ---- channel, power, CCA ---- */

int Radio_SetChannel(uint8_t channel)
{
    if (!radio.inited || channel < RADIO_CHAN_MIN || channel > RADIO_CHAN_MAX)
        return 0;

    REG(AVR_PHY_CC_CCA) = (uint8_t)((REG(AVR_PHY_CC_CCA) & (uint8_t)~ST_MASK) | channel);
    radio.channel = channel;
    return 1;
}

int Radio_GetChannel(uint8_t *channel)
{
    if (!radio.inited || channel == 0)
        return 0;

    *channel = radio.channel;
    return 1;
}

/* Snap down to the first step at or below the request, never above the cap. */
int Radio_SetPower(int8_t dbm)
{
    int16_t target = (int16_t)((int16_t)dbm * 10);
    uint8_t idx = 15u;
    uint8_t i;

    if (!radio.inited)
        return 0;

    for (i = AVR_RADIO_PWR_IDX_MIN; i < 16u; ++i)
    {
        if (radioPowerTenths[i] <= target)
        {
            idx = i;
            break;
        }
    }

    REG(AVR_PHY_TX_PWR) = (uint8_t)((REG(AVR_PHY_TX_PWR) & 0xF0u) | idx);
    radio.powerIdx = idx;
    return 1;
}

int Radio_GetPower(int8_t *dbm)
{
    if (!radio.inited || dbm == 0)
        return 0;

    *dbm = (int8_t)(radioPowerTenths[radio.powerIdx] / 10);
    return 1;
}

/* CCA_ED_THRES is 2 dB steps above -90 dBm; the upper nibble (CCA_CS_THRES) keeps its value. */
int Radio_SetCcaThreshold(int8_t dbm)
{
    int16_t steps = (int16_t)(((int16_t)dbm - ED_BASE_DBM) / 2);

    if (!radio.inited)
        return 0;

    if (steps < 0)
        steps = 0;

    if (steps > 15)
        steps = 15;

    REG(AVR_CCA_THRES) = (uint8_t)((REG(AVR_CCA_THRES) & 0xF0u) | (uint8_t)steps);
    return 1;
}

/* ---- receive ---- */

int Radio_SetRxFilter(int mode)
{
    if (!radio.inited || mode < RADIO_RX_FILTERED || mode > RADIO_RX_ALL)
        return 0;

    radio.rxFilter = (uint8_t)mode;
    radioSyncAckCfg();

    if (radio.rxOn && radio.txPhase == TX_IDLE && !radioEnterRx())
        return 0;

    Radio_Witnesses();
    return 1;
}

int Radio_RxEnable(int on)
{
    uint8_t sreg;
    int ok;

    if (!radio.inited)
        return 0;

    if (on)
    {
        if (!radioEnterRx())
            return 0;

        radio.rxOn = 1u;
        return 1;
    }

    radio.rxOn = 0u;

    if (radio.txPhase != TX_IDLE)
        return 1; /* the transmit machine returns to TRX_OFF itself */

    sreg = AvrOwn_LockSave();
    radio.isrPhase = ISR_OFF;
    radioCmd(CMD_FORCE_TRX_OFF);
    ok = radioWaitState(ST_TRX_OFF, ST_TRX_OFF);
    AvrOwn_LockRestore(sreg);
    return ok;
}

int Radio_SetPanId(uint16_t panid, int on)
{
    if (!radio.inited)
        return 0;

    radio.panId = panid;
    radio.panOn = on ? 1u : 0u;
    radioWriteAddresses();
    return 1;
}

int Radio_SetShortAddr(uint16_t addr, int on)
{
    if (!radio.inited)
        return 0;

    radio.shortAddr = addr;
    radio.shortOn = on ? 1u : 0u;
    radioWriteAddresses();
    return 1;
}

int Radio_SetExtAddr(const uint8_t ext8[8], int on)
{
    uint8_t i;

    if (!radio.inited || ext8 == 0)
        return 0;

    for (i = 0u; i < 8u; ++i)
        radio.extAddr[i] = ext8[i];

    radio.extOn = on ? 1u : 0u;

    /* The backoff generator's seed resets to the same value on every part; seeded from the address's
     * last octet in wire order, as the ConBee II does. */
    if (on)
        REG(AVR_CSMA_SEED_0) = ext8[7];

    radioWriteAddresses();
    return 1;
}

int Radio_SetSrcMatch(const SrcMatch_Table *table)
{
    uint8_t sreg;

    if (!radio.inited || table == 0)
        return 0;

    sreg = AvrOwn_LockSave();
    SrcMatch_CopyOnAir(&radio.srcMatch, table);
    AvrOwn_LockRestore(sreg);
    return 1;
}

/* ---- transmit ---- */

static void radioSetXah(uint8_t mask, uint8_t value)
{
    REG(AVR_XAH_CTRL_0) = (uint8_t)((REG(AVR_XAH_CTRL_0) & (uint8_t)~mask) | (value & mask));
}

static void radioBackoffFirst(void);

int Radio_Tx(const uint8_t *psdu, uint8_t len, int8_t dbm, int cca, int maxRetries, void *token)
{
    uint8_t i;

    if (!radio.inited || psdu == 0 || len == 0u)
        return 0;

    if (len > RADIO_TX_PSDU_MAX)
    {
        DBG(DBG_COMP_RF, DBG_RF_RSN_TX_TOO_LONG);
        return 0;
    }

    if (radio.txPhase != TX_IDLE)
        return 0;

    if (dbm != 0 && !Radio_SetPower(dbm))
        return 0;

    for (i = 0u; i < len; ++i)
        radio.txPsdu[i] = psdu[i];

    radio.txLen = len;
    radioSetXah(XAH_CSMA_RETRIES_MASK,
                (uint8_t)((cca ? XAH_CSMA_DEFAULT : XAH_CSMA_NONE) << AVR_MAX_CSMA_RETRIES0));

    radio.txAckWait = Mac_AckRequested(psdu, len) ? 1u : 0u;

    if (radio.txAckWait)
    {
        uint8_t want = (maxRetries < 0) ? 0u : ((maxRetries > 15) ? 15u : (uint8_t)maxRetries);

        if (!radio.txRetriesSet || want != radio.txRetries)
        {
            radioSetXah(XAH_FRAME_RETRIES_MASK, (uint8_t)(want << AVR_MAX_FRAME_RETRIES0));
            radio.txRetries = want;
            radio.txRetriesSet = 1u;
        }
    }

    radio.txToken = token;
    radio.txDeadline = radioNowMs() + TX_DEADLINE_MS;
    radio.txPhase = TX_TO_ARET;
    radio.txAckHave = 0u;

    /* A host that set keys needs a version 2 frame's Enhanced Ack, which ARET will not show. */
    if (radio.txAckWait && radio.macSec != 0 && len >= 3u && ((psdu[1] >> 4) & 3u) == FCF_VERSION_2015)
    {
        radio.txSeq = psdu[2];
        radio.txCca = cca ? 1u : 0u;
        radio.txTriesLeft = (uint8_t)(maxRetries < 0 ? 0 : (maxRetries > 15 ? 15 : maxRetries));
        radioBackoffFirst();
    }
    radio.counters.txFrames++;
    return 1;
}

int Radio_TxAt(const uint8_t *psdu, uint8_t len, int8_t dbm, int cca, uint32_t sfdEndUs, void *token)
{
    uint8_t i;
    int32_t lead;

    if (!radio.inited || psdu == 0 || len == 0u || len > RADIO_TX_PSDU_MAX || radio.txPhase != TX_IDLE)
        return 0;

    if (dbm != 0 && !Radio_SetPower(dbm))
        return 0;

    for (i = 0u; i < len; ++i)
        radio.txPsdu[i] = psdu[i];

    radio.txLen = len;
    radio.txAckWait = Mac_AckRequested(psdu, len) ? 1u : 0u;
    radio.txSeq = psdu[2];
    radio.txCca = cca ? 1u : 0u;
    radio.txTriesLeft = 0u;
    radio.txAtUs = sfdEndUs;
    radio.txAckHave = 0u;
    radio.txToken = token;

    /* A moment already gone is found out by the phase, and completes as radio.h says it must. */
    lead = (int32_t)(sfdEndUs - Tick_ClockUs());
    radio.txDeadline = radioNowMs() + TX_DEADLINE_MS + (lead > 0 ? (uint32_t)lead >> 9 : 0u);
    radio.txPhase = TX_TIMED;
    radio.counters.txFrames++;
    return 1;
}

const Radio_Frame *Radio_TxAck(void)
{
    return radio.txAckHave ? &radio.txAckFrame : 0;
}

int Radio_SetMacSec(MacSec_State *state)
{
    uint8_t sreg = AvrOwn_LockSave();

    radio.macSec = state;
    AvrOwn_LockRestore(sreg);
    return 1;
}

static void radioTxFinish(int status, int framePending)
{
    void (*fn)(void *, int, int) = radio.txDoneHandler;
    void *token = radio.txToken;

    if (status != RADIO_TX_OK)
        radio.counters.txFailed++;

    if (radio.rxOn)
    {
        (void)radioEnterRx();
    }
    else
    {
        uint8_t sreg = AvrOwn_LockSave();

        radio.isrPhase = ISR_OFF;
        radioCmd(CMD_TRX_OFF);
        (void)radioWaitState(ST_TRX_OFF, ST_TRX_OFF);
        AvrOwn_LockRestore(sreg);
    }

    radio.txPhase = TX_IDLE;
    radio.txToken = 0;

    if (fn)
        fn(token, status, framePending);
}

/* The vector stamps the clock's low half; the whole clock is only known out here. */
static uint64_t radioExtendUs(uint32_t low)
{
    uint64_t now = Radio_ClockUs();

    return now + (uint64_t)(int64_t)(int32_t)(low - (uint32_t)now);
}

/* From RX_ON to PLL_ON unless the radio is occupied. 1 = the interrupts are left masked, for
 * radioTxStartUpload. */
static int radioTxPrepare(void)
{
    uint8_t st;

    HAL_IrqLock();
    st = radioState();

    if (radio.isrPhase != ISR_RX || (REG(AVR_IRQ_STATUS) & AVR_BIT(AVR_RX_END)) != 0u || st == ST_BUSY_RX)
    {
        HAL_IrqUnlock();
        return 0;
    }

    radioCmd(CMD_FORCE_PLL_ON);

    if (!radioWaitState(ST_PLL_ON, ST_PLL_ON))
    {
        HAL_IrqUnlock();
        return 0;
    }

    radio.isrPhase = ISR_OFF;
    return 1;
}

/* TX_START, then the frame behind it: the stores outrun the air, and the PHR has 176 us (datasheet 9.4.2), so
 * nothing may come between them. The interrupts stay masked through the upload, at most 100 us. */
static void radioTxStartUpload(void)
{
    uint8_t i;

    radio.txEvent = TXEV_NONE;
    radio.isrPhase = ISR_TX;
    radioCmd(CMD_TX_START);
    FB[0] = radio.txLen;

    for (i = 0u; i < radio.txLen; ++i)
        FB[1u + i] = radio.txPsdu[i];

    HAL_IrqUnlock();
    radio.txDeadline = radioNowMs() + TX_DEADLINE_MS;
    radio.txPhase = TX_SENDING;
}

static void radioCcaRequest(void)
{
    uint8_t sreg = AvrOwn_LockSave();

    REG(AVR_PHY_CC_CCA) = (uint8_t)(REG(AVR_PHY_CC_CCA) | AVR_BIT(AVR_CCA_REQUEST));
    AvrOwn_LockRestore(sreg);
}

/* 1 = the assessment finished and found the channel idle, with nothing received meanwhile. */
static int radioCcaClear(void)
{
    uint8_t st = REG(AVR_TRX_STATUS);

    return (st & AVR_BIT(AVR_CCA_DONE)) != 0u && (st & AVR_BIT(AVR_CCA_STATUS)) != 0u && (st & ST_MASK) == ST_RX_ON;
}

static void radioBackoffNext(void)
{
    radio.rng = (radio.rng ^ Tick_CycleStamp()) * 0x41C64E6DuL + 12345uL;
    radio.txAtUs = Tick_ClockUs() + (((radio.rng >> 16) & ((1uL << radio.txBe) - 1uL)) * CSMA_UNIT_US);
    radio.txPhase = TX_BACKOFF;
}

static void radioBackoffFirst(void)
{
    radio.txBe = CSMA_MIN_BE;
    radio.txBackoffs = 0u;
    radio.txDeadline = radioNowMs() + TX_DEADLINE_MS;
    radioBackoffNext();
}

/* The channel was busy, or the radio was. */
static void radioBackoffAgain(void)
{
    if (radio.txBackoffs >= CSMA_MAX_BACKOFFS)
    {
        DBG(DBG_COMP_RF, DBG_RF_RSN_TX_CCA_FAIL);
        radioTxFinish(RADIO_TX_CCA_FAIL, 0);
        return;
    }

    radio.txBackoffs++;

    if (radio.txBe < CSMA_MAX_BE)
        radio.txBe++;

    radioBackoffNext();
}

/* A transmitter that is off still listens for the moment it sends: CCA, and then the Ack. */
static int radioTxListening(void)
{
    return radio.isrPhase != ISR_OFF || radioEnterRx();
}

static void radioPollBackoff(void)
{
    uint32_t n;

    if ((int32_t)(Tick_ClockUs() - radio.txAtUs) < 0)
        return;

    if (!radioTxListening())
    {
        radioTxFinish(RADIO_TX_TIMEOUT, 0);
        return;
    }

    if (radio.txCca)
    {
        radioCcaRequest();
        n = Tick_ClockUs();

        while ((int32_t)(Tick_ClockUs() - n) < (int32_t)CCA_US)
        {
        }

        if (!radioCcaClear())
        {
            radioBackoffAgain();
            return;
        }
    }

    if (!radioTxPrepare())
    {
        radioBackoffAgain();
        return;
    }

    radioTxStartUpload();
}

/* One attempt. The moment is honoured to the microsecond when nothing else runs in its last stretch, so the
 * wait ends with interrupts masked; anything that occupies the radio then has cost the window, which completes
 * as radio.h says it must. */
static void radioPollTimed(void)
{
    uint32_t start = radio.txAtUs - TX_TO_SFD_END_US;
    uint32_t need = TIMED_PLL_US + (radio.txCca ? CCA_US : 0u);
    int32_t left = (int32_t)(start - Tick_ClockUs());

    if (left < (int32_t)need)
    {
        radioTxFinish(RADIO_TX_CCA_FAIL, 0);
        return;
    }

    if (left > (int32_t)TIMED_SPIN_US)
        return;

    if (!radioTxListening())
    {
        radioTxFinish(RADIO_TX_TIMEOUT, 0);
        return;
    }

    while ((int32_t)(start - Tick_ClockUs()) > (int32_t)need)
    {
    }

    if (radio.txCca)
    {
        radioCcaRequest();

        while ((int32_t)(start - Tick_ClockUs()) > (int32_t)TIMED_PLL_US)
        {
        }

        if (!radioCcaClear())
        {
            radioTxFinish(RADIO_TX_CCA_FAIL, 0);
            return;
        }
    }

    if (!radioTxPrepare())
    {
        radioTxFinish(RADIO_TX_CCA_FAIL, 0);
        return;
    }

    if ((int32_t)(start - Tick_ClockUs()) < 0)
    {
        radioCmd(CMD_RX_ON);
        radio.isrPhase = ISR_RX;
        HAL_IrqUnlock();
        radioTxFinish(RADIO_TX_CCA_FAIL, 0);
        return;
    }

    while ((int32_t)(start - Tick_ClockUs()) > 0)
    {
    }

    radioTxStartUpload();
}

/* The vector carries the frame and its Ack; this decides what became of them. */
static void radioPollSending(void)
{
    uint8_t event = radio.txEvent;
    uint8_t sreg;

    if (event == TXEV_SENT)
    {
        radioTxFinish(RADIO_TX_OK, 0);
        return;
    }

    if (event == TXEV_NONE)
        return;

    if (event == TXEV_ACKED)
    {
        radio.txAckFrame.timestamp = radioExtendUs((uint32_t)radio.txAckFrame.timestamp);
        radio.txAckHave = 1u;
        radioTxFinish(RADIO_TX_OK, (radio.txAckFrame.psdu[0] & FCF_FRAME_PENDING) ? 1 : 0);
        return;
    }

    if (radio.txPhase != TX_ACK_WAIT)
    {
        radio.txPhase = TX_ACK_WAIT;
        radio.txAckExtraUs = 0u;
    }

    if ((int32_t)(Tick_ClockUs() - (radio.txEndUs + ACK_WAIT_US + radio.txAckExtraUs)) < 0)
        return;

    /* A reception under way by then could be the Ack: it gets the time it needs, once. One that has just ended
     * is on its way through the vector, so the wait never ends on the first look. */
    sreg = AvrOwn_LockSave();

    if (radio.txAckExtraUs == 0u)
    {
        radio.txAckExtraUs = radioState() == ST_BUSY_RX ? ACK_RX_US : ACK_END_US;
        AvrOwn_LockRestore(sreg);
        return;
    }

    if (radio.txEvent == TXEV_ACKED)
    {
        AvrOwn_LockRestore(sreg);
        return;
    }

    radio.isrPhase = ISR_RX;
    AvrOwn_LockRestore(sreg);

    if (radio.txTriesLeft == 0u)
    {
        DBG(DBG_COMP_RF, DBG_RF_RSN_TX_NO_ACK);
        radioTxFinish(RADIO_TX_NO_ACK, 0);
        return;
    }

    radio.txTriesLeft--;
    radioBackoffFirst();
}

static void radioPollTx(void)
{
    uint8_t sreg;
    uint8_t st;
    uint8_t i;

    if ((int32_t)(radioNowMs() - radio.txDeadline) >= 0)
    {
        DBG(DBG_COMP_RF, DBG_RF_RSN_TX_TIMEOUT);
        radioTxFinish(RADIO_TX_TIMEOUT, 0);
        return;
    }

    switch (radio.txPhase)
    {
        case TX_TO_ARET:
            /* The radio is taken from the vector only between frames: not while an Ack is on air, not
             * while a received frame waits for its vector, not mid-reception. TX_ARET_ON is entered
             * from PLL_ON or TRX_OFF. */
            sreg = AvrOwn_LockSave();
            st = radioState();

            if (radio.isrPhase == ISR_ACK || (REG(AVR_IRQ_STATUS) & AVR_BIT(AVR_RX_END)) != 0u ||
                st == ST_BUSY_RX)
            {
                AvrOwn_LockRestore(sreg);
                return;
            }

            radio.isrPhase = ISR_OFF;

            if (st != ST_TRX_OFF)
            {
                radioCmd(CMD_FORCE_PLL_ON);

                if (!radioWaitState(ST_PLL_ON, ST_PLL_ON))
                {
                    AvrOwn_LockRestore(sreg);
                    radioTxFinish(RADIO_TX_TIMEOUT, 0);
                    return;
                }
            }

            REG(AVR_IRQ_STATUS) = AVR_BIT(AVR_TX_END);
            radio.irqMirror = (uint8_t)(radio.irqMirror & (uint8_t)~AVR_BIT(AVR_TX_END));
            radioCmd(CMD_TX_ARET_ON);
            AvrOwn_LockRestore(sreg);
            radio.txDeadline = radioNowMs() + STATE_DEADLINE_MS;
            radio.txPhase = TX_WAIT_ARET;
            break;

        case TX_WAIT_ARET:
            st = radioState();

            if (st != ST_TX_ARET_ON && st != ST_BUSY_TX_ARET)
                return; /* the deadline above bounds this */

            /* The frame buffer takes the PHR first; the transceiver writes the FCS into the last two. */
            FB[0] = radio.txLen;

            for (i = 0u; i < radio.txLen; ++i)
                FB[1u + i] = radio.txPsdu[i];

            radioCmd(CMD_TX_START);
            radio.txDeadline = radioNowMs() + TX_DEADLINE_MS * (radio.txAckWait ? radio.txRetries + 1u : 1u);
            radio.txPhase = TX_WAIT_END;
            break;

        case TX_WAIT_END:
            /* TX_END comes after the acknowledgment, or right after a frame that asked for none; the
             * transaction's outcome is TRAC_STATUS. */
            if ((radio.irqBits & AVR_BIT(AVR_TX_END)) == 0u)
                return;

            radio.irqBits = (uint8_t)(radio.irqBits & (uint8_t)~AVR_BIT(AVR_TX_END));
            st = (uint8_t)(REG(AVR_TRX_STATE) >> AVR_TRAC_STATUS0);

            if (st == TRAC_SUCCESS || st == TRAC_SUCCESS_PD)
            {
                radioTxFinish(RADIO_TX_OK, st == TRAC_SUCCESS_PD ? 1 : 0);
            }
            else if (st == TRAC_NO_ACK)
            {
                DBG(DBG_COMP_RF, DBG_RF_RSN_TX_NO_ACK);
                radioTxFinish(RADIO_TX_NO_ACK, 0);
            }
            else if (st == TRAC_CHAN_FAIL)
            {
                DBG(DBG_COMP_RF, DBG_RF_RSN_TX_CCA_FAIL);
                radioTxFinish(RADIO_TX_CCA_FAIL, 0);
            }
            else
            {
                DBG(DBG_COMP_RF, DBG_RF_RSN_TX_TIMEOUT);
                radioTxFinish(RADIO_TX_TIMEOUT, 0);
            }

            break;

        case TX_BACKOFF:
            radioPollBackoff();
            break;

        case TX_TIMED:
            radioPollTimed();
            break;

        case TX_SENDING:
        case TX_ACK_WAIT:
            radioPollSending();
            break;

        default:
            break;
    }
}

/* ---- energy detection ---- */

int Radio_StartEd(void)
{
    uint8_t st;

    if (!radio.inited || radio.txPhase != TX_IDLE || radio.edState == RADIO_ED_MEASURING)
        return 0;

    st = radioState();

    if (st != ST_RX_ON && st != ST_BUSY_RX)
    {
        Debug_Emit(DBG_COMP_RF, st, DBG_RF_RSN_ED_BAD_STATE);
        return 0;
    }

    radio.irqMirror = (uint8_t)(radio.irqMirror & (uint8_t)~AVR_BIT(AVR_CCA_ED_DONE));
    /* A transmit's CCA raises the same cause; what it left is not this measurement's. */
    radio.irqBits = (uint8_t)(radio.irqBits & (uint8_t)~AVR_BIT(AVR_CCA_ED_DONE));
    REG(AVR_PHY_ED_LEVEL) = ED_TRIGGER_SENTINEL;
    radio.edState = RADIO_ED_MEASURING;
    radio.edFresh = 0u;
    radio.edDeadline = radioNowMs() + ED_CONFIRM_MS;
    radio.counters.edRequests++;
    return 1;
}

int Radio_TakeEd(int8_t *dbm, int *fresh)
{
    if (dbm == 0)
        return 0;

    if (radio.edState == RADIO_ED_READY)
    {
        *dbm = radio.edDbm;

        if (fresh != 0)
            *fresh = radio.edFresh;

        radio.edState = RADIO_ED_IDLE;
        return 1;
    }

    if (radio.edState == RADIO_ED_FAILED)
    {
        radio.edState = RADIO_ED_IDLE;
        radio.edFresh = 0u;
        return -1;
    }

    return 0;
}

void Radio_AbortEd(void)
{
    radio.edState = RADIO_ED_IDLE;
    radio.edFresh = 0u;
}

static void radioPollEd(void)
{
    uint8_t level;

    if (radio.edState != RADIO_ED_MEASURING)
        return;

    if ((radio.irqBits & AVR_BIT(AVR_CCA_ED_DONE)) == 0u)
    {
        if ((int32_t)(radioNowMs() - radio.edDeadline) >= 0)
        {
            Debug_Emit(DBG_COMP_RF, 0u, DBG_RF_RSN_ED_TIMEOUT);
            radio.edState = RADIO_ED_FAILED;
        }

        return;
    }

    radio.irqBits = (uint8_t)(radio.irqBits & (uint8_t)~AVR_BIT(AVR_CCA_ED_DONE));
    level = REG(AVR_PHY_ED_LEVEL);

    if (level == ED_TRIGGER_SENTINEL || level == ED_LEVEL_INVALID)
    {
        Debug_Emit(DBG_COMP_RF, level, DBG_RF_RSN_ED_NO_MEASURE);
        radio.edState = RADIO_ED_FAILED;
        return;
    }

    if (level > ED_LEVEL_MAX)
        level = ED_LEVEL_MAX;

    /* A level read while a frame is coming in belongs to that frame. */
    radio.edFresh = (radioState() == ST_BUSY_RX) ? 0u : 1u;
    radio.edDbm = (int8_t)(ED_BASE_DBM + (int16_t)level);
    radio.edState = RADIO_ED_READY;
}

/* ---- the vectors ---- */

static int radioRxRoom(void)
{
    return (uint8_t)(radio.rxHead - radio.rxTail) < RADIO_RX_SLOTS;
}

/* An Ack: FORCE_PLL_ON first, so a frame behind this one cannot land in the buffer, then the octets
 * [PHR, MPDU without its FCS], then TX_START on schedule. What of the received frame these overwrite has
 * already been copied out. */
static int radioIsrAckStart(uint32_t stamp, const uint8_t *buf, uint8_t n)
{
    uint32_t elapsed;
    uint8_t i;

    radioCmd(CMD_FORCE_PLL_ON);

    for (i = 0u; i < n; ++i)
        FB[i] = buf[i];

    if (!radioWaitState(ST_PLL_ON, ST_PLL_ON))
    {
        radio.isrStateFail++;
        return 0;
    }

    do
    {
        elapsed = Tick_CyclesSince(stamp);
    } while (elapsed < ACK_TX_START_US * CYCLES_PER_US);

    if (elapsed > ACK_LATEST_US * CYCLES_PER_US)
    {
        radio.isrAcksLate++;
        return 0;
    }

    radioCmd(CMD_TX_START);
    radio.lastAckCycles = (uint16_t)elapsed;
    return 1;
}

static int radioIsrAck(uint32_t stamp, uint8_t seq, int framePending)
{
    uint8_t ack[MAC_ACK_LEN];

    Mac_BuildImmAck(seq, framePending, ack);
    return radioIsrAckStart(stamp, ack, MAC_ACK_LEN);
}

/* The Enhanced Ack of a version 2 frame, secured when the frame was. It starts without its MIC, which follows
 * into the buffer while the header goes out: the transceiver reads the buffer as it transmits. Returns 1 when
 * the Ack went on air, with what the MIC needs in later and ack. */
static int radioIsrEnhAck(uint32_t stamp,
                          const Radio_Frame *frame,
                          uint8_t avail,
                          const MacSec_AckRx *rx,
                          int fp,
                          uint8_t *ack,
                          MacSec_AckResult *result,
                          MacSec_AckMic *later)
{
    uint8_t ie[MAC_SEC_PROBE_IE_MAX];
    uint8_t ieLen;
    uint8_t n;

    ieLen = MacSec_ProbeIe(radio.macSec, rx, frame->lqi, frame->rssi, Board_Get()->rxSensitivityDbm, ie);
    n = MacSec_BuildEnhAckHeader(radio.macSec, frame->psdu, avail, radio.extAddr, fp, ie, ieLen, &ack[1], result, later);

    if (n == 0u)
        return 0;

    ack[0] = n;
    return radioIsrAckStart(stamp, ack, (uint8_t)(1u + n - FCS_LEN));
}

/* The MIC takes some 550 us on this core (docs/hardware_avr.md §11.7), far longer than the USART holds what
 * arrives at the host link's rate, so it runs with the short vectors let in. None of them touches the engine:
 * a block of the loop's has finished before this vector runs, and this one's blocks hold the interrupts
 * themselves. The radio's own vectors stay quiet while the Ack is on air; one that came all the same finds the
 * phase already set. An Ack that ends before the MIC is in leaves the receiver off (the TX_END vector): a frame
 * received now would overwrite the tail still in the buffer and, through a nested RX_END, the slot this vector
 * has not published. radioIsrRxEnd turns the receiver on once both are done. */
static void radioIsrEnhAckMic(const MacSec_AckMic *later, const uint8_t *ack)
{
    uint8_t mic[4];
    uint8_t i;

    radio.micOpen = 1u;
    HAL_IrqUnlock();

    if (MacSec_EnhAckMic(later, &ack[1], mic))
    {
        for (i = 0u; i < 4u; ++i)
            FB[1u + later->adataLen + i] = mic[i];
    }
    else
    {
        radio.isrStateFail++;
    }

    HAL_IrqLock();
    radio.micOpen = 0u;
}

/* One received frame. Its length, FCS validity and energy are read first, while they still describe
 * it; the header is parsed and the Ack decided in place; the frame is copied into the ring slot; then
 * the Ack. The buffer is released by the vector afterwards, whatever this decided. */
static void radioIsrRxEnd(uint32_t stamp)
{
    uint8_t len = REG(AVR_TST_RX_LENGTH);
    uint8_t ed = REG(AVR_PHY_ED_LEVEL);
    int crcOk = (REG(AVR_PHY_RSSI) & AVR_BIT(AVR_RX_CRC_VALID)) != 0u;
    const uint8_t *psdu = (const uint8_t *)FB;
    Radio_Frame *dst;
    Mac_Hdr hdr;
    int parsed;
    int fp = 0;
    int ackDue;
    uint32_t t0;
    uint8_t i;
    uint8_t head = len;
    uint8_t enh;
    MacSec_AckRx rx;
    MacSec_AckMic later;
    MacSec_AckResult result;
    uint8_t ack[1u + MAC_SEC_ACK_MAX];
    uint32_t stampUs;

    later.adataLen = 0u;

    if (len == 0u || len > RADIO_PSDU_MAX)
    {
        radio.isrReadFail++;
        return;
    }

    radio.isrFrames++;

    if (!crcOk)
        radio.isrCrcBad++;

    if (!crcOk && radio.rxFilter != RADIO_RX_ALL)
    {
        radio.counters.rxUnfiltered++;
        return;
    }

    parsed = Mac_ParseHeader(psdu, len, &hdr);

    if (radio.rxFilter == RADIO_RX_FILTERED && (!parsed || !Mac_Filter(&hdr, &radio.ackCfg.filter)))
    {
        radio.counters.rxFiltered++;
        return;
    }

    ackDue = parsed && Mac_AckDecide(&hdr, psdu, len, crcOk, &radio.ackCfg, &radio.srcMatch, &fp);

    if (!radioRxRoom())
    {
        /* The host never sees this frame, so it is not acknowledged either. */
        if (ackDue)
            radio.isrAckNoRoom++;

        radio.counters.rxDropped++;
        return;
    }

    t0 = Tick_CycleStamp();
    dst = &radio.rxRing[radio.rxHead % RADIO_RX_SLOTS];

    /* An Enhanced Ack has the longest way to its slot, so what it does not overwrite waits behind TX_START. */
    enh = ackDue && hdr.version == FCF_VERSION_2015 && radio.macSec != 0;

    if (enh && head > ENH_ACK_HEAD)
        head = ENH_ACK_HEAD;

    for (i = 0u; i < head; ++i)
        dst->psdu[i] = FB[i];

    dst->len = len;
    dst->lqi = FB[len];
    dst->rssi = (int8_t)(ED_BASE_DBM + (int16_t)(ed > ED_LEVEL_MAX ? ED_LEVEL_MAX : ed));
    dst->channel = radio.channel;
    dst->ackedSec = 0;
    dst->ackKeyIndex = 0u;
    dst->ackFrameCounter = 0u;
    dst->crcOk = crcOk;
    dst->ackedFp = 0;
    radio.lastReadCycles = (uint16_t)Tick_CyclesSince(t0);

    if (ackDue)
    {
        int sent;

        if (enh && MacSec_ParseAckRx(dst->psdu, head, &rx))
        {
            fp = MacSec_AckPending(&rx, &radio.srcMatch);
            sent = radioIsrEnhAck(stamp, dst, head, &rx, fp, ack, &result, &later);

            if (sent)
            {
                dst->ackedSec = result.secured;
                dst->ackKeyIndex = result.keyIndex;
                dst->ackFrameCounter = result.frameCounter;
            }
        }
        else
            sent = radioIsrAck(stamp, hdr.seq, fp);

        if (sent)
        {
            radio.isrPhase = ISR_ACK;
            radio.ackSince = radioNowMs();
            radio.isrAcks++;

            if (fp)
                radio.isrAcksPending++;

            dst->ackedFp = fp;
        }
        else
        {
            radioCmd(CMD_RX_ON);
        }
    }

    /* The MIC first: it has a moment to meet. The frame's tail stays in the buffer while the Ack is on air. */
    if (later.adataLen != 0u && radio.isrPhase == ISR_ACK)
    {
        uint16_t cycles;

        radioIsrEnhAckMic(&later, ack);
        cycles = (uint16_t)Tick_CyclesSince(stamp);

        if (cycles > radio.maxMicCycles)
            radio.maxMicCycles = cycles;
    }

    for (i = head; i < len; ++i)
        dst->psdu[i] = FB[i];

    /* The frame's clock, taken only now: the stamp's distance is the stopwatch's, which is cheaper than the
     * clock ahead of the Ack's slot. */
    stampUs = Tick_ClockUs() - (Tick_CyclesSince(stamp) >> 4);
    dst->timestamp = stampUs - RX_END_TIRQ_US - ((uint32_t)len + 1u) * OCTET_US;
    radio.rxHead = (uint8_t)(radio.rxHead + 1u);

    if (radio.ackEndedInMic)
    {
        radio.ackEndedInMic = 0u;
        radioCmd(CMD_RX_ON);
        radio.isrPhase = ISR_RX;
    }
}

/* RX_END while our frame's Ack is awaited. Anything else that arrived is dropped, unacknowledged, as ARET
 * drops it (datasheet 9.8.4): its sender tries again. */
static void radioIsrTxAck(uint32_t stamp)
{
    uint8_t len = REG(AVR_TST_RX_LENGTH);
    uint8_t ed = REG(AVR_PHY_ED_LEVEL);
    Radio_Frame *ack = &radio.txAckFrame;
    uint32_t stampUs;
    uint8_t i;

    if ((REG(AVR_PHY_RSSI) & AVR_BIT(AVR_RX_CRC_VALID)) == 0u || len < 5u || len > RADIO_PSDU_MAX ||
        (FB[0] & FCF_TYPE_MASK) != FCF_TYPE_ACK || FB[2] != radio.txSeq)
        return;

    stampUs = Tick_ClockUs() - (Tick_CyclesSince(stamp) >> 4);
    ack->len = len;
    ack->rssi = (int8_t)(ED_BASE_DBM + (int16_t)(ed > ED_LEVEL_MAX ? ED_LEVEL_MAX : ed));
    ack->lqi = FB[len];
    ack->channel = radio.channel;
    ack->timestamp = stampUs - RX_END_TIRQ_US - ((uint32_t)len + 1u) * OCTET_US;
    ack->crcOk = 1;
    ack->ackedFp = 0;
    ack->ackedSec = 0;

    for (i = 0u; i < len; ++i)
        ack->psdu[i] = FB[i];

    radio.isrPhase = ISR_RX;
    radio.txEvent = TXEV_ACKED;
}

AVR_ISR(AVR_TRX24_RX_END_vect_num)
{
    uint32_t stamp = Tick_CycleStamp();

    HAL_IrqEnter();

    if (radio.isrPhase == ISR_RX)
        radioIsrRxEnd(stamp);
    else if (radio.isrPhase == ISR_TX_ACK)
        radioIsrTxAck(stamp);
    else
        radio.irqMirror = (uint8_t)(radio.irqMirror | AVR_BIT(AVR_RX_END));

    /* Released on every path: a valid frame stays protected until RX_SAFE_MODE is cleared (datasheet
     * 9.8.7), so a frame the filter dropped, or one the full ring refused, would otherwise keep every
     * later frame out of the buffer. Measured: one dropped foreign-PAN frame left the receiver deaf
     * until the next mode change. */
    REG(AVR_TRX_CTRL_2) = (uint8_t)(REG(AVR_TRX_CTRL_2) & (uint8_t)~AVR_BIT(AVR_RX_SAFE_MODE));
    REG(AVR_TRX_CTRL_2) = (uint8_t)(REG(AVR_TRX_CTRL_2) | AVR_BIT(AVR_RX_SAFE_MODE));

    HAL_IrqExit();
}

AVR_ISR(AVR_TRX24_TX_END_vect_num)
{
    HAL_IrqEnter();

    if (radio.isrPhase == ISR_ACK && radio.micOpen)
    {
        /* Inside the RX_END vector's MIC: it turns the receiver on (radioIsrEnhAckMic). */
        radio.ackEndedInMic = 1u;
    }
    else if (radio.isrPhase == ISR_ACK)
    {
        radioCmd(CMD_RX_ON);
        radio.isrPhase = ISR_RX;
    }
    else if (radio.isrPhase == ISR_TX)
    {
        radioCmd(CMD_RX_ON);
        radio.txEndUs = Tick_ClockUs();

        if (radio.txAckWait)
        {
            radio.isrPhase = ISR_TX_ACK;
            radio.txEvent = TXEV_WAIT;
        }
        else
        {
            radio.isrPhase = ISR_RX;
            radio.txEvent = TXEV_SENT;
        }
    }
    else
    {
        radio.irqMirror = (uint8_t)(radio.irqMirror | AVR_BIT(AVR_TX_END));
    }

    HAL_IrqExit();
}

AVR_ISR(AVR_TRX24_CCA_ED_DONE_vect_num)
{
    HAL_IrqEnter();
    radio.irqMirror = (uint8_t)(radio.irqMirror | AVR_BIT(AVR_CCA_ED_DONE));
    HAL_IrqExit();
}

/* ---- the loop's side ---- */

static void radioRxDeliver(void)
{
    Radio_Frame *f;

    if (radio.rxHead == radio.rxTail || radio.rxHandler == 0)
        return;

    f = &radio.rxRing[radio.rxTail % RADIO_RX_SLOTS];
    radio.counters.rxFrames++;
    f->timestamp = radioExtendUs((uint32_t)f->timestamp);

    if (!f->crcOk)
    {
        radio.counters.rxCrcBad++;
        DBG(DBG_COMP_RF, DBG_RF_RSN_RX_CRC_BAD);
    }

    radio.rxHandler(f);
    radio.rxTail = (uint8_t)(radio.rxTail + 1u);
}

void Radio_Poll(void)
{
    uint8_t sreg;

    if (!radio.inited)
        return;

    (void)Radio_ClockUs(); /* often enough never to miss the 32-bit clock's wrap */

    sreg = AvrOwn_LockSave();
    radio.irqBits = (uint8_t)(radio.irqBits | radio.irqMirror);
    radio.irqMirror = 0u;

    /* An Ack whose TX_END never came would leave the vector waiting for it for ever. */
    if (radio.isrPhase == ISR_ACK && (int32_t)(radioNowMs() - (radio.ackSince + ACK_STUCK_MS)) >= 0)
    {
        radio.isrAckStuck++;
        radioCmd(CMD_FORCE_PLL_ON);
        radioCmd(CMD_RX_ON);
        radio.isrPhase = ISR_RX;
    }

    AvrOwn_LockRestore(sreg);

    if (radio.txPhase != TX_IDLE)
        radioPollTx();

    radioPollEd();
    radioRxDeliver();
}

void Radio_GetCounters(Radio_Counters *out)
{
    if (out == 0)
        return;

    out->rxFrames = radio.counters.rxFrames;
    out->rxDropped = radio.counters.rxDropped;
    out->rxCrcBad = radio.counters.rxCrcBad;
    out->rxUnfiltered = radio.counters.rxUnfiltered;
    out->txFrames = radio.counters.txFrames;
    out->txFailed = radio.counters.txFailed;
    out->edRequests = radio.counters.edRequests;
    out->rxRingFull = radio.counters.rxRingFull;
    out->isrSpurious = radio.counters.isrSpurious;
    out->ackSent = radio.isrAcks;
    out->ackFp = radio.isrAcksPending;
    out->ackLate = radio.isrAcksLate;
    out->ackNotPllOn = radio.isrStateFail;
    out->rxFiltered = radio.counters.rxFiltered;
}

void Radio_SetRxHandler(void (*fn)(const Radio_Frame *frame))
{
    radio.rxHandler = fn;
}

void Radio_SetTxDoneHandler(void (*fn)(void *token, int status, int framePending))
{
    radio.txDoneHandler = fn;
}

uint64_t Radio_ClockUs(void)
{
    uint32_t now = Tick_ClockUs();

    if (now < radio.clockLast)
        radio.clockHigh++;

    radio.clockLast = now;
    return ((uint64_t)radio.clockHigh << 32) | now;
}

/* Microseconds at 1 ms resolution, from the board's millisecond (radio.h). */
uint32_t Radio_Now(void)
{
    return radioNowMs() * 1000u;
}
