/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mg21_radio.c — the ConBee III radio back-end: the EFR32MG21's 802.15.4 receiver.

The radio is driven the way docs/hardware_mg21.md §10.5 measured it can be: the radio controller
(RAC) runs this tree's own sequencer program from SEQRAM (mg21_seq_image.c, assembled from
tools/mg21_seq.py), whose RXWARM handler brings the receiver up - synthesiser enables, the VCO search,
SYNTHSTART, the LNA and the RX path - and whose RXPD handler takes it down. The CPU's part is the
configuration: the bring-up's register table and the channel tune (mg21_radio_cfg.c, values recorded
from the vendor bring-up), the receive enable, and reading frames out of the receive buffer, which the
frame controller fills on its own: every record a length octet, the PSDU without its FCS, an RSSI and a
status octet and a timestamp (mg21_phy.h).

What this back-end is, as a state machine: DOWN until Radio_Init has installed and configured the
radio, then IDLE (the RAC in OFF) or RX (a software receive enable held). An RX buffer overflow parks
the RAC in RXOVERFLOW; Radio_Poll recovers it - receive withdrawn, buffer cleared, CLEARRXOVERFLOW,
receive enabled again - and counts the frames lost.

Transmission is the same split. Radio_Tx withdraws a running receive (the RAC passes RXPD into OFF),
loads the frame into BUFC buffer 0 - the PHY length octet, then the PSDU without its FCS, which the
radio appends - and issues TXEN; the program's TXWARM brings the 10 dBm PA up, the frame controller
sends, and the program's TX decides what follows: with the RX_AFTER_TX cell set (an Ack is due, or the
host keeps receiving) TX2RX turns straight to receive, fast enough for an Ack 192 us later; otherwise
TXPD powers down. Radio_Poll follows the transmission as a state machine of its own (TX_SENT, then
TX_ACK_WAIT for an acknowledged frame) and reports it once.

What it does not do yet (docs/hardware_mg21.md §10.5): compose an Ack for a received frame, assess the
channel before transmitting, or set a power other than the stock firmware's 10 dBm. The radio receives
promiscuously and the third level filter (rcp/mac.c) runs here, in software, for RADIO_RX_FILTERED.
Frames with a bad FCS are discarded by the frame controller in every mode, so RADIO_RX_ALL delivers
only good ones. */
#include <stdint.h>

#include "dbg.h"
#include "hal.h"
#include "mac.h"
#include "mg21_phy.h"
#include "mg21_radio.h"
#include "mg21_radio_cfg.h"
#include "mg21_regs.h"
#include "radio.h"
#include "srcmatch.h"

/* Line fields of DBG_RADIO_RSN_REFUSED and DBG_RADIO_RSN_UNSUPPORTED (hal/dbg.h). */
enum
{
    MG21_RADIO_REFUSE_OFF_TIMEOUT = 1, /* the RAC did not reach OFF for the install */
    MG21_RADIO_REFUSE_BUFFER_RAM       /* the receive buffers are outside BUFC's 24-bit window */
};

enum
{
    MG21_RADIO_CALL_CCA = 1 /* a clear-channel assessment was asked for: the frame goes without one */
};

typedef enum
{
    MG21_RADIO_DOWN = 0,
    MG21_RADIO_IDLE,
    MG21_RADIO_RX
} Mg21Radio_State;

/* The transmission in flight, one at a time. */
typedef enum
{
    MG21_TX_IDLE = 0,
    MG21_TX_SENT,     /* TXEN issued; waiting for the frame controller's TXDONE */
    MG21_TX_ACK_WAIT  /* sent, an Ack requested; the receiver is on for it */
} Mg21Radio_TxPhase;

/* BUFC's buffers: the bring-up table configures three of them (BUF1 the receive FIFO, 512 bytes;
 * BUF2 128 and BUF3 1024, which the frame controller uses beside it). They are carved from one pool,
 * each at an address aligned to its size, so the pool is a kilobyte larger than the buffers. */
#define RADIO_BUF_ALIGN 1024u
#define RADIO_BUF3_BYTES 1024u
#define RADIO_BUF1_BYTES 512u
#define RADIO_BUF2_BYTES 128u
#define RADIO_BUF0_BYTES 128u /* the transmit buffer: one frame */
#define RADIO_BUF0_SIZE_CODE 1u /* 64 << 1 */
#define RADIO_POOL_BYTES                                                                           \
    (RADIO_BUF_ALIGN + RADIO_BUF3_BYTES + RADIO_BUF1_BYTES + RADIO_BUF2_BYTES + RADIO_BUF0_BYTES)
#define RADIO_TX_BUF 0u
#define RADIO_RX_BUF 1u
#define RADIO_BUFC_WINDOW 0x01000000u /* BUFn_ADDR's 24 bits above RAMBASEADDR */

/* Bounded waits, in polls of RAC.STATUS: a power-down measures 5 us and an overflow recovery none at
 * all, so these are far longer than any measured transition and still finite. */
#define RADIO_OFF_WAIT_LIMIT 0x4000u

#define RADIO_RECORD_MAX (RADIO_PSDU_MAX + MG21_PHY_RECORD_OVERHEAD)

/* A transmission's bounds, in milliseconds of the board's tick: a 127-octet frame is 4.3 ms on air
 * with its warm-up, and an Ack follows the frame by 192 us and lasts 352 us (802.15.4 macAckWaitDuration
 * is 864 us), so both are generous at this resolution. */
#define RADIO_TX_DONE_MS 15u
#define RADIO_TX_ACK_MS 3u
#define RADIO_ACK_PHY_LEN 5u
#define RADIO_FCF_TYPE_MASK 0x07u
#define RADIO_FCF_TYPE_ACK 0x02u
#define RADIO_FCF_FRAME_PENDING 0x10u
#define RADIO_SEQ_CELL(off) (*(volatile uint32_t *)(MG21_SEQRAM_BASE + (off)))
#define RADIO_DEFAULT_CHANNEL 11u
#define RADIO_DEFAULT_DBM 10 /* the one level measured: the stock firmware's 10 dBm */

#define RAC(off) MG21_RADIO_REG(MG21_RAC_BASE, (off))
#define FRC(off) MG21_RADIO_REG(MG21_FRC_BASE, (off))
#define MODEM(off) MG21_RADIO_REG(MG21_MODEM_BASE, (off))
#define SYNTH(off) MG21_RADIO_REG(MG21_SYNTH_BASE, (off))
#define AGC(off) MG21_RADIO_REG(MG21_AGC_BASE, (off))
#define BUFC(n, off) MG21_RADIO_REG(MG21_BUFC_BASE, (off) + (n) * MG21_BUFC_BUF_STRIDE)

typedef struct
{
    Mg21Radio_State state;
    uint8_t channel;
    int8_t power;
    uint8_t rxFilter;
    uint8_t panOn;
    uint8_t shortOn;
    uint8_t extOn;
    uint16_t panId;
    uint16_t shortAddr;
    uint8_t extAddr[8]; /* Spinel order, MSB first */
    Mac_AckCfg ackCfg;
    SrcMatch_Table srcMatch;

    uint32_t bufAddr[4]; /* each BUFC buffer's RAM, 0 for one the table does not use */
    uint8_t record[RADIO_RECORD_MAX];
    Radio_Frame rxRing[RADIO_RX_SLOTS];
    uint8_t rxHead;
    uint8_t rxTail;
    void (*rxHandler)(const Radio_Frame *frame);
    void (*txDoneHandler)(void *token, int status, int framePending);

    int edState;
    int8_t edDbm;

    Mg21Radio_TxPhase txPhase;
    uint8_t txPsdu[RADIO_TX_PSDU_MAX];
    uint8_t txLen;
    uint8_t txSeq;
    uint8_t txTriesLeft;
    int txAck;
    int txAcked;
    int txAckFp;
    uint32_t txSince;
    void *txToken;

    Radio_Counters counters;
    uint32_t overflows;
} Mg21Radio;

static Mg21Radio s_radio;
static uint32_t s_radioPool[RADIO_POOL_BYTES / 4u];

/* ---- small helpers ---- */

static uint32_t radioState(void)
{
    return (RAC(MG21_RAC_STATUS_OFFSET) & MG21_RAC_STATUS_STATE_MASK) >> MG21_RAC_STATUS_STATE_SHIFT;
}

/* Wait for the RAC to settle in OFF; 1 = it did. */
static int radioWaitOff(void)
{
    uint32_t n;

    for (n = 0u; n < RADIO_OFF_WAIT_LIMIT; ++n)
    {
        if (radioState() == MG21_RAC_STATUS_STATE_OFF)
            return 1;
    }

    Debug_Emit(DBG_COMP_RADIO, (uint16_t)radioState(), DBG_RADIO_RSN_OFF_TIMEOUT);
    return 0;
}

/* The receive withdrawn: the RAC passes RXPD, where the sequencer program powers the receiver down. */
static int radioRxStop(void)
{
    RAC(MG21_RAC_RXENSRCEN_OFFSET) = 0u;
    return radioWaitOff();
}

static void radioRxStart(void)
{
    RAC(MG21_RAC_CMD_OFFSET) = MG21_RAC_CMD_CLEARRXOVERFLOW;
    RAC(MG21_RAC_RXENSRCEN_OFFSET) = MG21_RAC_RXENSRCEN_SWRXEN;
}

static int radioSyncFilter(void)
{
    uint8_t k;

    s_radio.ackCfg.filter.mode = (s_radio.rxFilter == RADIO_RX_FILTERED)
                                     ? MAC_FILTER_OFF
                                     : (s_radio.rxFilter == RADIO_RX_PAN ? MAC_FILTER_PAN : MAC_FILTER_ALL);
    s_radio.ackCfg.filter.panId = s_radio.panOn ? s_radio.panId : 0xFFFFu;
    s_radio.ackCfg.filter.shortAddr = s_radio.shortOn ? s_radio.shortAddr : 0xFFFFu;

    for (k = 0; k < SRCMATCH_EXT_LEN; ++k)
        s_radio.ackCfg.filter.extAddr[k] = s_radio.extOn ? s_radio.extAddr[7u - k] : 0xFFu;

    s_radio.ackCfg.isCoordinator = 0u;
    return 1;
}

/* The channel tune, with the RAC in OFF: the synthesiser word, the channel's MODEM and RAC values,
 * and the constants the tune writes beside them (docs/hardware_mg21.md §10.5). */
static void radioTune(uint8_t channel)
{
    const Mg21RadioCfg_Channel *c = &Mg21RadioCfg_Channels[channel - MG21_RADIO_CFG_CHANNEL_FIRST];

    SYNTH(MG21_SYNTH_FREQ_OFFSET) = Mg21Phy_SynthFreq(channel);
    MODEM(MG21_RADIO_CLR_ALIAS + MG21_MODEM_DIGMIXCTRL_OFFSET) = MG21_MODEM_DIGMIXCTRL_DIGMIXFREQ_MASK;
    MODEM(MG21_RADIO_SET_ALIAS + MG21_MODEM_DIGMIXCTRL_OFFSET) = c->digMix;
    MODEM(MG21_RADIO_CLR_ALIAS + MG21_MODEM_SRCCHF_OFFSET) = MG21_MODEM_SRCCHF_TUNED_MASK;
    MODEM(MG21_RADIO_SET_ALIAS + MG21_MODEM_SRCCHF_OFFSET) = c->srcChf;
    RAC(MG21_RAC_XORETIMECTRL_OFFSET) = c->xoRetime;
    RAC(MG21_RAC_DIGCLKRETIMECTRL_OFFSET) = 0u;
    FRC(MG21_FRC_WCNTCMP2_OFFSET) = (uint32_t)(channel - MG21_RADIO_CFG_CHANNEL_FIRST);
    SYNTH(MG21_SYNTH_CHCTRL_OFFSET) = 0u;
}

/* Carve the BUFC buffers from the pool; 0 when they would not be reachable through BUFn_ADDR. */
static int radioBuffers(void)
{
    uint32_t base = ((uint32_t)s_radioPool + RADIO_BUF_ALIGN - 1u) & ~(RADIO_BUF_ALIGN - 1u);

    s_radio.bufAddr[3] = base;
    s_radio.bufAddr[1] = base + RADIO_BUF3_BYTES;
    s_radio.bufAddr[2] = base + RADIO_BUF3_BYTES + RADIO_BUF1_BYTES;
    s_radio.bufAddr[0] = base + RADIO_BUF3_BYTES + RADIO_BUF1_BYTES + RADIO_BUF2_BYTES;

    return (s_radio.bufAddr[0] + RADIO_BUF0_BYTES) - MG21_SRAM_BASE <= RADIO_BUFC_WINDOW;
}

/* A write of the bring-up table; a buffer-address register gets the buffer's RAM instead of its index. */
static void radioCfgWrite(const Mg21RadioCfg_Write *w)
{
    uint32_t n;

    for (n = 0u; n < 4u; ++n)
    {
        if (w->addr == MG21_BUFC_BASE + MG21_BUFC_BUF0_ADDR_OFFSET + n * MG21_BUFC_BUF_STRIDE)
        {
            *(volatile uint32_t *)w->addr = s_radio.bufAddr[w->value & 3u];
            return;
        }
    }

    *(volatile uint32_t *)w->addr = w->value;
}

/* ---- the receive buffer ---- */

static int radioRxRoom(void)
{
    return (uint8_t)(s_radio.rxHead - s_radio.rxTail) < RADIO_RX_SLOTS;
}

/* The buffer's byte at the read offset plus k, without consuming it. */
static uint8_t radioRxPeek(uint32_t k)
{
    uint32_t off = BUFC(RADIO_RX_BUF, MG21_BUFC_BUF0_READOFFSET_OFFSET) + k;

    return *(volatile uint8_t *)(s_radio.bufAddr[RADIO_RX_BUF] + (off & (RADIO_BUF1_BYTES - 1u)));
}

static void radioRxClear(void)
{
    BUFC(RADIO_RX_BUF, MG21_BUFC_BUF0_CMD_OFFSET) = MG21_BUFC_BUF_CMD_CLEAR;
}

/* Take one complete record out of the receive buffer into s_radio.record; its length, or 0 when no
 * whole record is there yet. A record is consumed only once every byte of it has arrived: the frame
 * controller writes a frame as it is received and takes back one that fails, so a partial record is
 * not yet a record. */
static uint16_t radioRxTake(void)
{
    uint32_t avail = BUFC(RADIO_RX_BUF, MG21_BUFC_BUF0_STATUS_OFFSET) & MG21_BUFC_BUF_STATUS_BYTES_MASK;
    uint8_t phyLen;
    uint16_t need;
    uint16_t i;

    if (avail == 0u)
        return 0u;

    phyLen = radioRxPeek(0u);
    need = Mg21Phy_RecordBytes(phyLen);

    if (need == 0u)
    {
        /* Not a frame's length: the buffer is out of step with the frames in it, and the only way
         * back in step is to drop what it holds. */
        Debug_Emit(DBG_COMP_RADIO, phyLen, DBG_RADIO_RSN_RX_BAD_RECORD);
        radioRxClear();
        s_radio.counters.rxDropped++;
        return 0u;
    }

    if (avail < need)
        return 0u;

    for (i = 0u; i < need; ++i)
        s_radio.record[i] = (uint8_t)BUFC(RADIO_RX_BUF, MG21_BUFC_BUF0_READDATA_OFFSET);

    return need;
}

static void radioRxPush(const Radio_Frame *frame)
{
    Radio_Frame *dst;
    uint8_t i;

    if (!radioRxRoom())
    {
        s_radio.counters.rxDropped++;
        s_radio.counters.rxRingFull++;
        return;
    }

    dst = &s_radio.rxRing[s_radio.rxHead % RADIO_RX_SLOTS];
    dst->len = frame->len;
    dst->rssi = frame->rssi;
    dst->lqi = frame->lqi;
    dst->channel = frame->channel;
    dst->timestamp = frame->timestamp;
    dst->crcOk = frame->crcOk;
    dst->ackedFp = frame->ackedFp;

    for (i = 0u; i < frame->len; ++i)
        dst->psdu[i] = frame->psdu[i];

    s_radio.rxHead = (uint8_t)(s_radio.rxHead + 1u);
}

/* Records into the ring, through the mode's filter. */
static void radioRxDrain(void)
{
    static Radio_Frame frame;
    Mac_Hdr hdr;
    uint16_t n;

    while ((n = radioRxTake()) != 0u)
    {
        if (!Mg21Phy_ParseRecord(s_radio.record, n, &frame))
            continue;

        frame.channel = s_radio.channel;
        frame.timestamp = Radio_Now();
        frame.ackedFp = 0;
        frame.ackedSec = 0;
        frame.ackKeyIndex = 0u;
        frame.ackFrameCounter = 0u;

        /* The Ack of the frame in flight is the transmission's answer, not a frame for the host. */
        if (s_radio.txPhase == MG21_TX_ACK_WAIT && frame.len == RADIO_ACK_PHY_LEN &&
            (frame.psdu[0] & RADIO_FCF_TYPE_MASK) == RADIO_FCF_TYPE_ACK && frame.psdu[2] == s_radio.txSeq)
        {
            s_radio.txAcked = 1;
            s_radio.txAckFp = (frame.psdu[0] & RADIO_FCF_FRAME_PENDING) ? 1 : 0;
            continue;
        }

        if (s_radio.rxFilter == RADIO_RX_FILTERED &&
            (!Mac_ParseHeader(frame.psdu, frame.len, &hdr) || !Mac_Filter(&hdr, &s_radio.ackCfg.filter)))
        {
            s_radio.counters.rxFiltered++;
            continue;
        }

        radioRxPush(&frame);
    }
}

static void radioRxDeliver(void)
{
    Radio_Frame *f;

    while (s_radio.rxHead != s_radio.rxTail && s_radio.rxHandler != 0)
    {
        f = &s_radio.rxRing[s_radio.rxTail % RADIO_RX_SLOTS];
        s_radio.counters.rxFrames++;

        if (!f->crcOk)
            s_radio.counters.rxCrcBad++;

        s_radio.rxHandler(f);
        s_radio.rxTail = (uint8_t)(s_radio.rxTail + 1u);
    }
}

/* The RAC parks in RXOVERFLOW when the buffer filled; what was in it is still good, so it is read
 * out first, then the receive restarts on an empty buffer. */
static void radioRxRecover(void)
{
    radioRxDrain();
    (void)radioRxStop();
    radioRxClear();
    s_radio.overflows++;
    s_radio.counters.rxDropped++;
    Debug_Emit(DBG_COMP_RADIO, (uint16_t)s_radio.overflows, DBG_RADIO_RSN_RX_OVERFLOW);

    if (s_radio.state == MG21_RADIO_RX)
        radioRxStart();
}

/* ---- lifecycle ---- */

int Radio_Init(void)
{
    static const uint32_t blocks[] = {MG21_RAC_BASE, MG21_FRC_BASE, MG21_MODEM_BASE, MG21_SYNTH_BASE,
                                      MG21_AGC_BASE, MG21_BUFC_BASE, MG21_PROTIMER_BASE, MG21_RFCRC_BASE};
    uint32_t n;

    if (s_radio.state != MG21_RADIO_DOWN)
        return 1;

    if (!radioBuffers())
    {
        Debug_Emit(DBG_COMP_RADIO, MG21_RADIO_REFUSE_BUFFER_RAM, DBG_RADIO_RSN_REFUSED);
        return 0;
    }

    MG21_CMU->LOCK = MG21_CMU_LOCKKEY_UNLOCK;
    MG21_CMU->RADIOCLKCTRL = MG21_CMU_RADIOCLKCTRL_EN;

    /* The install, in the order the bench used: the state machine held in OFF, the program loaded and
     * SEQRAM behind it cleared, the configuration registers (written while RAC is still disabled),
     * the block enables, then the runtime state (docs/hardware_mg21.md §10.5). */
    RAC(MG21_RAC_RXENSRCEN_OFFSET) = 0u;
    RAC(MG21_RADIO_SET_ALIAS + MG21_RAC_CTRL_OFFSET) = MG21_RAC_CTRL_FORCEDISABLE;

    for (n = 0u; n < MG21_SEQRAM_BYTES / 4u; ++n)
        *(volatile uint32_t *)(MG21_SEQRAM_BASE + 4u * n) = (n < MG21_SEQ_IMAGE_WORDS) ? Mg21Seq_Image[n] : 0u;

    RAC(MG21_RAC_SR0_OFFSET) = 0u;
    RAC(MG21_RAC_SR1_OFFSET) = 0u;
    RAC(MG21_RAC_SR2_OFFSET) = 0u;
    FRC(MG21_FRC_CMD_OFFSET) = MG21_FRC_CMD_CONVINIT;
    RAC(MG21_RAC_VECTADDR_OFFSET) = MG21_SEQRAM_BASE;
    RAC(MG21_RAC_SEQCTRL_OFFSET) = MG21_RAC_SEQCTRL_COMPACT;

    for (n = 0u; n < (uint32_t)(sizeof(blocks) / sizeof(blocks[0])); ++n)
        MG21_RADIO_REG(blocks[n], MG21_RADIO_EN_OFFSET) = MG21_RADIO_EN_EN;

    RAC(MG21_RAC_R6_OFFSET) = MG21_SEQRAM_STACK;

    if (!radioWaitOff())
    {
        Debug_Emit(DBG_COMP_RADIO, MG21_RADIO_REFUSE_OFF_TIMEOUT, DBG_RADIO_RSN_REFUSED);
        return 0;
    }

    for (n = 0u; n < MG21_RADIO_CFG_BRINGUP_WRITES; ++n)
        radioCfgWrite(&Mg21RadioCfg_Bringup[n]);

    /* The part's own IR calibration pair (docs/hardware_mg21.md §10.5), then the receive
     * calibration command the bring-up ends with. */
    MODEM(MG21_MODEM_IRCALCOEFWR0_OFFSET) = *(volatile const uint32_t *)MG21_DEVINFO_IRCAL0;
    MODEM(MG21_MODEM_IRCALCOEFWR1_OFFSET) = *(volatile const uint32_t *)MG21_DEVINFO_IRCAL1;
    RAC(MG21_RAC_CMD_OFFSET) = MG21_RAC_CMD_RXCAL;

    /* The transmit side: buffer 0, the stock firmware's ramp and 10 dBm level, the program's cells. */
    BUFC(RADIO_TX_BUF, MG21_BUFC_BUF0_CTRL_OFFSET) = RADIO_BUF0_SIZE_CODE;
    BUFC(RADIO_TX_BUF, MG21_BUFC_BUF0_ADDR_OFFSET) = s_radio.bufAddr[RADIO_TX_BUF];
    BUFC(RADIO_TX_BUF, MG21_BUFC_BUF0_CMD_OFFSET) = MG21_BUFC_BUF_CMD_CLEAR;
    MODEM(MG21_RADIO_CLR_ALIAS + MG21_MODEM_RAMPCTRL_OFFSET) = MG21_MODEM_RAMPCTRL_RATES_MASK;
    MODEM(MG21_RADIO_SET_ALIAS + MG21_MODEM_RAMPCTRL_OFFSET) = MG21_RADIO_CFG_TX_RAMPCTRL;
    MODEM(MG21_MODEM_RAMPLEV_OFFSET) = MG21_RADIO_CFG_TX_LEVEL_10DBM * 0x010101u;
    RADIO_SEQ_CELL(MG21_SEQ_PA_CELL) = (uint32_t)MG21_RADIO_CFG_TX_LEVEL_10DBM << 16;
    RADIO_SEQ_CELL(MG21_SEQ_RX_AFTER_TX_CELL) = 0u;

    s_radio.channel = RADIO_DEFAULT_CHANNEL;
    s_radio.power = RADIO_DEFAULT_DBM;
    s_radio.rxFilter = RADIO_RX_FILTERED;
    (void)radioSyncFilter();
    radioTune(s_radio.channel);
    radioRxClear();

    RAC(MG21_RADIO_CLR_ALIAS + MG21_RAC_CTRL_OFFSET) = MG21_RAC_CTRL_FORCEDISABLE;
    s_radio.state = MG21_RADIO_IDLE;
    Debug_Emit(DBG_COMP_RADIO, (uint16_t)MG21_SEQ_IMAGE_WORDS, DBG_RADIO_RSN_UP);
    return 1;
}

/* ---- channel and power ---- */

int Radio_SetChannel(uint8_t channel)
{
    if (s_radio.state == MG21_RADIO_DOWN || channel < RADIO_CHAN_MIN || channel > RADIO_CHAN_MAX)
        return 0;

    if (channel == s_radio.channel)
        return 1;

    if (s_radio.txPhase != MG21_TX_IDLE)
        return 0; /* retried by the caller once the transmission is reported */

    /* The tune is written with the RAC in OFF; a running receive is withdrawn around it, and the warm-up
     * that follows calibrates the VCO for the new channel. */
    if (s_radio.state == MG21_RADIO_RX)
    {
        radioRxDrain();

        if (!radioRxStop())
            return 0;
    }

    s_radio.channel = channel;
    radioTune(channel);

    if (s_radio.state == MG21_RADIO_RX)
        radioRxStart();

    return 1;
}

int Radio_GetChannel(uint8_t *channel)
{
    if (s_radio.state == MG21_RADIO_DOWN || channel == 0)
        return 0;

    *channel = s_radio.channel;
    return 1;
}

/* The power is recorded and reported; every transmission goes at the stock firmware's 10 dBm setting,
 * the one level this tree has measured (docs/hardware_mg21.md §10.5). */
int Radio_SetPower(int8_t dbm)
{
    if (s_radio.state == MG21_RADIO_DOWN)
        return 0;

    s_radio.power = dbm;
    return 1;
}

int Radio_GetPower(int8_t *dbm)
{
    if (s_radio.state == MG21_RADIO_DOWN || dbm == 0)
        return 0;

    *dbm = s_radio.power;
    return 1;
}

/* Accepted and not applied: this back-end runs no clear-channel assessment (docs/hardware_mg21.md §10.5). */
int Radio_SetCcaThreshold(int8_t dbm)
{
    (void)dbm;
    return s_radio.state == MG21_RADIO_DOWN ? 0 : 1;
}

/* ---- receive ---- */

int Radio_SetRxFilter(int mode)
{
    if (s_radio.state == MG21_RADIO_DOWN || mode < RADIO_RX_FILTERED || mode > RADIO_RX_ALL)
        return 0;

    s_radio.rxFilter = (uint8_t)mode;
    return radioSyncFilter();
}

void Radio_Witnesses(void)
{
    if (s_radio.state == MG21_RADIO_DOWN)
        return;

    Debug_Emit(DBG_COMP_RADIO, (uint16_t)radioState(), DBG_RADIO_RSN_STATE);
}

int Radio_RxEnable(int on)
{
    if (s_radio.state == MG21_RADIO_DOWN)
        return 0;

    /* During a transmission only the wish is recorded; the transmission's end applies it. */
    if (s_radio.txPhase != MG21_TX_IDLE)
    {
        s_radio.state = on ? MG21_RADIO_RX : MG21_RADIO_IDLE;
        return 1;
    }

    if (on)
    {
        if (s_radio.state != MG21_RADIO_RX)
        {
            radioRxStart();
            s_radio.state = MG21_RADIO_RX;
        }

        return 1;
    }

    if (s_radio.state == MG21_RADIO_RX)
    {
        radioRxDrain();
        s_radio.state = MG21_RADIO_IDLE;
        return radioRxStop();
    }

    return 1;
}

int Radio_SetPanId(uint16_t panid, int on)
{
    if (s_radio.state == MG21_RADIO_DOWN)
        return 0;

    s_radio.panId = panid;
    s_radio.panOn = on ? 1u : 0u;
    return radioSyncFilter();
}

int Radio_SetShortAddr(uint16_t addr, int on)
{
    if (s_radio.state == MG21_RADIO_DOWN)
        return 0;

    s_radio.shortAddr = addr;
    s_radio.shortOn = on ? 1u : 0u;
    return radioSyncFilter();
}

int Radio_SetExtAddr(const uint8_t ext8[8], int on)
{
    uint8_t i;

    if (s_radio.state == MG21_RADIO_DOWN || ext8 == 0)
        return 0;

    for (i = 0; i < 8u; ++i)
        s_radio.extAddr[i] = ext8[i];

    s_radio.extOn = on ? 1u : 0u;
    return radioSyncFilter();
}

/* Kept for the Ack this back-end will compose once it transmits. */
int Radio_SetSrcMatch(const SrcMatch_Table *table)
{
    if (s_radio.state == MG21_RADIO_DOWN || table == 0)
        return 0;

    SrcMatch_CopyOnAir(&s_radio.srcMatch, table);
    return 1;
}

/* ---- transmit ---- */

/* Load the frame into buffer 0 and start it, from OFF. */
static void radioTxStart(void)
{
    uint8_t i;

    BUFC(RADIO_TX_BUF, MG21_BUFC_BUF0_CMD_OFFSET) = MG21_BUFC_BUF_CMD_CLEAR;
    BUFC(RADIO_TX_BUF, MG21_BUFC_BUF0_WRITEDATA_OFFSET) = s_radio.txLen;

    for (i = 0u; i < (uint8_t)(s_radio.txLen - 2u); ++i)
        BUFC(RADIO_TX_BUF, MG21_BUFC_BUF0_WRITEDATA_OFFSET) = s_radio.txPsdu[i];

    RADIO_SEQ_CELL(MG21_SEQ_RX_AFTER_TX_CELL) = (s_radio.txAck || s_radio.state == MG21_RADIO_RX) ? 1u : 0u;
    s_radio.txAcked = 0;
    s_radio.txAckFp = 0;
    FRC(MG21_RADIO_CLR_ALIAS + MG21_FRC_IF_OFFSET) = MG21_FRC_IF_TXDONE;
    RAC(MG21_RAC_CMD_OFFSET) = MG21_RAC_CMD_TXEN;
    s_radio.txPhase = MG21_TX_SENT;
    s_radio.txSince = Radio_Now() / 1000u;
}

int Radio_Tx(const uint8_t *psdu, uint8_t len, int8_t dbm, int cca, int maxRetries, void *token)
{
    uint8_t i;

    (void)dbm;

    if (s_radio.state == MG21_RADIO_DOWN || s_radio.txPhase != MG21_TX_IDLE || psdu == 0 ||
        len < MG21_PHY_LEN_MIN || len > RADIO_TX_PSDU_MAX)
        return 0;

    if (cca)
        Debug_Emit(DBG_COMP_RADIO, MG21_RADIO_CALL_CCA, DBG_RADIO_RSN_UNSUPPORTED);

    for (i = 0u; i < len; ++i)
        s_radio.txPsdu[i] = psdu[i];

    s_radio.txLen = len;
    s_radio.txSeq = psdu[2];
    s_radio.txAck = Mac_AckRequested(psdu, len);
    s_radio.txTriesLeft = (uint8_t)(s_radio.txAck ? (maxRetries < 0 ? 0 : (maxRetries > 15 ? 15 : maxRetries)) : 0);
    s_radio.txToken = token;

    /* A transmission starts from OFF: a running receive is read out and withdrawn first. */
    if (s_radio.state == MG21_RADIO_RX)
    {
        radioRxDrain();

        if (!radioRxStop())
            return 0;
    }

    radioTxStart();
    return 1;
}

/* The transmission's report, and the receiver set the way the host wants it after it. */
static void radioTxFinish(int status, int framePending)
{
    void *token = s_radio.txToken;

    s_radio.txPhase = MG21_TX_IDLE;
    RADIO_SEQ_CELL(MG21_SEQ_RX_AFTER_TX_CELL) = 0u;

    if (status == RADIO_TX_OK || status == RADIO_TX_NO_ACK)
        s_radio.counters.txFrames++;
    else
        s_radio.counters.txFailed++;

    if (s_radio.state == MG21_RADIO_RX)
    {
        /* TX2RX left the receiver running on the program's source bit; hold it on the CPU's. */
        RAC(MG21_RAC_RXENSRCEN_OFFSET) = MG21_RAC_RXENSRCEN_SWRXEN;

        if (radioState() == MG21_RAC_STATUS_STATE_OFF)
            radioRxStart();
    }
    else
    {
        (void)radioRxStop();
    }

    if (s_radio.txDoneHandler != 0)
        s_radio.txDoneHandler(token, status, framePending);
}

static void radioPollTx(void)
{
    uint32_t nowMs = Radio_Now() / 1000u;

    switch (s_radio.txPhase)
    {
    case MG21_TX_SENT:
        if (FRC(MG21_FRC_IF_OFFSET) & MG21_FRC_IF_TXDONE)
        {
            if (!s_radio.txAck)
            {
                radioTxFinish(RADIO_TX_OK, 0);
                return;
            }

            s_radio.txPhase = MG21_TX_ACK_WAIT;
            s_radio.txSince = nowMs;
        }
        else if (nowMs - s_radio.txSince > RADIO_TX_DONE_MS)
        {
            (void)radioRxStop();
            radioTxFinish(RADIO_TX_TIMEOUT, 0);
        }

        break;

    case MG21_TX_ACK_WAIT:
        radioRxDrain();

        if (s_radio.txAcked)
        {
            radioTxFinish(RADIO_TX_OK, s_radio.txAckFp);
        }
        else if (nowMs - s_radio.txSince > RADIO_TX_ACK_MS)
        {
            if (s_radio.txTriesLeft != 0u)
            {
                s_radio.txTriesLeft--;
                (void)radioRxStop();
                radioTxStart();
            }
            else
            {
                radioTxFinish(RADIO_TX_NO_ACK, 0);
            }
        }

        break;

    case MG21_TX_IDLE:
    default:
        break;
    }
}

/* ---- energy detection ----
 * The AGC keeps a running RSSI while the receiver searches; a measurement is a reading of it, legal
 * while the RAC is in RXSEARCH. */

int Radio_StartEd(void)
{
    if (s_radio.state != MG21_RADIO_RX || s_radio.txPhase != MG21_TX_IDLE ||
        radioState() != MG21_RAC_STATUS_STATE_RXSEARCH)
    {
        s_radio.edState = RADIO_ED_FAILED;
        return 0;
    }

    s_radio.counters.edRequests++;
    s_radio.edState = Mg21Phy_RssiDbm(AGC(MG21_AGC_RSSI_OFFSET), &s_radio.edDbm) ? RADIO_ED_READY
                                                                               : RADIO_ED_FAILED;
    return s_radio.edState == RADIO_ED_READY;
}

int Radio_TakeEd(int8_t *dbm, int *fresh)
{
    int st = s_radio.edState;

    s_radio.edState = RADIO_ED_IDLE;

    if (st != RADIO_ED_READY)
        return -1;

    if (dbm != 0)
        *dbm = s_radio.edDbm;

    if (fresh != 0)
        *fresh = 1;

    return 1;
}

void Radio_AbortEd(void)
{
    s_radio.edState = RADIO_ED_IDLE;
}

/* ---- housekeeping ---- */

void Radio_SetRxHandler(void (*fn)(const Radio_Frame *frame))
{
    s_radio.rxHandler = fn;
}

void Radio_SetTxDoneHandler(void (*fn)(void *token, int status, int framePending))
{
    s_radio.txDoneHandler = fn;
}

void Radio_Poll(void)
{
    if (s_radio.state == MG21_RADIO_DOWN)
        return;

    if (s_radio.txPhase != MG21_TX_IDLE)
        radioPollTx();
    else if (radioState() == MG21_RAC_STATUS_STATE_RXOVERFLOW)
        radioRxRecover();
    else
        radioRxDrain();

    radioRxDeliver();
}

/* No phase here waits on a state without an interrupt: the loop's usual wake-ups serve every step. */
int Radio_PollAgain(void)
{
    return 0;
}

void Radio_GetCounters(Radio_Counters *out)
{
    if (out == 0)
        return;

    out->rxFrames = s_radio.counters.rxFrames;
    out->rxDropped = s_radio.counters.rxDropped;
    out->rxCrcBad = s_radio.counters.rxCrcBad;
    out->rxUnfiltered = s_radio.counters.rxUnfiltered;
    out->txFrames = s_radio.counters.txFrames;
    out->txFailed = s_radio.counters.txFailed;
    out->edRequests = s_radio.counters.edRequests;
    out->rxRingFull = s_radio.counters.rxRingFull;
    out->isrSpurious = 0u;
    out->ackSent = 0u;
    out->ackFp = 0u;
    out->ackLate = 0u;
    out->ackNotPllOn = 0u;
    out->rxFiltered = s_radio.counters.rxFiltered;
}

/* The contract's microsecond stamp: this board's millisecond, with no finer resolution than it,
 * which is what hal/radio.h says Radio_Now is (docs/protocol.md §7.2). */
uint32_t Radio_Now(void)
{
    uint32_t ms;

    Tick_GetMs(&ms);
    return ms * 1000u;
}

/* ---- hand-back before a reset (mg21_radio.h) ---- */

void Mg21Radio_Release(void)
{
    /* This route runs the radio from the main loop and its own sequencer program, with no interrupt of
     * its own: there is nothing to stop before the registers are. */
    Mg21Radio_Off();
    s_radio.state = MG21_RADIO_DOWN;
}
