/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mg21_uart.c — USART0 bring-up, the TX pipe and the two vectors of the ConBee III host link.

Glue only (docs/coding-rules.md §4): the divisor comes from usart_baud.c, which the host suite pins;
what is here is the order the part wants (reference manual 20.4/20.5 for the USART, 24.5/24.6 for the
pad routing in GPIO, 8.4 for the clock the block runs on) and one chunk out, one run in.

Three things are unlike the SERCOM side and are the reason this driver is a separate file:

  - The pads are the GPIO block's to route. USART0 has no pad selector of its own; GPIO names the
    port and pin in USARTROUTE[0] and switches them over with ROUTEEN.
  - The clock is not the block's to ask for. The USART runs on the peripheral bus, whose rate the
    clock module reports (mg21_clock.h), and a driver that reaches for a clock-enable bit here finds
    no such register.
  - A received octet carries its own error marks in RXDATAX, so a framing error is attributed to the
    byte it corrupted instead of to whoever reads the status register first. */
#include <stdint.h>

#include "com.h"
#include "dbg.h"
#include "hal.h"
#include "mg21_clock.h"
#include "mg21_regs.h"
#include "mg21_uart.h"
#include "mg21_wdt.h"
#include "uart_drv.h"
#include "u_bstream.h"
#include "usart_baud.h"

typedef char uartChunkIsTheContractWrite[(UART_TX_MAX == COM_TX_MAX) ? 1 : -1];

/* The board's pair, as the DeviceConfig CommConfig records it: port B, RxD on pin 0 and TxD on pin 1
 * (docs/hardware_mg21.md §10.8). */
#define UART_PAD_PORT 1u
#define UART_PAD_TX_PIN 1u
#define UART_PAD_RX_PIN 0u
#define UART_ROUTE_INDEX 0u

/* The line rate this link is specified at, and the oversampling the divisor arithmetic is done for.
 * Both are board facts (docs/hardware_mg21.md §10.4); the board descriptor carries the rate and the
 * driver carries how it is sampled. */
#define UART_REF_OVS USART_BAUD_OVS_16

/* Level 2 of four priority bits, the same relative place the SAMR21 gives the link: below the radio
 * vector that does not exist yet, above nothing in particular. */
#define UART_IRQ_PRIORITY 2u

/* Bounded waits on this block's enable status and on its FIFO draining. */
#define UART_ENABLE_WAIT_LIMIT 0x10000u

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

static uint8_t uartRxRun[UART_RX_RUN_MAX];
static Uart_DataInFn uartRxHandler;
static int uartReady;
static uint32_t uartClkDiv;
static uint32_t uartAchievedHz;

/* Counted in the vector, published by Uart_Witnesses: what the receiver could not keep, and how many
 * times it was asked. */
static volatile uint16_t uartVectors;
static volatile uint16_t uartRxVectors;
static volatile uint16_t uartRxOctets;
static volatile uint32_t uartRxLast;
static volatile uint16_t uartRxOverruns;
static volatile uint16_t uartRxFrameErrors;

static uint32_t uartNvicAddress(uint32_t base, uint32_t irq)
{
    return base + 4u * MG21_NVIC_GROUP(irq);
}

static void uartIrqEnable(uint32_t irq)
{
    *(volatile uint8_t *)(MG21_NVIC_IPRI_B0 + irq) = MG21_NVIC_PRIO(UART_IRQ_PRIORITY);
    *(volatile uint32_t *)uartNvicAddress(MG21_NVIC_ICPR0, irq) = MG21_NVIC_BIT(irq);
    *(volatile uint32_t *)uartNvicAddress(MG21_NVIC_ISER0, irq) = MG21_NVIC_BIT(irq);
}

/* ---- bring-up ---- */

/* PB1 push-pull as the transmitter's pad, PB0 input with its pull-up asserted (DOUT selects the
 * pull direction only in the input-with-pull mode, which is why plain input is not enough for a line
 * that idles high: docs/hardware_mg21.md §10.1). Both pins are in the low half of port B's MODE
 * register, which covers pins 0..7. DOUT is set for both before the mode changes: until ROUTEEN hands
 * PB1 to the USART the pad drives DOUT, and a low there is a start bit the host's bridge would see. */
static void uartPadModes(void)
{
    volatile Mg21GpioPort *port = MG21_GPIO_PORT(UART_PAD_PORT);
    uint32_t model;

    port->DOUT |= MG21_GPIO_PIN_BIT(UART_PAD_TX_PIN) | MG21_GPIO_PIN_BIT(UART_PAD_RX_PIN);

    model = port->MODEL;
    model &= ~(MG21_GPIO_MODE_MASK(UART_PAD_TX_PIN) | MG21_GPIO_MODE_MASK(UART_PAD_RX_PIN));
    model |= MG21_GPIO_MODE_PUSHPULL << MG21_GPIO_MODE_SHIFT(UART_PAD_TX_PIN);
    model |= MG21_GPIO_MODE_INPUTPULL << MG21_GPIO_MODE_SHIFT(UART_PAD_RX_PIN);
    port->MODEL = model;
}

/* GPIO switches the pads over to the peripheral and names which pin is which; the route word is a
 * port number in [1:0] and a pin number in [19:16] (24.5). */
static void uartPadRoute(void)
{
    volatile Mg21UsartRoute *route = MG21_USART_ROUTE(UART_ROUTE_INDEX);

    route->RXROUTE = MG21_GPIO_USARTROUTE(UART_PAD_PORT, UART_PAD_RX_PIN);
    route->TXROUTE = MG21_GPIO_USARTROUTE(UART_PAD_PORT, UART_PAD_TX_PIN);
    route->ROUTEEN = MG21_GPIO_USART_ROUTEEN_RXPEN | MG21_GPIO_USART_ROUTEEN_TXPEN;
}

static int uartWaitStatus(uint32_t mask, int want, uint16_t rsn)
{
    uint32_t n;

    for (n = 0; n < UART_ENABLE_WAIT_LIMIT; ++n)
    {
        if ((want != 0) ? (MG21_USART0->STATUS & mask) == mask
                       : (MG21_USART0->STATUS & mask) == 0u)
            return 1;

        Clock_BringupPump();
        Wdt_FeedPoll(n);
    }

    DBG(DBG_COMP_UART, rsn);
    return 0;
}

int Uart_Init(uint32_t baud)
{
    uint32_t refHz;

    refHz = Clock_LinkHz();

    if (refHz == 0u)
    {
        DBG(DBG_COMP_UART, DBG_UART_RSN_MG21_NO_LINK_CLOCK);
        return 0;
    }

    if (!UsartBaud_ClkDiv(refHz, baud, UART_REF_OVS, &uartClkDiv))
    {
        DBG(DBG_COMP_UART, DBG_UART_RSN_BAUD_REFUSED);
        return 0;
    }

    uartAchievedHz = UsartBaud_RateHz(refHz, UART_REF_OVS, uartClkDiv);

    uartPadModes();
    uartPadRoute();

    MG21_USART0->EN = MG21_USART_EN_EN;
    MG21_USART0->FRAME = MG21_USART_FRAME_DATABITS_EIGHT | MG21_USART_FRAME_PARITY_NONE
                         | MG21_USART_FRAME_STOPBITS_ONE;
    MG21_USART0->CTRL = MG21_USART_CTRL_OVS_X16;
    MG21_USART0->CLKDIV = uartClkDiv;

    MG21_USART0->CMD = MG21_USART_CMD_CLEARRX | MG21_USART_CMD_CLEARTX;

    if (!uartWaitStatus(MG21_USART_STATUS_RXENS | MG21_USART_STATUS_TXENS, 0,
                        DBG_UART_RSN_MG21_RX_CLEAR))
        return 0;

    MG21_USART0->CMD = MG21_USART_CMD_RXEN | MG21_USART_CMD_TXEN;

    if (!uartWaitStatus(MG21_USART_STATUS_RXENS | MG21_USART_STATUS_TXENS, 1,
                        DBG_UART_RSN_MG21_ENABLE))
        return 0;

    /* Only the sticky flags can be cleared; TXBL and RXDATAV follow the buffers (mg21_regs.h). */
    MG21_USART0_IF_CLR = MG21_USART_IF_RXOF | MG21_USART_IF_FERR | MG21_USART_IF_TXC;
    MG21_USART0->IEN = MG21_USART_IEN_RXDATAV | MG21_USART_IEN_RXOF | MG21_USART_IEN_FERR;

    uartTxPipe = UART_PIPE_READY;
    uartReady = 1;

    uartIrqEnable(MG21_IRQ_USART0_RX);
    uartIrqEnable(MG21_IRQ_USART0_TX);

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

/* Only TXBL is armed: it is a level flag, so the vector is entered while the buffer has room. TXC is
 * sticky and is set by every idle line, including the one the previous chunk left, so it is cleared
 * and enabled only once the last octet of this chunk is in the buffer (USART0_TX_Handler). */
static void uartArmTx(void)
{
    MG21_USART0->IEN |= MG21_USART_IEN_TXBL;
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

#ifdef DS_LINK_HEARTBEAT_MS
/* The live link report (DS_LINK_HEARTBEAT_MS, set by the board): the boot witnesses say how the link
 * was configured, this says what it is doing - whether the receive vector runs, whether octets reach
 * the link, and what the RX pad reads - on a board with no debugger, where a link that transmits and
 * never receives is otherwise indistinguishable from one that receives nothing. */
static uint32_t uartNextBeatMs = DS_LINK_HEARTBEAT_MS;

static void uartHeartbeat(void)
{
    uint32_t ms;
    uint32_t x;

    Tick_GetMs(&ms);

    if ((int32_t)(ms - uartNextBeatMs) < 0)
        return;

    uartNextBeatMs = ms + DS_LINK_HEARTBEAT_MS;
    x = uartRxLast;

    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)(ms / 1000u), DBG_WITNESS_RSN_MG21_HEARTBEAT);
    Debug_Emit(DBG_COMP_WITNESS, uartRxVectors, DBG_WITNESS_RSN_MG21_UART_RX_VECTORS);
    Debug_Emit(DBG_COMP_WITNESS, uartRxOctets, DBG_WITNESS_RSN_MG21_UART_RX_OCTETS);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)x, DBG_WITNESS_RSN_MG21_UART_RX_LAST);
    Debug_Emit(DBG_COMP_WITNESS, uartRxOverruns, DBG_WITNESS_RSN_MG21_UART_RX_OF);
    Debug_Emit(DBG_COMP_WITNESS, uartRxFrameErrors, DBG_WITNESS_RSN_MG21_UART_RX_FERR);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)MG21_USART0->IF, DBG_WITNESS_RSN_MG21_USART_IF);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)MG21_USART0->STATUS, DBG_WITNESS_RSN_MG21_USART_STATUS);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)MG21_GPIO_PORT(UART_PAD_PORT)->DIN,
               DBG_WITNESS_RSN_MG21_GPIO_DIN);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)*(volatile uint32_t *)MG21_NVIC_ISER0,
               DBG_WITNESS_RSN_MG21_NVIC_ISER0);
}
#endif

uint16_t Uart_TxPoll(void)
{
    uint16_t n;

#ifdef DS_LINK_HEARTBEAT_MS
    if (uartReady)
        uartHeartbeat();
#endif

    if (uartTxPipe != UART_PIPE_READY)
        return 0;

    n = Debug_QueueTake(uartTxChunk, (uint16_t)sizeof(uartTxChunk));

    if (n == 0u)
        return 0;

    uartTxAt = 0u;
    uartTxLen = n;
    uartTxPipe = UART_PIPE_ARMED;
    uartArmTx();
    return n;
}

/* ---- the vectors (IRQ 11 and 12: startup/startup_mg21.S) ---- */

/* One read of RXDATAX takes the octet and whatever the receiver said about it, so a bad frame is
 * attributed to the byte it corrupted (20.5). An octet the receiver could not keep is not delivered;
 * the marker is counted the way the CDC link counts a discarded record. */
static void uartDrainRx(void)
{
    uint16_t run = 0u;

    while (run < UART_RX_RUN_MAX && ((MG21_USART0->IF & MG21_USART_IF_RXDATAV) != 0u))
    {
        uint32_t x = MG21_USART0->RXDATAX;

        uartRxLast = x;

        if ((x & MG21_USART_RXDATAX_FERR) != 0u)
        {
            MG21_USART0_IF_CLR = MG21_USART_IF_FERR;
            uartRxFrameErrors++;
            DBG(DBG_COMP_UART, DBG_UART_RSN_RX_FRAME_ERR);
            continue;
        }

        uartRxRun[run++] = (uint8_t)(x & MG21_USART_DATA_MASK);
    }

    /* FERR is sticky and enabled: once the buffer is empty, whatever byte raised it has been read and
     * counted, and a flag left set would hold this vector pending for ever. */
    if ((MG21_USART0->IF & (MG21_USART_IF_FERR | MG21_USART_IF_RXDATAV)) == MG21_USART_IF_FERR)
        MG21_USART0_IF_CLR = MG21_USART_IF_FERR;

    if ((MG21_USART0->IF & MG21_USART_IF_RXOF) != 0u)
    {
        MG21_USART0_IF_CLR = MG21_USART_IF_RXOF;
        uartRxOverruns++;
        DBG(DBG_COMP_UART, DBG_UART_RSN_RX_LOST);
    }

    uartRxOctets = (uint16_t)(uartRxOctets + run);

    if (run != 0u && uartRxHandler != 0)
        uartRxHandler(uartRxRun, run);
}

void USART0_RX_Handler(void)
{
    uint32_t flags = MG21_USART0->IF;

    HAL_IrqEnter();
    uartVectors++;
    uartRxVectors++;

    if ((flags & (MG21_USART_IF_RXDATAV | MG21_USART_IF_RXOF | MG21_USART_IF_FERR)) != 0u)
        uartDrainRx();

    HAL_IrqExit();
}

/* Two phases per chunk, told apart by which interrupt is enabled: TXBL feeds octets while the buffer
 * has room, and after the last one TXC waits for the line to go idle. The flag word alone cannot tell
 * them apart, because TXBL stays set whenever the buffer is empty and TXC stays set until cleared. */
void USART0_TX_Handler(void)
{
    uint32_t flags = MG21_USART0->IF;
    uint32_t ien = MG21_USART0->IEN;

    HAL_IrqEnter();
    uartVectors++;

    if ((ien & MG21_USART_IEN_TXBL) != 0u && (flags & MG21_USART_IF_TXBL) != 0u)
    {
        if (uartTxAt < uartTxLen)
            MG21_USART0->TXDATA = uartTxChunk[uartTxAt++];

        if (uartTxAt >= uartTxLen)
        {
            /* The last octet is in the buffer, so the line cannot be idle until it has gone: a TXC
             * set before this point is stale, and the next one is this chunk's. */
            MG21_USART0->IEN &= ~MG21_USART_IEN_TXBL;
            MG21_USART0_IF_CLR = MG21_USART_IF_TXC;
            MG21_USART0->IEN |= MG21_USART_IEN_TXC;
        }
    }
    else if ((ien & MG21_USART_IEN_TXC) != 0u && (flags & MG21_USART_IF_TXC) != 0u)
    {
        MG21_USART0_IF_CLR = MG21_USART_IF_TXC;
        MG21_USART0->IEN &= ~MG21_USART_IEN_TXC;

        uartTxAt = 0u;
        uartTxLen = 0u;
        uartTxPipe = UART_PIPE_READY;
    }

    HAL_IrqExit();
}

/* ---- hand-back ---- */

void Uart_Release(void)
{
    volatile Mg21GpioPort *port = MG21_GPIO_PORT(UART_PAD_PORT);
    volatile Mg21UsartRoute *route = MG21_USART_ROUTE(UART_ROUTE_INDEX);

    *(volatile uint32_t *)uartNvicAddress(MG21_NVIC_ICER0, MG21_IRQ_USART0_RX) =
        MG21_NVIC_BIT(MG21_IRQ_USART0_RX);
    *(volatile uint32_t *)uartNvicAddress(MG21_NVIC_ICER0, MG21_IRQ_USART0_TX) =
        MG21_NVIC_BIT(MG21_IRQ_USART0_TX);

    MG21_USART0->IEN = 0u;
    MG21_USART0->EN = 0u;

    route->ROUTEEN = 0u;
    route->RXROUTE = 0u;
    route->TXROUTE = 0u;

    port->MODEL &= ~(MG21_GPIO_MODE_MASK(UART_PAD_TX_PIN) | MG21_GPIO_MODE_MASK(UART_PAD_RX_PIN));
    port->DOUT &= ~(MG21_GPIO_PIN_BIT(UART_PAD_TX_PIN) | MG21_GPIO_PIN_BIT(UART_PAD_RX_PIN));

    uartReady = 0;
    uartTxPipe = UART_PIPE_DOWN;
}

/* ---- read-backs ---- */

static void uartEmitPair(uint32_t v, uint16_t rsnLow, uint16_t rsnHigh)
{
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)v, rsnLow);

    if (rsnHigh != 0u)
        Debug_Emit(DBG_COMP_WITNESS, (uint16_t)(v >> 16), rsnHigh);
}

void Uart_Witnesses(void)
{
    volatile Mg21UsartRoute *route;

    if (!uartReady)
        return;

    route = MG21_USART_ROUTE(UART_ROUTE_INDEX);

    uartEmitPair(MG21_USART0->EN, DBG_WITNESS_RSN_MG21_USART_EN, 0u);
    uartEmitPair(MG21_USART0->CTRL, DBG_WITNESS_RSN_MG21_USART_CTRL, 0u);
    uartEmitPair(MG21_USART0->FRAME, DBG_WITNESS_RSN_MG21_USART_FRAME, 0u);
    uartEmitPair(MG21_USART0->CLKDIV, DBG_WITNESS_RSN_MG21_USART_CLKDIV,
                 DBG_WITNESS_RSN_MG21_USART_CLKDIV_HI);
    uartEmitPair(MG21_USART0->STATUS, DBG_WITNESS_RSN_MG21_USART_STATUS, 0u);
    uartEmitPair(MG21_USART0->IEN, DBG_WITNESS_RSN_MG21_USART_IEN, 0u);
    uartEmitPair(route->ROUTEEN, DBG_WITNESS_RSN_MG21_GPIO_ROUTEEN, 0u);
    uartEmitPair(route->TXROUTE, DBG_WITNESS_RSN_MG21_GPIO_TXROUTE,
                 DBG_WITNESS_RSN_MG21_GPIO_TXROUTE_HI);
    uartEmitPair(route->RXROUTE, DBG_WITNESS_RSN_MG21_GPIO_RXROUTE,
                 DBG_WITNESS_RSN_MG21_GPIO_RXROUTE_HI);
    uartEmitPair(MG21_GPIO_PORT(UART_PAD_PORT)->MODEL, DBG_WITNESS_RSN_MG21_GPIO_MODEL, 0u);
    uartEmitPair(uartAchievedHz, DBG_WITNESS_RSN_MG21_USART_RATE, DBG_WITNESS_RSN_MG21_USART_RATE_HI);

    Debug_Emit(DBG_COMP_WITNESS, uartVectors, DBG_WITNESS_RSN_MG21_UART_VECTORS);
    Debug_Emit(DBG_COMP_WITNESS, uartRxOverruns, DBG_WITNESS_RSN_MG21_UART_RX_OF);
    Debug_Emit(DBG_COMP_WITNESS, uartRxFrameErrors, DBG_WITNESS_RSN_MG21_UART_RX_FERR);
}
