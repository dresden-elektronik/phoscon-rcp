/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* samr21_uart.c — SERCOM5 USART bring-up, the TX pipe and the receive vector.
 *
 * Glue only (docs/coding-rules.md §4): register writes, one chunk out, one run in,
 * markers for what the receiver could not keep. Framing and protocol are above
 * COM_* in src/rcp/. Register citations are the datasheet's SERCOM USART chapter
 * (24) as the SAMR21 lays it out - its CTRLA bit positions are not the SAMD
 * family's, so nothing here is copied from a SAMD driver. */
#include <stdint.h>

#include "samr21_uart.h"
#include "dbg.h"
#include "hal.h"
#include "hal_diag.h"
#include "samr21_regs.h"
#include "uart_baud.h"
#include "com.h"
#include "u_bstream.h"

/* The adapter sizes its rings around COM_TX_MAX and hands one write at a time; this
 * driver's chunk is that same number, and a board that changes one has to change both. */
typedef char uartChunkIsTheContractWrite[(UART_TX_MAX == COM_TX_MAX) ? 1 : -1];

/* 24.8.1: MODE 0x1 = internal clock, SAMPR 0 = 16x oversampling with arithmetic
 * baud, FORM 0 = the plain USART frame (8 data bits, no parity), DORD 1 = LSB
 * first - the convention the host's UART samples by. Pad routing is the board's:
 * TXPO 1 puts TxD on PAD2 (PA24), RXPO 3 puts RxD on PAD3 (PA25). CHSIZE 0 and
 * SBMODE 0 in CTRLB are the reset values, written as such. */
#define UART_CTRLA                                                                                     \
    (SERCOM_UART_CTRLA_MODE_INT_CLK | SERCOM_UART_CTRLA_SAMPR_16X | SERCOM_UART_CTRLA_FORM_USART |  \
     SERCOM_UART_CTRLA_DORD_LSB | SERCOM_UART_CTRLA_TXPO(1u) | SERCOM_UART_CTRLA_RXPO(3u))

#define UART_CTRLB (SERCOM_UART_CTRLB_TXEN | SERCOM_UART_CTRLB_RXEN)

/* The core clock the SERCOM is clocked from: generator 0, which is the locked DFLL
 * - or its USBCRM failover - whenever HAL_Init did not return 0. */
#define UART_GCLK_GEN 0u
#define UART_REF_HZ 48000000u

/* Same budget as the SPI access layer: about 80 ms at 8 MHz, far above the
 * microsecond-scale sync delays and far below the ~8 s watchdog. */
#define UART_SYNC_WAIT_LIMIT 0x10000u

/* Level 0, above the radio vector (1): at 115 200 the receiver's two octets of buffer last about
 * 260 us, and the radio vector can run longer, so it would cost an octet of every frame the host
 * sends while the radio receives. This vector takes microseconds (docs/architecture.md §7). */
#define UART_IRQ_PRIORITY 0u

/* One armed chunk out: DOWN = the link never came up, READY = the chunk buffer is
 * free, ARMED = octets are on the wire. Only the vector takes ARMED out. */
typedef enum
{
    UART_PIPE_DOWN = 0,
    UART_PIPE_READY,
    UART_PIPE_ARMED
} Uart_PipeState;

static volatile Uart_PipeState uartTxPipe;
static uint8_t uartTxChunk[UART_TX_MAX];
static volatile uint16_t uartTxLen;
static volatile uint16_t uartTxAt;

/* Written and read inside the vector only. */
static uint8_t uartRxRun[UART_RX_RUN_MAX];

static Uart_DataInFn uartRxHandler;
static int uartReady;
static volatile uint16_t uartBaudValue;

/* ---- bring-up ---- */

/* A GCLK write while STATUS.SYNCBUSY is set is the errata-10416 freeze, so a failed
 * wait aborts before any further GCLK write (docs/hardware_samr21.md §2). */
static int uartGclkIdle(uint16_t rsn)
{
    uint32_t n;

    for (n = 0; n < UART_SYNC_WAIT_LIMIT; ++n)
    {
        if ((SAMR21_GCLK->STATUS & GCLK_STATUS_SYNCBUSY) == 0u)
            return 1;

        Clock_BringupPump();
        Wdt_FeedPoll(n);
    }

    DBG(DBG_COMP_UART, rsn);
    return 0;
}

static int uartSercomSyncClear(uint32_t mask, uint16_t rsn)
{
    uint32_t n;

    for (n = 0; n < UART_SYNC_WAIT_LIMIT; ++n)
    {
        if ((SAMR21_SERCOM5->SYNCBUSY & mask) == 0u)
            return 1;

        Clock_BringupPump();
        Wdt_FeedPoll(n);
    }

    DBG(DBG_COMP_UART, rsn);
    return 0;
}

static int uartSercomSwrstClear(void)
{
    uint32_t n;

    for (n = 0; n < UART_SYNC_WAIT_LIMIT; ++n)
    {
        if ((SAMR21_SERCOM5->SYNCBUSY & SERCOM_UART_SYNCBUSY_SWRST) == 0u &&
            (SAMR21_SERCOM5->CTRLA & SERCOM_UART_CTRLA_SWRST) == 0u)
            return 1;

        Clock_BringupPump();
        Wdt_FeedPoll(n);
    }

    DBG(DBG_COMP_UART, DBG_UART_RSN_SWRST);
    return 0;
}

/* PA24 = bit 8, PA25 = bit 9 of the half-word selected by HWSEL. Two writes per pin,
 * deliberately: errata 12368 says the pulls on these two pins are not dropped by the mux
 * switch, so WRPINCFG clears them first (PULLEN written 0, and with it the pull direction's
 * effect), and only then does WRPMUX/PMUXEN hand the pin to SERCOM5. Only the RxD pad takes
 * INEN. */
static void uartPinInit(void)
{
    Samr21PortGroup *g0 = SAMR21_PORT_G(0);
    uint32_t selTx = PORT_WRCONFIG_HWSEL | PORT_WRCONFIG_PINMASK(PORT_PIN(24) >> 16);
    uint32_t selRx = PORT_WRCONFIG_HWSEL | PORT_WRCONFIG_PINMASK(PORT_PIN(25) >> 16);

    g0->DIRSET = PORT_PIN(24);
    g0->WRCONFIG = selTx | PORT_WRCONFIG_WRPINCFG;
    g0->WRCONFIG = selTx | PORT_WRCONFIG_WRPINCFG | PORT_WRCONFIG_WRPMUX |
                   PORT_WRCONFIG_PMUX(PORT_MUX_PA24D_SERCOM5_PAD2) | PORT_WRCONFIG_PMUXEN;

    g0->DIRCLR = PORT_PIN(25);
    g0->WRCONFIG = selRx | PORT_WRCONFIG_WRPINCFG;
    g0->WRCONFIG = selRx | PORT_WRCONFIG_WRPINCFG | PORT_WRCONFIG_WRPMUX |
                   PORT_WRCONFIG_PMUX(PORT_MUX_PA25D_SERCOM5_PAD3) | PORT_WRCONFIG_INEN |
                   PORT_WRCONFIG_PMUXEN;
}

int Uart_Init(uint32_t baud)
{
    uint16_t baudReg;

    if (!UartBaud_Register(UART_REF_HZ, baud, &baudReg))
    {
        DBG(DBG_COMP_UART, DBG_UART_RSN_BAUD_REFUSED);
        return 0;
    }

    uartBaudValue = baudReg;

    SAMR21_PM->APBCMASK |= PM_APBCMASK_SERCOM5;
    uartPinInit();

    if (!uartGclkIdle(DBG_UART_RSN_GCLK_SYNC))
        return 0;

    SAMR21_GCLK->CLKCTRL = GCLK_CLKCTRL_ID(GCLK_CLKCTRL_ID_SERCOM5_CORE) |
                           GCLK_CLKCTRL_GEN(UART_GCLK_GEN) | GCLK_CLKCTRL_CLKEN;

    SAMR21_SERCOM5->CTRLA = SERCOM_UART_CTRLA_SWRST;

    if (!uartSercomSwrstClear())
        return 0;

    /* BAUD is enable-protected (24.8.3): written while the SERCOM is disabled. */
    SAMR21_SERCOM5->BAUD = uartBaudValue;

    SAMR21_SERCOM5->CTRLB = UART_CTRLB;

    if (!uartSercomSyncClear(SERCOM_UART_SYNCBUSY_CTRLB, DBG_UART_RSN_CTRLB))
        return 0;

    SAMR21_SERCOM5->CTRLA = UART_CTRLA | SERCOM_UART_CTRLA_ENABLE;

    if (!uartSercomSyncClear(SERCOM_UART_SYNCBUSY_ENABLE, DBG_UART_RSN_ENABLE))
        return 0;

    /* Reception is always armed; transmission arms its two interrupts per chunk. */
    SAMR21_SERCOM5->INTENSET = SERCOM_UART_INT_RXC;

    NVIC_SET_PRIO(IRQN_SERCOM5, UART_IRQ_PRIORITY);
    NVIC_ICPR0 = 1uL << IRQN_SERCOM5;
    NVIC_ISER0 = 1uL << IRQN_SERCOM5;

    uartTxPipe = UART_PIPE_READY;
    uartReady = 1;
    DBG(DBG_COMP_UART, DBG_UART_RSN_INIT_DONE);
    return 1;
}

int Uart_Present(void)
{
    return uartReady;
}

void Uart_SetRxHandler(Uart_DataInFn fn)
{
    uartRxHandler = fn;
}

/* ---- out: one chunk in flight ---- */

/* Clear both flags before arming: a stale TXC from the previous chunk would retire
 * this one before its first octet has gone. */
static void uartArmTx(void)
{
    SAMR21_SERCOM5->INTFLAG = SERCOM_UART_INT_DRE | SERCOM_UART_INT_TXC;
    SAMR21_SERCOM5->INTENSET = SERCOM_UART_INT_DRE | SERCOM_UART_INT_TXC;
}

int Uart_TxSend(const uint8_t *buf, uint16_t len)
{
    U_BStream bs;

    if (buf == 0 || len == 0u || len > UART_TX_MAX)
        return 0;

    if (uartTxPipe != UART_PIPE_READY)
        return 0;

    U_bstream_init(&bs, uartTxChunk, (unsigned long)sizeof(uartTxChunk));
    U_bstream_put_bytes(&bs, buf, (unsigned long)len);

    uartTxAt = 0u;
    uartTxLen = len;
    uartTxPipe = UART_PIPE_ARMED;
    uartArmTx();
    return 1;
}

uint16_t Uart_TxPoll(void)
{
    uint16_t n;

    if (uartTxPipe != UART_PIPE_READY)
        return 0;

    /* Whole 8-byte records back to back (dbg.h); the chunk buffer is the loop's while
     * the pipe is READY, so this is the only writer of it here. */
    n = Debug_QueueTake(uartTxChunk, (uint16_t)sizeof(uartTxChunk));

    if (n == 0u)
        return 0;

    uartTxAt = 0u;
    uartTxLen = n;
    uartTxPipe = UART_PIPE_ARMED;
    uartArmTx();
    return n;
}

/* ---- the vector (IRQ 14: startup/startup_samr21.S) ---- */

/* STATUS is read before DATA: each error bit reports the status of the character the
 * DATA read takes out (24.8.9). A flagged octet is not delivered - it is the one the
 * receiver could not keep - and the bit is cleared by writing a one back. */
static void uartDrainRx(void)
{
    uint16_t run = 0u;

    while (run < UART_RX_RUN_MAX && (SAMR21_SERCOM5->INTFLAG & SERCOM_UART_INT_RXC))
    {
        uint16_t status = SAMR21_SERCOM5->STATUS;
        uint8_t octet = (uint8_t)SAMR21_SERCOM5->DATA;

        if (status & SERCOM_UART_STATUS_BUFOVF)
        {
            SAMR21_SERCOM5->STATUS = SERCOM_UART_STATUS_BUFOVF;
            DBG(DBG_COMP_UART, DBG_UART_RSN_RX_LOST);
            continue;
        }

        if (status & SERCOM_UART_STATUS_FERR)
        {
            SAMR21_SERCOM5->STATUS = SERCOM_UART_STATUS_FERR;
            DBG(DBG_COMP_UART, DBG_UART_RSN_RX_FRAME_ERR);
            continue;
        }

        uartRxRun[run++] = octet;
    }

    if (run != 0u && uartRxHandler)
        uartRxHandler(uartRxRun, run);
}

void SERCOM5_Handler(void)
{
    uint8_t flags = SAMR21_SERCOM5->INTFLAG;

    HAL_IrqEnter();

    if (flags & SERCOM_UART_INT_RXC)
        uartDrainRx();

    /* A late vector can find the line already run dry with octets still to send, and then TXC is set
     * beside DRE. TXC retires the chunk only once every octet has gone, or the rest of it is lost. */
    if ((flags & SERCOM_UART_INT_DRE) && uartTxAt < uartTxLen)
    {
        SAMR21_SERCOM5->DATA = uartTxChunk[uartTxAt++];
        SAMR21_SERCOM5->INTFLAG = SERCOM_UART_INT_TXC;

        /* The last octet has left DATA: TXC, already enabled, retires the chunk. */
        if (uartTxAt >= uartTxLen)
            SAMR21_SERCOM5->INTENCLR = SERCOM_UART_INT_DRE;
    }
    else if (flags & SERCOM_UART_INT_TXC)
    {
        SAMR21_SERCOM5->INTFLAG = SERCOM_UART_INT_TXC;

        if (uartTxPipe == UART_PIPE_ARMED && uartTxAt >= uartTxLen)
        {
            SAMR21_SERCOM5->INTENCLR = SERCOM_UART_INT_TXC | SERCOM_UART_INT_DRE;
            uartTxAt = 0u;
            uartTxLen = 0u;
            uartTxPipe = UART_PIPE_READY;
        }
    }

    HAL_IrqExit();
}

/* ---- read-backs ---- */

void Uart_Witnesses(void)
{
    if (!uartReady)
        return;

    /* CTRLA needs two records: the pad routing sits at 23:16, above the 16-bit line
     * field (dbg.h). */
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)SAMR21_SERCOM5->CTRLA, DBG_WITNESS_RSN_UART_CTRLA);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)(SAMR21_SERCOM5->CTRLA >> 16),
               DBG_WITNESS_RSN_UART_CTRLA_PADS);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)SAMR21_SERCOM5->CTRLB, DBG_WITNESS_RSN_UART_CTRLB);
    /* RXEN 17 and TXEN 16 sit above the 16-bit line field: the half that holds them travels
     * beside it, or a working link witnesses as 0 (measured on the RaspBee II). */
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)(SAMR21_SERCOM5->CTRLB >> 16),
               DBG_WITNESS_RSN_UART_CTRLB_PADS);
    Debug_Emit(DBG_COMP_WITNESS, uartBaudValue, DBG_WITNESS_RSN_UART_BAUD);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)SAMR21_SERCOM5->INTENSET, DBG_WITNESS_RSN_UART_INTENSET);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)SAMR21_SERCOM5->STATUS, DBG_WITNESS_RSN_UART_STATUS);
}
