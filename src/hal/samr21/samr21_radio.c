/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* samr21_radio.c — AT86RF233 back-end for hal/radio.h.
 *
 * Reception runs in the radio vector (EIC EXTINT0, the transceiver's IRQ pin): the handler
 * reads each frame at TRX_END, filters it the way the transceiver's own third level filter
 * would, and - in the filtered mode - composes the immediate Ack itself and starts it inside
 * the 192 us turnaround, with a Frame Pending bit decided from the host's source-match table
 * (docs/ack-pending-frame.md §4). The transceiver therefore receives in basic RX_ON in every
 * mode and never acknowledges on its own. Transmission is a polled four-phase state machine
 * in thread context through TX_ARET_ON. SPI access is samr21_rf.h; thread code holds the SPI
 * against the vector through Rf_Lock. */
#include <stdint.h>

#include "board.h"
#include "hal.h"
#include "hal_diag.h"
#include "dbg.h"
#include "radio.h"
#include "mac.h"
#include "samr21_rf.h"
#include "u_bstream.h"
#ifdef RCP_TX_TIMING
#include "mac_sec.h"
#endif

/* ---- AT86RF233 registers (at86rf233.txt; samr21At86rf233.h:56-104) ---- */
#define RADIO_REG_PHY_TX_PWR 0x05u
#define RADIO_REG_PHY_RSSI 0x06u
#define RADIO_REG_PHY_ED_LEVEL 0x07u
#define RADIO_REG_PHY_CC_CCA 0x08u
#define RADIO_REG_CCA_THRES 0x09u
#define RADIO_REG_TRX_CTRL_1 0x04u
#define RADIO_REG_IRQ_MASK 0x0Eu
#define RADIO_REG_TRX_CTRL_2 0x0Cu
#define RADIO_REG_SHORT_ADDR_0 0x20u
#define RADIO_REG_PAN_ID_0 0x22u
#define RADIO_REG_IEEE_ADDR_0 0x24u
#define RADIO_REG_TRX_STATE 0x02u
#define RADIO_REG_XAH_CTRL_0 0x2Cu
#define RADIO_REG_CSMA_SEED_0 0x2Du
/* TRX_STATE bits 7:5 = TRAC_STATUS: how the last ARET transaction ended
 * (BitCloud at86rf233.h:164-170). */
#define RADIO_TRAC_STATUS_MASK 0xE0u
#define RADIO_TRAC_STATUS_SHIFT 5
#define RADIO_TRAC_SUCCESS 0u
#define RADIO_TRAC_SUCCESS_PD 1u
#define RADIO_TRAC_CHAN_FAIL 3u
#define RADIO_TRAC_NO_ACK 5u
/* XAH_CTRL_0: bits 7:4 MAX_FRAME_RETRIES, bits 3:1 MAX_CSMA_RETRIES, bit 0
 * SLOTTED_OPERATION (at86rf233.txt:8385-8541; BitCloud
 * REG_XAH_CTRL_0_MAX_FRAME_RETRIES_MASK 0xF0 /
 * REG_XAH_CTRL_0_MAX_CSMA_RETRIES_MASK 0x0E). MAX_CSMA_RETRIES = 7 transmits
 * immediately, without CSMA-CA (at86rf233.txt:8537-8539). */
#define RADIO_XAH_CTRL_0_RETRIES_MASK 0xF0u
#define RADIO_XAH_CTRL_0_RETRIES_SHIFT 4
#define RADIO_XAH_CTRL_0_CSMA_MASK 0x0Eu
#define RADIO_XAH_CTRL_0_CSMA_SHIFT 1
#define RADIO_XAH_CTRL_0_CSMA_NONE 7u
#define RADIO_XAH_CTRL_0_CSMA_DEFAULT 4u

/* ---- TRX_STATUS / TRX_CMD values (Table 7-3 / Table 7-4) ---- */
#define RADIO_ST_TRX_OFF 0x08u
#define RADIO_ST_PLL_ON 0x09u
#define RADIO_ST_RX_ON 0x06u
#define RADIO_ST_BUSY_TX 0x02u
#define RADIO_ST_BUSY_RX 0x01u /* RX_ON caught a valid SHR (Table 7-3; measured, docs/hardware_samr21.md §6) */
#define RADIO_ST_TX_ARET_ON 0x19u
#define RADIO_ST_BUSY_TX_ARET 0x12u
#define RADIO_ST_TRANSITION 0x1Fu
#define RADIO_ST_MASK 0x1Fu
#define RADIO_CMD_TX_START 0x02u
#define RADIO_CMD_RX_ON 0x06u
/* Commands on this part: NOP 0, TX_START 2, FORCE_TRX_OFF 3, FORCE_PLL_ON 4. */
#define RADIO_CMD_FORCE_TRX_OFF 0x03u
/* FORCE_PLL_ON aborts a reception in progress, which is what the Ack needs: once the frame it
 * answers is read, the buffer is about to carry the Ack (at86rf233 datasheet 7.1.3). */
#define RADIO_CMD_FORCE_PLL_ON 0x04u
/* TX_ARET_ON: command and status are the same value (BitCloud at86rf233.h:181, :150). */
#define RADIO_CMD_TX_ARET_ON 0x19u
#define RADIO_CMD_TRX_OFF 0x08u
#define RADIO_CMD_PLL_ON 0x09u

/* TRX_CTRL_1 bit 5: automatic FCS generation (Table 7-18). Must be set: it replaces
 * the PSDU's last two octets with the checksum Rf_FrameBufferWrite's PHR counts. */
#define RADIO_TRX_CTRL_1_CRC_ON 0x20u
/* TRX_CTRL_1 bit 7 = PA_EXT_EN: makes DIG3/DIG4 a differential RX/TX indicator that
 * drives the external PA, LNA and RF switches. With the bit clear the front end is
 * off and the radio runs bare, so the ConBee II's PA/LNA never engage (Section 11.5.1,
 * at86rf233.txt:17313-17334; reset value 0, at86rf233.txt:2660-2690). The indicator reaches the
 * front end only once Rf_FrontEndInit has routed it and switched the board's front end on. */
#define RADIO_TRX_CTRL_1_PA_EXT_EN 0x80u

/* PHY_RSSI bit 7 = RX_CRC_VALID, bits [4:0] = 5-bit RSSI (Section 8.4.2; Table 6-3 for the
 * frame buffer's copy). RSSI 0 means "<= -94 dBm", each step 3 dB: PRF = -94 + 3 x RSSI
 * (at86rf233.txt:11236-11239). */
#define RADIO_PHY_RSSI_CRC_VALID 0x80u
#define RADIO_PHY_RSSI_MASK 0x1Fu
#define RADIO_RSSI_BASE_DBM (-94)

/* PHY_CC_CCA: bit 7 CCA_REQUEST (unused now: TX_ARET runs the CCA), bits [6:5]
 * CCA_MODE, bits [4:0] CHANNEL (at86rf233.txt:11828-11860). CCA_MODE 1 = "energy
 * above threshold" is the mode the ARET CSMA-CA assesses with. */
/* TRX_CTRL_2 bit 7: Dynamic Frame Buffer Protection. In RX_ON a frame with a valid FCS is
 * protected from the next one until its Frame Buffer read ends (at86rf233 datasheet 11.8,
 * Figure 11-27). Bit 5 is OQPSK_SCRAM_EN, set at reset, and is not touched. */
#define RADIO_TRX_CTRL_2_RX_SAFE_MODE 0x80u

#define RADIO_CCA_MODE_ENERGY 0x20u
/* CCA_THRES bits 3:0 CCA_ED_THRES: busy above RSSI_BASE_VAL + 2 dB x CCA_ED_THRES; the reset value 7
 * is -80 dBm (at86rf233.txt:11971-12056). Bits 7:4 are reserved and kept as read. */
#define RADIO_CCA_ED_THRES_MASK 0x0Fu
#define RADIO_CCA_ED_THRES_MAX 15
#define RADIO_CHANNEL_MASK 0x1Fu

/* IRQ_STATUS bits this driver consumes. One read per Radio_Poll pass serves them all:
 * reading IRQ_STATUS clears the register and the pin, so two consumers reading it themselves
 * steal each other's bits - which is how an ED completion went unnoticed. */
#define RADIO_IRQ_TRX_END 0x08u
#define RADIO_IRQ_ED_DONE 0x10u
/* The causes that drive the IRQ pin. Every cause the driver consumes is here, because the
 * handler is the only reader of IRQ_STATUS: a cause that does not raise the pin would still be
 * cleared by the handler's read and has to reach the thread through the mirror anyway. */
#define RADIO_IRQ_MASK_USED (RADIO_IRQ_TRX_END | RADIO_IRQ_ED_DONE)

/* Frame Buffer receive status byte, bit 7 = RX_CRC_VALID (at86rf233 datasheet Table 6-3). */
#define RADIO_RX_STATUS_CRC_VALID 0x80u

/* The immediate Ack's schedule, from the end of the frame it answers. IEEE 802.15.4 sends the
 * Ack aTurnaroundTime = 12 symbols = 192 us after the last symbol; TX_START puts the first
 * preamble symbol on air 16 us later (PA ramp, at86rf233 datasheet 7.1.3), so TX_START is due
 * at 176 us. TRX_END reaches the pin tIRQ = 9 us after the frame ends, and the handler's
 * stamp is taken no earlier than that, so the delay counted from the stamp is 167 us and any
 * entry latency only makes the Ack later, never early. An Ack is abandoned when it could no
 * longer start by 450 us: the sender stops listening 864 us after its frame and the Ack
 * occupies 352 us of that (docs/ack-pending-frame.md §3). */
#define RADIO_ACK_TX_START_US 167u
#define RADIO_ACK_LATEST_US 441u
/* Processor cycles per microsecond of the cycle stopwatch (hal.h Tick_CycleStamp). */
#define RADIO_CYCLES_PER_US 48u
/* An Ack in flight longer than this has lost its TRX_END: the thread returns the radio to RX. */
#define RADIO_ACK_STUCK_MS 3u

#ifdef RCP_TX_TIMING
/* A frame's clock: TRX_END reaches the pin tIRQ = 9 us after its last symbol, the PHR and PSDU take
 * 32 us an octet behind the SFD, and TX_START puts the end of the SFD on air 176 us later: 16 us of PA
 * ramp and 160 us of SHR (at86rf233 datasheet 7.1.3, 10.2). */
#define RADIO_TRX_END_TIRQ_US 9u
#define RADIO_OCTET_US 32u
#define RADIO_TX_TO_SFD_END_US 176u

/* The longer a timed frame's lead, the longer the main loop is not held: this is how long ahead it starts
 * to wait in earnest, longer than a pass of the loop. RADIO_TIMED_CMD_US is what a TX_START write takes
 * to reach the transceiver, RADIO_TIMED_PLL_US the room for FORCE_PLL_ON before it. */
#define RADIO_TIMED_SPIN_US 3000u
#define RADIO_TIMED_CMD_US 11u
#define RADIO_TIMED_PLL_US 120u
/* CCA takes 8 symbols, 128 us, from the end of the request's register write. */
#define RADIO_CCA_US 180u

/* CSMA-CA with IEEE 802.15.4's defaults: macMinBE, macMaxBE, macMaxCSMABackoffs, aUnitBackoffPeriod. */
#define RADIO_CSMA_MIN_BE 3u
#define RADIO_CSMA_MAX_BE 5u
#define RADIO_CSMA_MAX_BACKOFFS 4u
#define RADIO_CSMA_UNIT_US 320u

/* An Ack starts within macAckWaitDuration of the frame's end, 864 us, and the rest of a reception
 * that has begun by then is bounded by the longest Ack. */
#define RADIO_ACK_WAIT_US 1000u
#define RADIO_ACK_RX_US 1500u
/* A secured Ack ends about when the wait does. Past its last octet TRX_STATUS has left BUSY_RX and the
 * vector has yet to take it: this is the time that needs, the IRQ's latency and the buffer's read. */
#define RADIO_ACK_END_US 150u

#define RADIO_FCS_LEN 2u
#define RADIO_FCF_VERSION_2015 2u
#define RADIO_FCF_TYPE_MASK 0x07u
#define RADIO_FCF_TYPE_ACK 0x02u
#define RADIO_FCF_FRAME_PENDING 0x10u
#define RADIO_CCA_REQUEST 0x80u
#define RADIO_TRX_STATUS_CCA_DONE 0x80u
#define RADIO_TRX_STATUS_CCA_IDLE 0x40u
#endif

/* PHY_ED_LEVEL: 0x00 = "<= RSSIBASE_VAL", 0x53 = top of the 83 dB range, 0xFF = no
 * measurement started yet (at86rf233.txt:11506-11523). */
#define RADIO_ED_LEVEL_INVALID 0xFFu
#define RADIO_ED_LEVEL_MAX 0x53u
#define RADIO_ED_BASE_DBM (-94)
/* What is written to *request* a measurement. Not 0: 0 is a plausible measurement result (a quiet
 * channel reads at or below the floor), so only a value no measurement could produce proves that
 * a completion interrupt had a measurement behind it. Measured on the board: a requested
 * measurement never leaves its sentinel in the register (docs/hardware_samr21.md §6). */
#define RADIO_ED_TRIGGER_SENTINEL 0x55u

/* SCLK = gen0 / (2 x (BAUD + 1)); 48 MHz / (2 x 6) = 4 MHz. The rate bounds the Ack's
 * turnaround - the whole frame is read before the Ack is written, 2 us per octet - but 6 MHz
 * (BAUD 3), inside the RF233's 7.5 MHz asynchronous limit on paper (at86rf233 datasheet 6.2),
 * corrupts Frame Buffer reads on the ConBee II (measured: a 13-octet PSDU read back as PHR 71),
 * and the OpenThread SAMR21 port runs this bus at 4 MHz too (docs/hardware_samr21.md §6). */
#define RADIO_SPI_BAUD_48MHZ 5u

/* One attempt's bound: CSMA-CA at the default settings 37 ms, a 127-octet frame 4.25 ms and the Ack's
 * wait 1 ms. ARET runs all its attempts inside one wait for TRX_END, so that wait gets one bound per
 * attempt. The ED measurement needs 128 us (8 symbol periods). */
#define RADIO_TX_DEADLINE_MS 50u
#define RADIO_STATE_DEADLINE_MS 5u
#define RADIO_ED_CONFIRM_MS 3u /* IRQ_4 must arrive within this: a measurement itself takes at
                                  * most 180 us (docs/hardware_samr21.md §6), so three loop passes is
                                  * already generous, and a scan must never block on one */

/* Output power steps, tenths of a dBm in index order (Table 9-9: 0x0 = +4 dBm,
 * 0xF = -17). */
static const int16_t radioPowerTenths[16] = {
    40, 37, 34, 30, 25, 20, 10, 0, -10, -20, -30, -40, -60, -80, -120, -170};

typedef struct
{
    int inited;
    uint8_t channel;
    uint8_t powerIdx;
    int rxOn;
    uint8_t rxFilter; /* RADIO_RX_* */
    uint16_t panId;
    int panOn;
    uint16_t shortAddr;
    int shortOn;
    uint8_t extAddr[8];
    int extOn;

    Radio_Counters counters;

    /* Single producer (the radio vector) and single consumer (the loop): the handler advances
     * rxHead only, the loop rxTail only, and both run free, so the fill is their difference. */
    Radio_Frame rxRing[RADIO_RX_SLOTS];
    volatile uint8_t rxHead;
    volatile uint8_t rxTail;
    void (*rxHandler)(const Radio_Frame *frame);

    /* What the radio vector does with a TRX_END. The thread changes it only under Rf_Lock. */
    volatile uint8_t isrPhase; /* RADIO_ISR_* */
    volatile uint32_t ackSince; /* board ms the Ack went on air, for the stuck-Ack recovery */
    /* IRQ_STATUS causes the handler read and did not act on, for the thread to take. */
    volatile uint8_t irqMirror;
    /* The filter and Ack configuration the handler reads; written under Rf_Lock. */
    Mac_AckCfg ackCfg;
    Rf_RxFrame isrRaw;
    Radio_Frame isrFrame;

    /* Handler statistics, read by Radio_Witnesses: the handler emits no records itself. */
    volatile uint16_t isrFrames;
    volatile uint16_t isrAcks;
    volatile uint16_t isrAcksPending;
    volatile uint16_t isrAcksLate;
    volatile uint16_t isrAckNoRoom;
    volatile uint16_t isrSpiFail;
    volatile uint16_t isrAckStuck;
    volatile uint16_t isrCrcBad;
    volatile uint16_t isrReadFail;
    volatile uint16_t lastAckCycles;  /* the TRX_END stamp to TX_START */
    volatile uint16_t lastReadCycles; /* the Frame Buffer read of the last frame */

    /* TX: advanced one phase per Radio_Poll pass. Every frame goes through
     * TX_ARET_ON, so there is one transmit path, not two. */
    enum
    {
        RADIO_TX_IDLE = 0,
        RADIO_TX_TO_ARET,
        RADIO_TX_WAIT_ARET,
        RADIO_TX_WAIT_END,
#ifdef RCP_TX_TIMING
        /* A frame whose Ack the host must see, or one due at a set moment: ARET keeps the Ack to itself
         * and starts when it likes, so the steps are this file's own, on RX_ON and PLL_ON. */
        RADIO_TX_BACKOFF,
        RADIO_TX_TIMED,
        RADIO_TX_SENDING,
        RADIO_TX_ACK_WAIT,
#endif
    } txPhase;
    uint8_t txPsdu[RADIO_PSDU_MAX];
    uint8_t txLen;
    int txAckWait; /* the frame requested an acknowledgment: ARET retries apply */
    uint8_t txRetries;
    int txRetriesSet;
    void *txToken;
    uint32_t txDeadline;

    /* Energy detection: one request at a time, RADIO_ED_* in radio.h. edFresh says the level
   * was taken while the transceiver was not mid-reception, which is the only case where the
   * number belongs to the request rather than to the frame in the air. */
    int edState;
    uint32_t edDeadline;
    int8_t edDbm;
    int edFresh;

    /* One IRQ_STATUS read per pass, shared by the TX, RX and ED consumers (see above). */
    uint8_t irqBits; /* this pass's IRQ_STATUS snapshot; consumers take the bits they act on */
    int irqSeen; /* 1 when the snapshot came from a successful read */

    void (*txDoneHandler)(void *token, int status, int framePending);

#ifdef RCP_TX_TIMING
    MacSec_State *macSec; /* set while a host's keys are: the Enhanced Ack is on */
    uint32_t clockHigh;   /* Radio_ClockUs */
    uint32_t clockLast;
    uint32_t rng;
    uint8_t txSeq;
    uint8_t txTriesLeft;
    uint8_t txCca;
    uint8_t txBe;
    uint8_t txBackoffs;
    uint32_t txAtUs; /* the end of a backoff, or the moment a timed frame's SFD ends */
    /* The vector's word on a transmission; RADIO_TXEV_*, and the frame's end for the Ack wait. */
    volatile uint8_t txEvent;
    volatile uint32_t txEndUs;
    uint16_t txAckExtraUs; /* what the wait grew by when it ran out; 0 until then */
    Radio_Frame txAckFrame;
    int txAckHave;
#endif

    /* The source-match table the host writes through Spinel (docs/protocol.md §10.2): the Ack
     * the radio vector composes for a Data Request reads its Frame Pending bit from here. */
    SrcMatch_Table srcMatch;
} RadioState;

static RadioState radio;

enum
{
    RADIO_ISR_OFF = 0, /* TRX_END belongs to the thread (TX_ARET, TRX_OFF): mirror it */
    RADIO_ISR_RX,      /* RX_ON: TRX_END is a received frame */
    RADIO_ISR_ACK,     /* an Ack is on air: TRX_END ends it */
#ifdef RCP_TX_TIMING
    RADIO_ISR_TX,      /* our own frame is on air: TRX_END ends it */
    RADIO_ISR_TX_ACK   /* RX_ON behind it: TRX_END is its Ack, or some frame to drop */
#endif
};

#ifdef RCP_TX_TIMING
enum
{
    RADIO_TXEV_NONE = 0,
    RADIO_TXEV_SENT,  /* the frame is out, and nothing more comes for it */
    RADIO_TXEV_WAIT,  /* the frame is out, and its Ack is awaited */
    RADIO_TXEV_ACKED
};
#endif

/* The handler's filter view of the addresses. Under Rf_Lock, so the handler never sees half an
 * update. The extended address is stored MSB-first and compared in on-air order. */
static void radioSyncAckCfg(void)
{
    uint8_t k;

    Rf_Lock();
    radio.ackCfg.filter.mode = (radio.rxFilter == RADIO_RX_FILTERED)
                                   ? MAC_FILTER_OFF
                                   : (radio.rxFilter == RADIO_RX_PAN ? MAC_FILTER_PAN
                                                                     : MAC_FILTER_ALL);
    radio.ackCfg.filter.panId = radio.panOn ? radio.panId : 0xFFFFu;
    radio.ackCfg.filter.shortAddr = radio.shortOn ? radio.shortAddr : 0xFFFFu;

    for (k = 0; k < SRCMATCH_EXT_LEN; ++k)
        radio.ackCfg.filter.extAddr[k] = radio.extOn ? radio.extAddr[7u - k] : 0xFFu;

    radio.ackCfg.isCoordinator = 0u;
    Rf_Unlock();
}

/* ---- small helpers ---------------------------------------------------- */

static int radioWritePower(uint8_t idx)
{
    uint8_t v;

    if (!Rf_RegRead(RADIO_REG_PHY_TX_PWR, &v))
        return 0;

    v = (uint8_t)((v & 0xF0u) | (idx & 0x0Fu));
    return Rf_RegWrite(RADIO_REG_PHY_TX_PWR, v);
}

static int radioSetState(uint8_t cmd)
{
    return Rf_TrxCmd(cmd);
}

/* A switched-off address pair is the datasheet's permissive default (0xFFFF), not a stale value.
 * The transceiver's own filter takes no part in RX_ON; the registers are kept programmed so they
 * read back what the host set, and the vector's filter view is synced from the same fields. */
static int radioSetFilter(void)
{
    uint16_t pan = radio.panOn ? radio.panId : 0xFFFFu;
    uint16_t shortAddr = radio.shortOn ? radio.shortAddr : 0xFFFFu;
    uint8_t v;
    int ok = 1;

    if (!Rf_RegWrite(RADIO_REG_PAN_ID_0, (uint8_t)(pan & 0xFFu)) ||
        !Rf_RegWrite((uint8_t)(RADIO_REG_PAN_ID_0 + 1u), (uint8_t)(pan >> 8)))
        ok = 0;

    if (!Rf_RegWrite(RADIO_REG_SHORT_ADDR_0, (uint8_t)(shortAddr & 0xFFu)) ||
        !Rf_RegWrite((uint8_t)(RADIO_REG_SHORT_ADDR_0 + 1u), (uint8_t)(shortAddr >> 8)))
        ok = 0;
    /* IEEE_ADDR_0..7 are least significant byte first - the transceiver compares them in
   * the order the address appears on air - while the contract takes the EUI-64 most
   * significant byte first, the order Spinel's 'E' carries (see radio.h, and the
   * AT86RF233 datasheet's frame filter description). */
    for (v = 0; v < 8u; ++v)
    {
        uint8_t octet = radio.extOn ? radio.extAddr[7u - v] : 0xFFu;

        if (!Rf_RegWrite((uint8_t)(RADIO_REG_IEEE_ADDR_0 + v), octet))
            ok = 0;
    }

    radioSyncAckCfg();
    return ok;
}

/* The mode reaches the radio vector's filter view only: every mode receives in RX_ON, where
 * the transceiver's own promiscuous and auto-Ack bits (XAH_CTRL_1, CSMA_SEED_1) take no part,
 * so nothing is written to them. A promiscuous receiver "shall not be acknowledged, even if
 * requested" (at86rf233.txt:6695-6708), which the vector's Mac_AckDecide applies. */
static int radioApplyFilterBits(void)
{
    radioSyncAckCfg();
    return 1;
}

/* Every mode receives in RX_ON: the filter and the Ack are the radio vector's (see the top of
 * this file). The switch passes through TRX_OFF, forced, because a stored TRX_OFF waits for an
 * active state to finish (at86rf233.txt:4108-4113). The whole sequence holds the SPI, so the
 * vector never sees a half-made state, and a TRX_END left over from the previous state is
 * discarded on the way: it described a frame or a transmission that is no longer there. */
static int radioEnterRx(void)
{
    uint8_t state;
    uint8_t stale;
    int ok;

    Rf_Lock();
    radio.isrPhase = RADIO_ISR_OFF;

    if (!radioSetState(RADIO_CMD_FORCE_TRX_OFF) || !Rf_WaitTrxState(RADIO_ST_TRX_OFF))
    {
        Rf_Unlock();
        Debug_Emit(DBG_COMP_RF, 0u, DBG_RF_RSN_RX_MODE_FAIL);
        return 0;
    }

    if (Rf_IrqStatus(&stale))
        radio.irqMirror = (uint8_t)(radio.irqMirror | (stale & RADIO_IRQ_ED_DONE));

    ok = radioSetState(RADIO_CMD_RX_ON) && Rf_WaitTrxStateAny(RADIO_ST_RX_ON, RADIO_ST_BUSY_RX);

    if (ok)
        radio.isrPhase = RADIO_ISR_RX;

    Rf_Unlock();

    /* The state readback is what says whether the switch took. */
    if (Rf_TrxStatus(&state))
    {
        Debug_Emit(DBG_COMP_RF,
                   (uint16_t)(state & RADIO_ST_MASK),
                   ok ? DBG_RF_RSN_RX_MODE : DBG_RF_RSN_RX_MODE_FAIL);
    }
    else if (!ok)
        Debug_Emit(DBG_COMP_RF, 0u, DBG_RF_RSN_RX_MODE_FAIL);

    return ok;
}

static uint32_t radioNow(void)
{
    uint32_t ms;

    Tick_GetMs(&ms);
    return ms;
}

/* ---- lifecycle -------------------------------------------------------- */

int Radio_Init(void)
{
    uint8_t part = 0;
    uint8_t ctrl;
    uint8_t cca;

    if (radio.inited)
        return 1;

    if (!Rf_HwInit())
    {
        DBG(DBG_COMP_RF, DBG_RF_RSN_RADIO_INIT_FAIL);
        return 0;
    }
    /* The SPI bit rate comes first: SERCOM4 is clocked from gen0, so the divisor for an
   * 8 MHz tree gives 12 MHz once the CPU is on the DFLL, above the RF233's 8 MHz fsync
   * maximum, and the transceiver stops answering (at86rf233.txt:20690-20700). */
    if (!Rf_SetSpiBaud(RADIO_SPI_BAUD_48MHZ))
    {
        DBG(DBG_COMP_RF, DBG_RF_RSN_SPI_BAUD_FAIL);
        return 0;
    }
    /* The clock bring-up already woke it: do not reset, a reset drops CLKM, the DFLL
   * reference the CPU runs on. */
    if (!Rf_PartNum(&part) || part != 0x0Bu)
    {
        DBG(DBG_COMP_RF, DBG_RF_RSN_RADIO_INIT_FAIL);
        return 0;
    }

#ifdef RCP_TX_TIMING
    Tick_ClockStart();
#endif

    /* Read-modify-write: keep bit 5 (CRC) and set bit 7 (front-end control), the way
   * the vendor turns on the RX/TX indicator after reaching TRX_OFF. */
    if (!Rf_RegRead(RADIO_REG_TRX_CTRL_1, &ctrl) ||
        !Rf_RegWrite(RADIO_REG_TRX_CTRL_1,
                     (uint8_t)(ctrl | RADIO_TRX_CTRL_1_CRC_ON | RADIO_TRX_CTRL_1_PA_EXT_EN)))
    {
        DBG(DBG_COMP_RF, DBG_RF_RSN_RADIO_INIT_FAIL);
        return 0;
    }
    /* With PA_EXT_EN set, DIG3/DIG4 carry the RX/TX indicator; the front end follows it once
     * routed and switched on. */
    Rf_FrontEndInit(Board_Get()->radioFrontEndPins);
    /* The pin is configured before the transceiver may raise it; the vector is armed last,
     * once the transceiver is in a known state. */
    Rf_IrqLineInit();

    if (!Rf_RegWrite(RADIO_REG_IRQ_MASK, RADIO_IRQ_MASK_USED))
    {
        DBG(DBG_COMP_RF, DBG_RF_RSN_RADIO_INIT_FAIL);
        return 0;
    }

    {
        uint8_t ctrl2;

        if (!Rf_RegRead(RADIO_REG_TRX_CTRL_2, &ctrl2) ||
            !Rf_RegWrite(RADIO_REG_TRX_CTRL_2, (uint8_t)(ctrl2 | RADIO_TRX_CTRL_2_RX_SAFE_MODE)))
        {
            DBG(DBG_COMP_RF, DBG_RF_RSN_RADIO_INIT_FAIL);
            return 0;
        }
    }

    if (!Rf_RegRead(RADIO_REG_PHY_CC_CCA, &cca))
    {
        DBG(DBG_COMP_RF, DBG_RF_RSN_RADIO_INIT_FAIL);
        return 0;
    }

    radio.channel = 11u;

    if (!Rf_RegWrite(RADIO_REG_PHY_CC_CCA,
                     (uint8_t)((cca & (uint8_t)~RADIO_CHANNEL_MASK) | RADIO_CCA_MODE_ENERGY |
                               radio.channel)))
    {
        DBG(DBG_COMP_RF, DBG_RF_RSN_RADIO_INIT_FAIL);
        return 0;
    }

    radio.powerIdx = 7u; /* 0 dBm, Table 9-9 */
    (void)radioWritePower(radio.powerIdx);
    radio.extAddr[0] = 0u;
    /* Filtered by default, with the permissive defaults until the host gives addresses. */
    radio.rxFilter = RADIO_RX_FILTERED;

    if (!radioSetFilter() || !radioApplyFilterBits() || !radioSetState(RADIO_CMD_TRX_OFF) ||
        !Rf_WaitTrxState(RADIO_ST_TRX_OFF))
    {
        DBG(DBG_COMP_RF, DBG_RF_RSN_RADIO_INIT_FAIL);
        return 0;
    }

    radio.txPhase = RADIO_TX_IDLE;
    radio.isrPhase = RADIO_ISR_OFF;
    radioSyncAckCfg();

    {
        uint8_t stale;

        (void)Rf_IrqStatus(&stale);
    }

    Rf_IrqArm();
    radio.inited = 1;
    return 1;
}

/* Publish what the receiver is actually doing, not what was requested, and the radio vector's
 * counters: the vector itself emits nothing, so this is where its work becomes visible. */
void Radio_Witnesses(void)
{
    uint8_t first;
    uint8_t second;

    if (!radio.inited)
        return;

    Debug_Emit(DBG_COMP_RF, radio.isrFrames, DBG_RF_RSN_ISR_FRAMES);
    Debug_Emit(DBG_COMP_RF, radio.isrCrcBad, DBG_RF_RSN_ISR_CRC_BAD);
    Debug_Emit(DBG_COMP_RF, radio.isrAcks, DBG_RF_RSN_ISR_ACKS);
    Debug_Emit(DBG_COMP_RF, radio.isrAcksPending, DBG_RF_RSN_ISR_ACKS_PENDING);
    Debug_Emit(DBG_COMP_RF, radio.isrAcksLate, DBG_RF_RSN_ISR_ACKS_LATE);
    Debug_Emit(DBG_COMP_RF, radio.isrAckNoRoom, DBG_RF_RSN_ISR_ACK_NO_ROOM);
    Debug_Emit(DBG_COMP_RF, radio.isrSpiFail, DBG_RF_RSN_ISR_SPI_FAIL);
    Debug_Emit(DBG_COMP_RF, radio.isrReadFail, DBG_RF_RSN_ISR_READ_FAIL);
    Debug_Emit(DBG_COMP_RF, radio.isrAckStuck, DBG_RF_RSN_ISR_ACK_STUCK);
    Debug_Emit(DBG_COMP_RF, radio.lastAckCycles, DBG_RF_RSN_ISR_ACK_CYCLES);
    Debug_Emit(DBG_COMP_RF, radio.lastReadCycles, DBG_RF_RSN_ISR_READ_CYCLES);

    /* The transceiver's status after a state change is briefly stale, so read each value
   * twice: line = (first read << 8) | second read. */
    if (Rf_TrxStatus(&first) && Rf_TrxStatus(&second))
    {
        Debug_Emit(DBG_COMP_RF,
                   (uint16_t)(((uint16_t)(first & RADIO_ST_MASK) << 8) |
                              (uint16_t)(second & RADIO_ST_MASK)),
                   DBG_RF_RSN_RX_MODE);
    }
}

/* ---- channel / power -------------------------------------------------- */

int Radio_SetChannel(uint8_t channel)
{
    uint8_t cca;

    if (!radio.inited || channel < RADIO_CHAN_MIN || channel > RADIO_CHAN_MAX)
        return 0;

    if (!Rf_RegRead(RADIO_REG_PHY_CC_CCA, &cca))
        return 0;

    if (!Rf_RegWrite(RADIO_REG_PHY_CC_CCA,
                     (uint8_t)((cca & (uint8_t)~RADIO_CHANNEL_MASK) | channel)))
        return 0;

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

int Radio_SetPower(int8_t dbm)
{
    int target = (int)dbm * 10;
    uint8_t idx = 15u;

    if (!radio.inited)
        return 0;
    /* Snap down: the first step at or below the request. */
    {
        uint8_t i;

        for (i = 0; i < 16u; ++i)
        {
            if (radioPowerTenths[i] <= target)
            {
                idx = i;
                break;
            }
        }
    }

    if (!radioWritePower(idx))
        return 0;

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

int Radio_SetCcaThreshold(int8_t dbm)
{
    /* Snap down, as the power does: -71 dBm is step 11, -72 dBm. */
    int steps = ((int)dbm - RADIO_RSSI_BASE_DBM) / 2;
    uint8_t v;

    if (!radio.inited)
        return 0;

    if (steps < 0)
        steps = 0;

    if (steps > RADIO_CCA_ED_THRES_MAX)
        steps = RADIO_CCA_ED_THRES_MAX;

    if (!Rf_RegRead(RADIO_REG_CCA_THRES, &v))
        return 0;

    return Rf_RegWrite(RADIO_REG_CCA_THRES,
                       (uint8_t)((v & (uint8_t)~RADIO_CCA_ED_THRES_MASK) | (uint8_t)steps));
}

/* ---- receive ---------------------------------------------------------- */

int Radio_SetRxFilter(int mode)
{
    if (!radio.inited || mode < RADIO_RX_FILTERED || mode > RADIO_RX_ALL)
        return 0;
    /* The last *requested* mode, not a promise the transceiver reached it: no early return,
   * so a failed switch stays retryable (the caller retries while this returns 0). */
    radio.rxFilter = (uint8_t)mode;

    if (!radioApplyFilterBits())
        return 0;
    /* Apply now only when the receiver is running and no frame is going out; a transmission
   * in flight picks the mode up in radioTxFinish. */
    if (radio.rxOn && radio.txPhase == RADIO_TX_IDLE)
    {
        if (!radioEnterRx())
            return 0;
    }

    Radio_Witnesses();
    return 1;
}

int Radio_RxEnable(int on)
{
    if (!radio.inited)
        return 0;

    if (on)
    {
        if (!radioEnterRx())
            return 0;

        radio.rxOn = 1;
        return 1;
    }

    radio.rxOn = 0;

    if (radio.txPhase != RADIO_TX_IDLE)
    {
        return 1; /* the TX engine returns to TRX_OFF itself */
    }

    {
        int ok;

        Rf_Lock();
        radio.isrPhase = RADIO_ISR_OFF;
        ok = radioSetState(RADIO_CMD_FORCE_TRX_OFF) && Rf_WaitTrxState(RADIO_ST_TRX_OFF);
        Rf_Unlock();
        return ok;
    }
}

int Radio_SetPanId(uint16_t panid, int on)
{
    if (!radio.inited)
        return 0;

    radio.panId = panid;
    radio.panOn = on ? 1 : 0;
    return radioSetFilter();
}

int Radio_SetShortAddr(uint16_t addr, int on)
{
    if (!radio.inited)
        return 0;

    radio.shortAddr = addr;
    radio.shortOn = on ? 1 : 0;
    return radioSetFilter();
}

int Radio_SetExtAddr(const uint8_t ext8[8], int on)
{
    uint8_t i;

    if (!radio.inited || ext8 == 0)
        return 0;

    for (i = 0; i < 8u; ++i)
        radio.extAddr[i] = ext8[i];

    radio.extOn = on ? 1 : 0;

    /* CSMA_SEED_0 is the low byte of the backoff generator's seed and resets to the same value on
     * every part, so every stick would back off in the same sequence. The stock stack seeds it from
     * the extended address's low byte, the last octet in wire order (phyReset.c); so does this. */
    if (on && !Rf_RegWrite(RADIO_REG_CSMA_SEED_0, ext8[7]))
        return 0;

    return radioSetFilter();
}

/* ---- transmit --------------------------------------------------------- */

/* ARET reads the retry budget from XAH_CTRL_0. */
static int radioSetRetries(uint8_t retries)
{
    uint8_t v;

    if (!Rf_RegRead(RADIO_REG_XAH_CTRL_0, &v))
        return 0;

    v = (uint8_t)((v & (uint8_t)~RADIO_XAH_CTRL_0_RETRIES_MASK) |
                  (uint8_t)((retries << RADIO_XAH_CTRL_0_RETRIES_SHIFT) &
                            RADIO_XAH_CTRL_0_RETRIES_MASK));

    if (!Rf_RegWrite(RADIO_REG_XAH_CTRL_0, v))
        return 0;

    radio.txRetries = retries;
    radio.txRetriesSet = 1;
    return 1;
}

/* enableCSMACA = 0 asks for a frame sent "without performing CCA"; the datasheet's
 * immediate-TX_ARET setting, MAX_CSMA_RETRIES = 7, is exactly that
 * (at86rf233.txt:7235-7239, 8537-8539). */
static int radioSetCsma(uint8_t retries)
{
    uint8_t v;

    if (!Rf_RegRead(RADIO_REG_XAH_CTRL_0, &v))
        return 0;

    v = (uint8_t)((v & (uint8_t)~RADIO_XAH_CTRL_0_CSMA_MASK) |
                  (uint8_t)((retries << RADIO_XAH_CTRL_0_CSMA_SHIFT) &
                            RADIO_XAH_CTRL_0_CSMA_MASK));
    return Rf_RegWrite(RADIO_REG_XAH_CTRL_0, v);
}

#ifdef RCP_TX_TIMING
static void radioBackoffFirst(void);
#endif

int Radio_Tx(const uint8_t *psdu, uint8_t len, int8_t dbm, int cca, int maxRetries, void *token)
{
    if (!radio.inited || psdu == 0 || len == 0u)
        return 0;

    if (len > RADIO_TX_PSDU_MAX)
    {
        DBG(DBG_COMP_RF, DBG_RF_RSN_TX_TOO_LONG);
        return 0;
    }

    if (radio.txPhase != RADIO_TX_IDLE)
    {
        return 0; /* one frame in flight; the caller was told */
    }

    if (dbm != 0 && !Radio_SetPower(dbm))
        return 0;

    {
        uint8_t i;

        for (i = 0; i < len; ++i)
            radio.txPsdu[i] = psdu[i];
    }
    radio.txLen = len;
    /* ARET runs CSMA-CA for every frame, so a caller asking for no CCA has to have the
   * CSMA budget turned off explicitly; 7 is the datasheet's "transmit immediately". */
    if (!radioSetCsma(cca ? RADIO_XAH_CTRL_0_CSMA_DEFAULT : RADIO_XAH_CTRL_0_CSMA_NONE))
        return 0;

    /* ARET reads the same FCF bit to decide whether to wait for an acknowledgment, so take
   * it from the frame rather than a flag a caller could forget. */
    radio.txAckWait = Mac_AckRequested(psdu, len);

    if (radio.txAckWait)
    {
        uint8_t want = (maxRetries < 0) ? 0u : ((maxRetries > 15) ? 15u : (uint8_t)maxRetries);

        if (!radio.txRetriesSet || want != radio.txRetries)
        {
            if (!radioSetRetries(want))
                return 0;
        }
    }

    radio.txToken = token;
    radio.txDeadline = radioNow() + RADIO_TX_DEADLINE_MS;
    radio.txPhase = RADIO_TX_TO_ARET;
#ifdef RCP_TX_TIMING
    radio.txAckHave = 0;

    /* A host that set keys needs a version 2 frame's Enhanced Ack, which ARET will not show. */
    if (radio.txAckWait && radio.macSec != 0 && len >= 3u && ((psdu[1] >> 4) & 3u) == RADIO_FCF_VERSION_2015)
    {
        radio.txSeq = psdu[2];
        radio.txCca = cca ? 1u : 0u;
        radio.txTriesLeft = (uint8_t)(maxRetries < 0 ? 0 : (maxRetries > 15 ? 15 : maxRetries));
        radioBackoffFirst();
    }
#endif
    radio.counters.txFrames++;
    return 1;
}

#ifdef RCP_TX_TIMING
int Radio_TxAt(const uint8_t *psdu, uint8_t len, int8_t dbm, int cca, uint32_t sfdEndUs, void *token)
{
    uint8_t i;
    int32_t lead;

    if (!radio.inited || psdu == 0 || len == 0u || len > RADIO_TX_PSDU_MAX || radio.txPhase != RADIO_TX_IDLE)
        return 0;

    if (dbm != 0 && !Radio_SetPower(dbm))
        return 0;

    for (i = 0u; i < len; ++i)
        radio.txPsdu[i] = psdu[i];

    radio.txLen = len;
    radio.txAckWait = Mac_AckRequested(psdu, len);
    radio.txSeq = psdu[2];
    radio.txCca = cca ? 1u : 0u;
    radio.txTriesLeft = 0u;
    radio.txAtUs = sfdEndUs;
    radio.txAckHave = 0;
    radio.txToken = token;

    /* A moment already gone is found out by the phase, and completes as radio.h says it must. */
    lead = (int32_t)(sfdEndUs - Tick_ClockUs());
    radio.txDeadline = radioNow() + RADIO_TX_DEADLINE_MS + (lead > 0 ? (uint32_t)lead >> 9 : 0u);
    radio.txPhase = RADIO_TX_TIMED;
    radio.counters.txFrames++;
    return 1;
}

const Radio_Frame *Radio_TxAck(void)
{
    return radio.txAckHave ? &radio.txAckFrame : 0;
}

int Radio_SetMacSec(MacSec_State *state)
{
    Rf_Lock();
    radio.macSec = state;
    Rf_Unlock();
    return 1;
}
#endif

/* status is a RADIO_TX_* value; framePending carries the received ACK's Frame Pending
 * subfield (0/1) and is 0 whenever no acknowledgment was received. */
static void radioTxFinish(int status, int framePending)
{
    void (*fn)(void *, int, int) = radio.txDoneHandler;
    void *token = radio.txToken;

    if (status != RADIO_TX_OK)
        radio.counters.txFailed++;

    /* Back to the caller's mode, never left in PLL_ON, through radioEnterRx so a filtered mode
   * survives a transmission instead of being replaced by RX_ON. The phase turns idle only once
   * the radio has settled, so "idle" means "the next command may be issued": a command sent
   * during a transition can be ignored. The wait is bounded and this runs in thread context
   * from radioPollTx, never from an interrupt. */
    if (radio.rxOn)
        (void)radioEnterRx();
    else
    {
        Rf_Lock();
        radio.isrPhase = RADIO_ISR_OFF;

        if (radioSetState(RADIO_CMD_TRX_OFF))
            (void)Rf_WaitTrxState(RADIO_ST_TRX_OFF);

        Rf_Unlock();
    }

    radio.txPhase = RADIO_TX_IDLE;
    radio.txToken = 0;

    if (fn)
        fn(token, status, framePending);
}

/* ---- energy detection -------------------------------------------------- */

int Radio_StartEd(void)
{
    uint8_t st;

    if (!radio.inited)
        return 0;

    if (radio.txPhase != RADIO_TX_IDLE)
        return 0; /* a scan yields to a transmit rather than interrupting one */

    if (radio.edState == RADIO_ED_MEASURING)
        return 0; /* one measurement at a time */

    /* A manual measurement is legal only in a receive state (docs/hardware_samr21.md §6), and asking
   * anywhere else is the datasheet's own trap: IRQ_4 arrives with no measurement behind it. */
    if (!Rf_TrxStatus(&st))
        return 0;
    st = (uint8_t)(st & RADIO_ST_MASK);
    if (st != RADIO_ST_RX_ON && st != RADIO_ST_BUSY_RX)
    {
        Debug_Emit(DBG_COMP_RF, (uint16_t)st, DBG_RF_RSN_ED_BAD_STATE);
        return 0;
    }

    /* The request is a write access to the register itself, and the value written is a sentinel:
   * 0 is what a quiet channel measures, so only a value no measurement could produce proves
   * that a measurement replaced it (measured on the board). */
    if (!Rf_RegWrite(RADIO_REG_PHY_ED_LEVEL, RADIO_ED_TRIGGER_SENTINEL))
        return 0;

    radio.edState = RADIO_ED_MEASURING;
    radio.edFresh = 0;
    radio.edDeadline = radioNow() + RADIO_ED_CONFIRM_MS;
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
        /* The refusal already reached the record where it happened. Consuming the state is what
         * lets the next request be made at all. */
        radio.edState = RADIO_ED_IDLE;
        radio.edFresh = 0;
        return -1;
    }

    return 0; /* MEASURING or IDLE: nothing to take, and nothing wrong */
}

void Radio_AbortEd(void)
{
    radio.edState = RADIO_ED_IDLE;
    radio.edFresh = 0;
}

/* ---- polling ---------------------------------------------------------- */

/* Field-by-field, through U_BStream for the payload, never a struct assignment: GCC
 * lowers a large struct copy to a libc memcpy, which this -nostdlib image cannot link
 * (usb/usbd.h:21). Handler context: the slot is filled before rxHead publishes it. */
static int radioRxRoom(void)
{
    return (uint8_t)(radio.rxHead - radio.rxTail) < RADIO_RX_SLOTS;
}

static void radioRxPush(const Radio_Frame *frame)
{
    Radio_Frame *dst;
    U_BStream bs;

    if (!radioRxRoom())
    {
        /* Drop-newest, counted (docs/coding-rules.md §4.3). */
        radio.counters.rxDropped++;
        return;
    }

    dst = &radio.rxRing[radio.rxHead % RADIO_RX_SLOTS];
    dst->len = frame->len;
    dst->rssi = frame->rssi;
    dst->lqi = frame->lqi;
    dst->channel = frame->channel;
    dst->timestamp = frame->timestamp;
    dst->crcOk = frame->crcOk;
    dst->ackedFp = frame->ackedFp;
#ifdef RCP_TX_TIMING
    dst->ackedSec = frame->ackedSec;
    dst->ackKeyIndex = frame->ackKeyIndex;
    dst->ackFrameCounter = frame->ackFrameCounter;
#endif
    U_bstream_init(&bs, dst->psdu, RADIO_PSDU_MAX);
    (void)U_bstream_put_bytes(&bs, frame->psdu, frame->len);
    radio.rxHead = (uint8_t)(radio.rxHead + 1u);
}

#ifdef RCP_TX_TIMING
/* The vector stamps the clock's low half; the whole clock is only known out here. */
static uint64_t radioExtendUs(uint32_t low)
{
    uint64_t now = Radio_ClockUs();

    return now + (uint64_t)(int64_t)(int32_t)(low - (uint32_t)now);
}
#endif

/* The slot is released only after the handler returned, so the vector cannot refill a frame
 * that is still being read. */
static void radioRxDeliver(void)
{
    Radio_Frame *f;

    if (radio.rxHead == radio.rxTail || radio.rxHandler == 0)
        return;

    f = &radio.rxRing[radio.rxTail % RADIO_RX_SLOTS];
    radio.counters.rxFrames++;

    if (!f->crcOk)
    {
        radio.counters.rxCrcBad++;
        DBG(DBG_COMP_RF, DBG_RF_RSN_RX_CRC_BAD);
    }

#ifdef RCP_TX_TIMING
    f->timestamp = radioExtendUs((uint32_t)f->timestamp);
#endif
    radio.rxHandler(f);
    radio.rxTail = (uint8_t)(radio.rxTail + 1u);
}

/* ---- the radio vector -------------------------------------------------- */

/* Start an Ack for the frame just read. FORCE_PLL_ON first, so a frame arriving behind this one cannot
 * overwrite the Ack in the shared buffer, then the octets [PHR, MPDU without its FCS], then TX_START on
 * schedule. Returns 1 when the Ack went on air. */
static int radioIsrAckStart(uint32_t stamp, const uint8_t *buf, uint8_t n)
{
    uint8_t st = 0;
    uint32_t i;
    uint32_t elapsed;

    if (!radioSetState(RADIO_CMD_FORCE_PLL_ON) || !Rf_FrameBufferWriteRaw(buf, n))
    {
        radio.isrSpiFail++;
        return 0;
    }

    /* RX_ON -> PLL_ON takes 1 us (at86rf233 datasheet Table 7-1); the bound only guards a
     * transceiver that stopped answering. */
    for (i = 0; i < 64u; ++i)
    {
        if (Rf_TrxStatus(&st) && (st & RADIO_ST_MASK) == RADIO_ST_PLL_ON)
            break;
    }

    if ((st & RADIO_ST_MASK) != RADIO_ST_PLL_ON)
    {
        radio.isrSpiFail++;
        return 0;
    }

    do
    {
        elapsed = Tick_CyclesSince(stamp);
    } while (elapsed < RADIO_ACK_TX_START_US * RADIO_CYCLES_PER_US);

    if (elapsed > RADIO_ACK_LATEST_US * RADIO_CYCLES_PER_US)
    {
        radio.isrAcksLate++;
        return 0;
    }

    if (!radioSetState(RADIO_CMD_TX_START))
    {
        radio.isrSpiFail++;
        return 0;
    }

    radio.lastAckCycles = (uint16_t)(elapsed > 0xFFFFu ? 0xFFFFu : elapsed);
    return 1;
}

static int radioIsrAck(uint32_t stamp, uint8_t seq, int framePending)
{
    uint8_t ack[MAC_ACK_LEN];

    Mac_BuildImmAck(seq, framePending, ack);
    return radioIsrAckStart(stamp, ack, MAC_ACK_LEN);
}

#ifdef RCP_TX_TIMING
/* The Enhanced Ack of a version 2 frame, secured when the frame was. Its block encryptions do not fit
 * ahead of the Ack's slot, so the Ack starts without its MIC, and the MIC follows into the buffer while
 * the header goes out: the transceiver reads the buffer as it transmits. The MIC's first octet leaves
 * 176 + 32 x (1 + header) us behind TX_START: 688 us for the shortest header that needs four blocks,
 * against 560 us measured for them. A MIC that came late would only make the sender drop the Ack and
 * retry. Returns 1 when the Ack went on air. */
static int radioIsrEnhAck(uint32_t stamp, const Rf_RxFrame *raw, const MacSec_AckRx *rx, int fp, Radio_Frame *frame)
{
    uint8_t ack[1u + MAC_SEC_ACK_MAX];
    uint8_t ie[MAC_SEC_PROBE_IE_MAX];
    MacSec_AckResult result;
    MacSec_AckMic later;
    uint8_t ieLen;
    uint8_t n;

    ieLen = MacSec_ProbeIe(radio.macSec, rx, frame->lqi, frame->rssi, Board_Get()->rxSensitivityDbm, ie);
    n = MacSec_BuildEnhAckHeader(radio.macSec, raw->psdu, raw->len, radio.extAddr, fp, ie, ieLen, &ack[1], &result, &later);

    if (n == 0u)
        return 0;

    ack[0] = n;

    if (!radioIsrAckStart(stamp, ack, (uint8_t)(1u + n - RADIO_FCS_LEN)))
        return 0;

    if (later.adataLen != 0u)
    {
        uint8_t mic[4];

        if (!MacSec_EnhAckMic(&later, &ack[1], mic) || !Rf_SramWrite((uint8_t)(1u + later.adataLen), mic, 4u))
            radio.isrSpiFail++;
    }

    frame->ackedSec = result.secured;
    frame->ackKeyIndex = result.keyIndex;
    frame->ackFrameCounter = result.frameCounter;
    return 1;
}
#endif

/* TRX_END in RX_ON: one received frame. The whole frame is read before anything else - the
 * Frame Buffer is shared with transmission, so the Ack can only be written once its frame is
 * out - then filtered, answered, and queued. */
static void radioIsrRxEnd(uint32_t stamp, uint32_t stampUs)
{
    Rf_RxFrame *raw = &radio.isrRaw;
    Radio_Frame *frame = &radio.isrFrame;
    Mac_Hdr hdr;
    int parsed;
    int fp = 0;
    int ackDue;
    uint32_t t0 = Tick_CycleStamp();
    uint32_t c;

    if (!Rf_FrameBufferRead(raw) || raw->len > RADIO_PSDU_MAX)
    {
        radio.isrReadFail++;
        return;
    }

    c = Tick_CyclesSince(t0);
    radio.lastReadCycles = (uint16_t)(c > 0xFFFFu ? 0xFFFFu : c);
    radio.isrFrames++;

    frame->len = raw->len;
    /* The Frame Buffer's ED byte is the energy over this frame's reception, in 1 dB steps
     * above -94 dBm (at86rf233 datasheet 8.5, Table 6-3). */
    frame->rssi = (int8_t)(RADIO_ED_BASE_DBM + (int)raw->ed);
    frame->lqi = raw->lqi;
    frame->channel = radio.channel;
#ifdef RCP_TX_TIMING
    frame->timestamp = stampUs - RADIO_TRX_END_TIRQ_US - ((uint32_t)raw->len + 1u) * RADIO_OCTET_US;
    frame->ackedSec = 0;
    frame->ackKeyIndex = 0u;
    frame->ackFrameCounter = 0u;
#else
    (void)stampUs;
    frame->timestamp = Radio_Now();
#endif
    frame->crcOk = (raw->rxStatus & RADIO_RX_STATUS_CRC_VALID) ? 1 : 0;
    frame->ackedFp = 0;

    if (!frame->crcOk)
        radio.isrCrcBad++;

    if (!frame->crcOk && radio.rxFilter != RADIO_RX_ALL)
    {
        /* FCS-invalid frames reach RX_ON too; only the sniffer mode delivers them. */
        radio.counters.rxUnfiltered++;
        return;
    }

    parsed = Mac_ParseHeader(raw->psdu, raw->len, &hdr);

    if (radio.rxFilter == RADIO_RX_FILTERED && (!parsed || !Mac_Filter(&hdr, &radio.ackCfg.filter)))
    {
        radio.counters.rxUnfiltered++;
        return;
    }

    ackDue = parsed && Mac_AckDecide(&hdr, raw->psdu, raw->len, frame->crcOk, &radio.ackCfg,
                                     &radio.srcMatch, &fp);

    /* No room means the host never sees this frame, so it is not acknowledged either: the
     * sender retries, which an Ack would have told it not to. */
    if (ackDue && !radioRxRoom())
    {
        radio.isrAckNoRoom++;
        ackDue = 0;
    }

    if (ackDue)
    {
        int sent;

#ifdef RCP_TX_TIMING
        MacSec_AckRx rx;

        if (hdr.version == RADIO_FCF_VERSION_2015 && radio.macSec != 0 && MacSec_ParseAckRx(raw->psdu, raw->len, &rx))
        {
            fp = MacSec_AckPending(&rx, &radio.srcMatch);
            sent = radioIsrEnhAck(stamp, raw, &rx, fp, frame);
        }
        else
#endif
            sent = radioIsrAck(stamp, hdr.seq, fp);

        if (sent)
        {
            radio.isrPhase = RADIO_ISR_ACK;
            radio.ackSince = radioNow();
            radio.isrAcks++;

            if (fp)
                radio.isrAcksPending++;

            frame->ackedFp = fp;
        }
        else
        {
            /* Back to listening; the frame is still delivered, unacknowledged. */
            (void)radioSetState(RADIO_CMD_RX_ON);
        }
    }

    {
        uint8_t i;

        for (i = 0; i < raw->len; ++i)
            frame->psdu[i] = raw->psdu[i];
    }

    radioRxPush(frame);
}

#ifdef RCP_TX_TIMING
/* TRX_END while our frame's Ack is awaited. Anything else that arrived is dropped, unacknowledged, as
 * ARET drops it (at86rf233 datasheet 7.2.4): its sender tries again. */
static void radioIsrTxAck(uint32_t stampUs)
{
    Rf_RxFrame *raw = &radio.isrRaw;
    Radio_Frame *ack = &radio.txAckFrame;
    uint8_t i;

    if (!Rf_FrameBufferRead(raw))
    {
        radio.isrReadFail++;
        return;
    }

    if ((raw->rxStatus & RADIO_RX_STATUS_CRC_VALID) == 0u || raw->len < 5u ||
        (raw->psdu[0] & RADIO_FCF_TYPE_MASK) != RADIO_FCF_TYPE_ACK || raw->psdu[2] != radio.txSeq)
        return;

    ack->len = raw->len;
    ack->rssi = (int8_t)(RADIO_ED_BASE_DBM + (int)raw->ed);
    ack->lqi = raw->lqi;
    ack->channel = radio.channel;
    ack->timestamp = stampUs - RADIO_TRX_END_TIRQ_US - ((uint32_t)raw->len + 1u) * RADIO_OCTET_US;
    ack->crcOk = 1;
    ack->ackedFp = 0;
    ack->ackedSec = 0;

    for (i = 0u; i < raw->len; ++i)
        ack->psdu[i] = raw->psdu[i];

    radio.isrPhase = RADIO_ISR_RX;
    radio.txEvent = RADIO_TXEV_ACKED;
}
#endif

/* The transceiver's IRQ (PB00, EIC EXTINT0). One IRQ_STATUS read per entry: it clears the
 * register and drops the pin; causes the vector does not act on go to the mirror. */
void EIC_Handler(void)
{
    uint32_t stamp = Tick_CycleStamp();
#ifdef RCP_TX_TIMING
    uint32_t stampUs = Tick_ClockUs();
#else
    uint32_t stampUs = 0u;
#endif
    uint8_t bits = 0;

    HAL_IrqEnter();
    Rf_HandlerEnter();

    if (!Rf_HandlerIrqStatus(&bits))
    {
        radio.isrSpiFail++;
        Rf_HandlerExit();
        HAL_IrqExit();
        return;
    }

    if (bits & RADIO_IRQ_TRX_END)
    {
        switch (radio.isrPhase)
        {
            case RADIO_ISR_RX:
                bits = (uint8_t)(bits & (uint8_t)~RADIO_IRQ_TRX_END);
                radioIsrRxEnd(stamp, stampUs);
                break;

            case RADIO_ISR_ACK:
                bits = (uint8_t)(bits & (uint8_t)~RADIO_IRQ_TRX_END);
                (void)radioSetState(RADIO_CMD_RX_ON);
                radio.isrPhase = RADIO_ISR_RX;
                break;

#ifdef RCP_TX_TIMING
            case RADIO_ISR_TX:
                bits = (uint8_t)(bits & (uint8_t)~RADIO_IRQ_TRX_END);
                (void)radioSetState(RADIO_CMD_RX_ON);
                radio.txEndUs = stampUs;

                if (radio.txAckWait)
                {
                    radio.isrPhase = RADIO_ISR_TX_ACK;
                    radio.txEvent = RADIO_TXEV_WAIT;
                }
                else
                {
                    radio.isrPhase = RADIO_ISR_RX;
                    radio.txEvent = RADIO_TXEV_SENT;
                }

                break;

            case RADIO_ISR_TX_ACK:
                bits = (uint8_t)(bits & (uint8_t)~RADIO_IRQ_TRX_END);
                radioIsrTxAck(stampUs);
                break;
#endif

            default:
                break;
        }
    }

    radio.irqMirror = (uint8_t)(radio.irqMirror | bits);
    Rf_HandlerExit();
    HAL_IrqExit();
}

/* Confirm, then read - in that order, because the register keeps the last measurement's value
 * across a channel change (measured, docs/hardware_samr21.md §6): a read that is not preceded by this
 * channel's own confirmed request answers with the channel the scan left. */
static void radioPollEd(void)
{
    uint8_t level;
    uint8_t st;

    if (radio.edState != RADIO_ED_MEASURING)
        return;

    if ((radio.irqBits & RADIO_IRQ_ED_DONE) == 0u)
    {
        if ((int32_t)(radioNow() - radio.edDeadline) >= 0)
        {
            Debug_Emit(DBG_COMP_RF, 0u, DBG_RF_RSN_ED_TIMEOUT);
            radio.edState = RADIO_ED_FAILED;
        }
        return;
    }
    radio.irqBits = (uint8_t)(radio.irqBits & (uint8_t)~RADIO_IRQ_ED_DONE);

    if (!Rf_RegRead(RADIO_REG_PHY_ED_LEVEL, &level))
        return;

    if (level == RADIO_ED_TRIGGER_SENTINEL || level == RADIO_ED_LEVEL_INVALID)
    {
        /* The completion came and nothing was measured behind it: exactly the case the datasheet
       * warns about, reported rather than papered over with a stale number. */
        Debug_Emit(DBG_COMP_RF, (uint16_t)level, DBG_RF_RSN_ED_NO_MEASURE);
        radio.edState = RADIO_ED_FAILED;
        return;
    }

    if (level > RADIO_ED_LEVEL_MAX)
        level = RADIO_ED_LEVEL_MAX;

    /* An energy value is not refreshed during a reception, even for a manual request: a level
     * read while the transceiver is BUSY_RX belongs to that frame. */
    if (Rf_TrxStatus(&st) && (st & RADIO_ST_MASK) == RADIO_ST_BUSY_RX)
    {
        radio.edFresh = 0;
    }
    else
    {
        radio.edFresh = 1;
    }

    radio.edDbm = (int8_t)(RADIO_ED_BASE_DBM + (int)level);
    radio.edState = RADIO_ED_READY;
}

#ifdef RCP_TX_TIMING
/* From RX_ON to PLL_ON unless the radio is occupied. 1 = the bus is left held, for radioTxGo. */
static int radioTxPrepare(void)
{
    uint8_t st;

    Rf_Lock();

    if (radio.isrPhase != RADIO_ISR_RX || Rf_IrqPinHigh() || !Rf_TrxStatus(&st) ||
        (st & RADIO_ST_MASK) == RADIO_ST_BUSY_RX || !radioSetState(RADIO_CMD_FORCE_PLL_ON) ||
        !Rf_WaitTrxState(RADIO_ST_PLL_ON))
    {
        Rf_Unlock();
        return 0;
    }

    radio.isrPhase = RADIO_ISR_OFF;
    return 1;
}

/* TX_START, then the frame behind it: the bus outruns the air, but the PHR has only 176 us (at86rf233
 * datasheet 10.2), so nothing long may come between them. The caller has masked interrupts. A USB
 * vector is long, so they stay masked through the upload, at most 500 us. A link at 115 200 cannot hold
 * its octets that long, and on such a board only its vector and the tick can run here, each for
 * microseconds, so they go back on behind the command (PRODUCT_TX_UPLOAD_UNMASKED). */
static void radioTxStartUpload(void)
{
    radio.txEvent = RADIO_TXEV_NONE;
    radio.isrPhase = RADIO_ISR_TX;
    (void)radioSetState(RADIO_CMD_TX_START);
#ifdef PRODUCT_TX_UPLOAD_UNMASKED
    HAL_IrqUnlock();
#endif
    (void)Rf_FrameBufferWrite(radio.txPsdu, radio.txLen);
#ifndef PRODUCT_TX_UPLOAD_UNMASKED
    HAL_IrqUnlock();
#endif
    Rf_Unlock();
    radio.txDeadline = radioNow() + RADIO_TX_DEADLINE_MS;
    radio.txPhase = RADIO_TX_SENDING;
}

static void radioCcaRequest(void)
{
    uint8_t v;

    Rf_Lock();

    if (Rf_RegRead(RADIO_REG_PHY_CC_CCA, &v))
        (void)Rf_RegWrite(RADIO_REG_PHY_CC_CCA, (uint8_t)(v | RADIO_CCA_REQUEST));

    Rf_Unlock();
}

/* 1 = the assessment finished and found the channel idle, with nothing received meanwhile. */
static int radioCcaClear(void)
{
    uint8_t st = 0u;

    return Rf_TrxStatus(&st) && (st & RADIO_TRX_STATUS_CCA_DONE) && (st & RADIO_TRX_STATUS_CCA_IDLE) &&
           (st & RADIO_ST_MASK) == RADIO_ST_RX_ON;
}

static void radioBackoffNext(void)
{
    radio.rng = (radio.rng ^ Tick_CycleStamp()) * 0x41C64E6DuL + 12345u;
    radio.txAtUs = Tick_ClockUs() + (((radio.rng >> 16) & ((1uL << radio.txBe) - 1u)) * RADIO_CSMA_UNIT_US);
    radio.txPhase = RADIO_TX_BACKOFF;
}

static void radioBackoffFirst(void)
{
    radio.txBe = RADIO_CSMA_MIN_BE;
    radio.txBackoffs = 0u;
    radio.txDeadline = radioNow() + RADIO_TX_DEADLINE_MS;
    radioBackoffNext();
}

/* The channel was busy, or the radio was. */
static void radioBackoffAgain(void)
{
    if (radio.txBackoffs >= RADIO_CSMA_MAX_BACKOFFS)
    {
        DBG(DBG_COMP_RF, DBG_RF_RSN_TX_CCA_FAIL);
        radioTxFinish(RADIO_TX_CCA_FAIL, 0);
        return;
    }

    radio.txBackoffs++;

    if (radio.txBe < RADIO_CSMA_MAX_BE)
        radio.txBe++;

    radioBackoffNext();
}

/* A transmitter that is off still listens for the moment it sends: CCA, and then the Ack. */
static int radioTxListening(void)
{
    return radio.isrPhase != RADIO_ISR_OFF || radioEnterRx();
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

        while ((int32_t)(Tick_ClockUs() - n) < (int32_t)RADIO_CCA_US)
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

    HAL_IrqLock();
    radioTxStartUpload();
}

/* One attempt. The moment is honoured to the microsecond when nothing else runs in its last stretch, so
 * the wait ends with interrupts masked; anything that occupies the radio then has cost the window, which
 * completes as radio.h says it must. */
static void radioPollTimed(void)
{
    uint32_t start = radio.txAtUs - RADIO_TX_TO_SFD_END_US - RADIO_TIMED_CMD_US;
    uint32_t need = RADIO_TIMED_PLL_US + (radio.txCca ? RADIO_CCA_US : 0u);
    int32_t left = (int32_t)(start - Tick_ClockUs());

    if (left < (int32_t)need)
    {
        radioTxFinish(RADIO_TX_CCA_FAIL, 0);
        return;
    }

    if (left > (int32_t)RADIO_TIMED_SPIN_US)
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

        while ((int32_t)(start - Tick_ClockUs()) > (int32_t)RADIO_TIMED_PLL_US)
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

    HAL_IrqLock();

    if ((int32_t)(start - Tick_ClockUs()) < 0)
    {
        HAL_IrqUnlock();
        (void)radioSetState(RADIO_CMD_RX_ON);
        radio.isrPhase = RADIO_ISR_RX;
        Rf_Unlock();
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
    uint8_t st;

    if (event == RADIO_TXEV_SENT)
    {
        radioTxFinish(RADIO_TX_OK, 0);
        return;
    }

    if (event == RADIO_TXEV_NONE)
        return;

    if (event == RADIO_TXEV_ACKED)
    {
        radio.txAckFrame.timestamp = radioExtendUs((uint32_t)radio.txAckFrame.timestamp);
        radio.txAckHave = 1;
        radioTxFinish(RADIO_TX_OK, (radio.txAckFrame.psdu[0] & RADIO_FCF_FRAME_PENDING) ? 1 : 0);
        return;
    }

    if (radio.txPhase != RADIO_TX_ACK_WAIT)
    {
        radio.txPhase = RADIO_TX_ACK_WAIT;
        radio.txAckExtraUs = 0u;
    }

    if ((int32_t)(Tick_ClockUs() - (radio.txEndUs + RADIO_ACK_WAIT_US + radio.txAckExtraUs)) < 0)
        return;

    /* A reception under way by then could be the Ack: it gets the time it needs, once. One that has just
     * ended is on its way through the vector, so the wait never ends on the first look. */
    Rf_Lock();

    if (radio.txAckExtraUs == 0u)
    {
        radio.txAckExtraUs = (Rf_TrxStatus(&st) && (st & RADIO_ST_MASK) == RADIO_ST_BUSY_RX) ? RADIO_ACK_RX_US : RADIO_ACK_END_US;
        Rf_Unlock();
        return;
    }

    if (radio.txEvent == RADIO_TXEV_ACKED)
    {
        Rf_Unlock();
        return;
    }

    radio.isrPhase = RADIO_ISR_RX;
    Rf_Unlock();

    if (radio.txTriesLeft == 0u)
    {
        DBG(DBG_COMP_RF, DBG_RF_RSN_TX_NO_ACK);
        radioTxFinish(RADIO_TX_NO_ACK, 0);
        return;
    }

    radio.txTriesLeft--;
    radioBackoffFirst();
}
#endif

static void radioPollTx(void)
{
    uint8_t st;
    uint8_t trac;

    /* One guard covers every phase: an early return on a failed register read would leave
   * txPhase non-idle forever, so the deadline would never fire and the TX-done handler
   * would never report an outcome. */
    if ((int32_t)(radioNow() - radio.txDeadline) >= 0)
    {
        DBG(DBG_COMP_RF, DBG_RF_RSN_TX_TIMEOUT);
        radioTxFinish(RADIO_TX_TIMEOUT, 0);
        return;
    }

    switch (radio.txPhase)
    {
        case RADIO_TX_TO_ARET:
            /* Every frame uses TX_ARET_ON: it performs the CSMA-CA and, when the frame
       * does not request an acknowledgment, ends the transaction with TRX_END and
       * TRAC_STATUS = SUCCESS right after the frame (at86rf233.txt:7145-7154).
       *
       * The radio is taken from the vector only between frames: not while an Ack is on air,
       * not while a received frame waits in the buffer (the IRQ pin is high), and not
       * mid-reception. Each of those defers to the next pass; the phase deadline still
       * bounds the wait. TX_ARET_ON is entered from PLL_ON or TRX_OFF only (at86rf233
       * datasheet 7.2.2), so RX_ON goes through FORCE_PLL_ON first. */
            Rf_Lock();

            if (radio.isrPhase == RADIO_ISR_ACK || Rf_IrqPinHigh() || !Rf_TrxStatus(&st) ||
                (st & RADIO_ST_MASK) == RADIO_ST_BUSY_RX)
            {
                Rf_Unlock();
                return;
            }

            radio.isrPhase = RADIO_ISR_OFF;

            if ((st & RADIO_ST_MASK) != RADIO_ST_TRX_OFF &&
                (!radioSetState(RADIO_CMD_FORCE_PLL_ON) || !Rf_WaitTrxState(RADIO_ST_PLL_ON)))
            {
                Rf_Unlock();
                radioTxFinish(RADIO_TX_TIMEOUT, 0);
                return;
            }

            if (!radioSetState(RADIO_CMD_TX_ARET_ON))
            {
                Rf_Unlock();
                radioTxFinish(RADIO_TX_TIMEOUT, 0);
                return;
            }

            Rf_Unlock();
            radio.txDeadline = radioNow() + RADIO_STATE_DEADLINE_MS;
            radio.txPhase = RADIO_TX_WAIT_ARET;
            break;

        case RADIO_TX_WAIT_ARET:
            if (!Rf_TrxStatus(&st))
                return;

            /* ARET waits for the state it was asked for, then sends. */
            if ((st & RADIO_ST_MASK) != RADIO_ST_TX_ARET_ON &&
                (st & RADIO_ST_MASK) != RADIO_ST_BUSY_TX_ARET)
            {
                if ((int32_t)(radioNow() - radio.txDeadline) >= 0)
                {
                    DBG(DBG_COMP_RF, DBG_RF_RSN_TX_TIMEOUT);
                    radioTxFinish(RADIO_TX_TIMEOUT, 0);
                }

                return;
            }

            if (!Rf_FrameBufferWrite(radio.txPsdu, radio.txLen) ||
                !radioSetState(RADIO_CMD_TX_START))
            {
                radioTxFinish(RADIO_TX_TIMEOUT, 0);
                return;
            }

            radio.txDeadline = radioNow() + RADIO_TX_DEADLINE_MS * (radio.txAckWait ? radio.txRetries + 1u : 1u);
            radio.txPhase = RADIO_TX_WAIT_END;
            break;

        case RADIO_TX_WAIT_END:
            /* TRX_END is *after* the acknowledgment or, when no ACK is requested, right
       * after the frame; TRAC_STATUS says how the transaction went. Reading IRQ_STATUS
       * clears the flag, and ARET passes through BUSY_TX_ARET, so the state readback
       * alone would look finished the moment transmission started. */
            if ((radio.irqBits & RADIO_IRQ_TRX_END) == 0u)
            {
                if ((int32_t)(radioNow() - radio.txDeadline) >= 0)
                {
                    DBG(DBG_COMP_RF, DBG_RF_RSN_TX_TIMEOUT);
                    radioTxFinish(RADIO_TX_TIMEOUT, 0);
                }

                return;
            }

            if (!Rf_RegRead(RADIO_REG_TRX_STATE, &trac))
            {
                radioTxFinish(RADIO_TX_TIMEOUT, 0);
                return;
            }

            trac = (uint8_t)((trac & RADIO_TRAC_STATUS_MASK) >> RADIO_TRAC_STATUS_SHIFT);

            /* The RF233 reports the received ACK's Frame Pending subfield through
             * TRAC_STATUS: SUCCESS_DATA_PENDING (1) is SUCCESS with that bit set
             * (at86rf233.txt:7195-7225). The host needs it to know whether more data
             * waits for a sleepy child, so carry it out on the success path. */
            if (trac == RADIO_TRAC_SUCCESS || trac == RADIO_TRAC_SUCCESS_PD)
                radioTxFinish(RADIO_TX_OK, trac == RADIO_TRAC_SUCCESS_PD ? 1 : 0);
            else if (trac == RADIO_TRAC_NO_ACK)
            {
                DBG(DBG_COMP_RF, DBG_RF_RSN_TX_NO_ACK);
                radioTxFinish(RADIO_TX_NO_ACK, 0);
            }
            else if (trac == RADIO_TRAC_CHAN_FAIL)
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

#ifdef RCP_TX_TIMING
        case RADIO_TX_BACKOFF:
            radioPollBackoff();
            break;

        case RADIO_TX_TIMED:
            radioPollTimed();
            break;

        case RADIO_TX_SENDING:
        case RADIO_TX_ACK_WAIT:
            radioPollSending();
            break;
#endif

        default:
            break;
    }
}

void Radio_Poll(void)
{
    if (!radio.inited)
        return;

#ifdef RCP_TX_TIMING
    (void)Radio_ClockUs(); /* often enough never to miss the 32-bit clock's wrap */
#endif

    /* The vector read IRQ_STATUS; take what it left for the thread. Each consumer then takes
     * the bits it acts on, so a transmit's TRX_END is never mistaken for anything else. */
    Rf_Lock();
    radio.irqBits = radio.irqMirror;
    radio.irqMirror = 0u;

    /* An Ack whose TRX_END never came would leave the vector waiting for it forever. */
    if (radio.isrPhase == RADIO_ISR_ACK &&
        (int32_t)(radioNow() - (radio.ackSince + RADIO_ACK_STUCK_MS)) >= 0)
    {
        radio.isrAckStuck++;
        (void)radioSetState(RADIO_CMD_FORCE_PLL_ON);
        (void)radioSetState(RADIO_CMD_RX_ON);
        radio.isrPhase = RADIO_ISR_RX;
    }

    Rf_Unlock();

    if (radio.txPhase != RADIO_TX_IDLE)
        radioPollTx();

    radioPollEd();
    radioRxDeliver();
}

/* Field-by-field for the same memcpy reason (see radioRxPush). */
void Radio_GetCounters(Radio_Counters *out)
{
    if (out == 0)
        return;

    out->rxFrames = radio.counters.rxFrames;
    out->rxDropped = radio.counters.rxDropped;
    out->rxCrcBad = radio.counters.rxCrcBad;
    out->txFrames = radio.counters.txFrames;
    out->txFailed = radio.counters.txFailed;
    out->edRequests = radio.counters.edRequests;
}

void Radio_SetRxHandler(void (*fn)(const Radio_Frame *frame))
{
    radio.rxHandler = fn;
}

int Radio_SetSrcMatch(const SrcMatch_Table *table)
{
    if (!radio.inited || table == 0)
        return 0;

    Rf_Lock();
    SrcMatch_CopyOnAir(&radio.srcMatch, table);
    Rf_Unlock();
    return 1;
}

void Radio_SetTxDoneHandler(void (*fn)(void *token, int status, int framePending))
{
    radio.txDoneHandler = fn;
}

#ifdef RCP_TX_TIMING
uint64_t Radio_ClockUs(void)
{
    uint32_t now = Tick_ClockUs();

    if (now < radio.clockLast)
        radio.clockHigh++;

    radio.clockLast = now;
    return ((uint64_t)radio.clockHigh << 32) | now;
}
#else
/* Millisecond resolution where a board has no timed transmit. */
uint64_t Radio_ClockUs(void)
{
    uint32_t ms;

    Tick_GetMs(&ms);
    return (uint64_t)ms * 1000u;
}
#endif

uint32_t Radio_Now(void)
{
    uint32_t ms;

    Tick_GetMs(&ms);
    return ms * 1000u; /* microseconds; 1 ms resolution, by design */
}
