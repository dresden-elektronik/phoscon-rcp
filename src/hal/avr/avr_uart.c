/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* avr_uart.c — USART0 bring-up, the TX pipe and the two vectors of the ConBee I / RaspBee I host link.
 *
 * Glue only (docs/coding-rules.md §4): the divisor comes from avr_baud.c, which the host suite pins;
 * what is here is the order the part wants (datasheet, USART chapter) and one chunk out, one run in.
 *
 * The bootloader jumps here with USART0 still enabled at its own divisor for 8 MHz and its interrupt
 * enables cleared (docs/hardware_avr.md §11.2), so the block is switched off before it is set up
 * again: a divisor written under a running receiver is a byte received at two rates.
 *
 * The receiver reports a bad frame and an overrun in UCSR0A for the octet at the head of its buffer,
 * so the flags are read before UDR0, which pops it; an octet with FE0 is dropped and counted, and an
 * overrun counts the octets lost before this one, which is delivered. */
#include <stdint.h>

#include "avr_baud.h"
#include "avr_clock.h"
#include "avr_regs.h"
#include "avr_uart.h"
#include "com.h"
#include "dbg.h"
#include "hal.h"
#include "uart_drv.h"
#include "u_bstream.h"

typedef char uartChunkIsTheContractWrite[(UART_TX_MAX == COM_TX_MAX) ? 1 : -1];

/* PE1 is TXD0 and PE0 is RXD0 (§11.1). */
#define UART_PAD_TX 1u
#define UART_PAD_RX 0u

/* Bounded wait for the last octet to leave: at 38 400 an octet is 260 us, a few thousand polls. */
#define UART_DRAIN_LIMIT 0x4000u

/* The pipe state is one byte so the loop's read of it cannot tear against the vector's write. */
#define UART_PIPE_DOWN 0u
#define UART_PIPE_READY 1u
#define UART_PIPE_ARMED 2u

static volatile uint8_t uartTxPipe;
static uint8_t uartTxChunk[UART_TX_MAX];
static volatile uint8_t uartTxLen;
static volatile uint8_t uartTxAt;

static uint8_t uartRxRun[UART_RX_RUN_MAX];
static Uart_DataInFn uartRxHandler;
static int uartReady;
static uint16_t uartUbrr;
static uint32_t uartAchievedHz;

/* Counted in the vectors, published by Uart_Witnesses. */
static volatile uint16_t uartVectors;
static volatile uint16_t uartRxOverruns;
static volatile uint16_t uartRxFrameErrors;

/* ---- bring-up ---- */

int Uart_Init(uint32_t baud)
{
    uint16_t ubrr = 0u;
    int u2x = 0;

    if (!AvrBaud_Ubrr(Clock_CpuHz(), baud, &ubrr, &u2x))
    {
        DBG(DBG_COMP_UART, DBG_UART_RSN_BAUD_REFUSED);
        return 0;
    }

    uartUbrr = ubrr;
    uartAchievedHz = AvrBaud_RateHz(Clock_CpuHz(), ubrr, u2x);

    AVR_REG8(AVR_PRR0) = (uint8_t)(AVR_REG8(AVR_PRR0) & (uint8_t)~AVR_BIT(AVR_PRUSART0));

    /* Off first: the bootloader's receiver and transmitter are still enabled. */
    AVR_REG8(AVR_UCSR0B) = 0u;

    /* TXD0 idles high: drive PE1 high as an output before the transmitter takes the pad, so the hand
     * over is not a start bit. PE0 is an input with its pull-up, which a disconnected line needs. */
    AVR_REG8(AVR_PORTE) = (uint8_t)(AVR_REG8(AVR_PORTE) | AVR_BIT(UART_PAD_TX) | AVR_BIT(UART_PAD_RX));
    AVR_REG8(AVR_DDRE) = (uint8_t)((AVR_REG8(AVR_DDRE) | AVR_BIT(UART_PAD_TX)) &
                                   (uint8_t)~AVR_BIT(UART_PAD_RX));

    /* UBRR0H first: writing UBRR0L updates the prescaler at once (datasheet, UBRRn). */
    AVR_REG8(AVR_UBRR0H) = (uint8_t)(ubrr >> 8);
    AVR_REG8(AVR_UBRR0L) = (uint8_t)ubrr;
    AVR_REG8(AVR_UCSR0A) = u2x ? AVR_BIT(AVR_U2X0) : 0u;
    AVR_REG8(AVR_UCSR0C) = (uint8_t)(AVR_BIT(AVR_UCSZ01) | AVR_BIT(AVR_UCSZ00)); /* 8N1, async */

    /* Whatever the bootloader's receiver held is a different rate's: emptied before the vector runs. */
    while ((AVR_REG8(AVR_UCSR0A) & AVR_BIT(AVR_RXC0)) != 0u)
        (void)AVR_REG8(AVR_UDR0);

    AVR_REG8(AVR_UCSR0B) = (uint8_t)(AVR_BIT(AVR_RXCIE0) | AVR_BIT(AVR_RXEN0) | AVR_BIT(AVR_TXEN0));

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

/* UDRE0 is a level: the vector is entered while the data register has room, until it is disabled. */
static void uartArmTx(uint8_t len)
{
    uartTxAt = 0u;
    uartTxLen = len;
    uartTxPipe = UART_PIPE_ARMED;
    AVR_REG8(AVR_UCSR0B) = (uint8_t)(AVR_REG8(AVR_UCSR0B) | AVR_BIT(AVR_UDRIE0));
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
    uartArmTx((uint8_t)len);
    return 1;
}

uint16_t Uart_TxPoll(void)
{
    uint16_t n;

    if (uartTxPipe != UART_PIPE_READY)
        return 0;

    n = Debug_QueueTake(uartTxChunk, (uint16_t)sizeof(uartTxChunk));

    if (n == 0u)
        return 0;

    uartArmTx((uint8_t)n);
    return n;
}

/* ---- the vectors (slots 25 and 26: startup/startup_avr.S) ---- */

AVR_ISR(AVR_USART0_RX_vect_num)
{
    uint8_t run = 0u;

    HAL_IrqEnter();
    uartVectors++;

    while (run < UART_RX_RUN_MAX && (AVR_REG8(AVR_UCSR0A) & AVR_BIT(AVR_RXC0)) != 0u)
    {
        uint8_t status = AVR_REG8(AVR_UCSR0A);
        uint8_t octet = AVR_REG8(AVR_UDR0);

        if ((status & AVR_BIT(AVR_DOR0)) != 0u)
        {
            uartRxOverruns++;
            DBG(DBG_COMP_UART, DBG_UART_RSN_RX_LOST);
        }

        if ((status & AVR_BIT(AVR_FE0)) != 0u)
        {
            uartRxFrameErrors++;
            DBG(DBG_COMP_UART, DBG_UART_RSN_RX_FRAME_ERR);
            continue;
        }

        uartRxRun[run++] = octet;
    }

    if (run != 0u && uartRxHandler != 0)
        uartRxHandler(uartRxRun, run);

    HAL_IrqExit();
}

/* The chunk buffer is free as soon as its last octet is in UDR0: the pipe is ready again then, and the
 * line finishes the octet on its own. */
AVR_ISR(AVR_USART0_UDRE_vect_num)
{
    HAL_IrqEnter();
    uartVectors++;

    if (uartTxAt < uartTxLen)
        AVR_REG8(AVR_UDR0) = uartTxChunk[uartTxAt++];

    if (uartTxAt >= uartTxLen)
    {
        AVR_REG8(AVR_UCSR0B) = (uint8_t)(AVR_REG8(AVR_UCSR0B) & (uint8_t)~AVR_BIT(AVR_UDRIE0));
        uartTxPipe = UART_PIPE_READY;
    }

    HAL_IrqExit();
}

/* ---- hand-back ---- */

/* TXC0 is set when the last octet has left the shift register and UDR0 is empty; it is cleared by
 * writing one, so it is cleared before the drain is waited on, or a TXC0 from an earlier chunk would
 * end the wait at once. A chunk the vector had not finished is cut where it stands. */
void Uart_Release(void)
{
    uint16_t n;

    if (!uartReady)
        return;

    AVR_REG8(AVR_UCSR0B) = (uint8_t)(AVR_REG8(AVR_UCSR0B) &
                                     (uint8_t)~(AVR_BIT(AVR_UDRIE0) | AVR_BIT(AVR_RXCIE0)));

    for (n = 0u; n < UART_DRAIN_LIMIT; ++n)
    {
        if ((AVR_REG8(AVR_UCSR0A) & AVR_BIT(AVR_UDRE0)) != 0u)
            break;
    }

    AVR_REG8(AVR_UCSR0A) = (uint8_t)(AVR_REG8(AVR_UCSR0A) | AVR_BIT(AVR_TXC0));

    for (n = 0u; n < UART_DRAIN_LIMIT; ++n)
    {
        if ((AVR_REG8(AVR_UCSR0A) & AVR_BIT(AVR_TXC0)) != 0u)
            break;
    }

    AVR_REG8(AVR_UCSR0B) = 0u;
    uartReady = 0;
    uartTxPipe = UART_PIPE_DOWN;
}

/* ---- read-backs ---- */

void Uart_Witnesses(void)
{
    uint16_t vectors;
    uint16_t overruns;
    uint16_t frameErrors;
    uint8_t sreg;

    if (!uartReady)
        return;

    /* Two bytes each that the vectors write (docs/architecture.md §7). */
    sreg = AvrOwn_LockSave();
    vectors = uartVectors;
    overruns = uartRxOverruns;
    frameErrors = uartRxFrameErrors;
    AvrOwn_LockRestore(sreg);

    Debug_Emit(DBG_COMP_WITNESS, AVR_REG8(AVR_UCSR0A), DBG_WITNESS_RSN_AVR_UCSR0A);
    Debug_Emit(DBG_COMP_WITNESS, AVR_REG8(AVR_UCSR0B), DBG_WITNESS_RSN_AVR_UCSR0B);
    Debug_Emit(DBG_COMP_WITNESS, AVR_REG8(AVR_UCSR0C), DBG_WITNESS_RSN_AVR_UCSR0C);
    Debug_Emit(DBG_COMP_WITNESS, uartUbrr, DBG_WITNESS_RSN_AVR_UBRR0);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)uartAchievedHz, DBG_WITNESS_RSN_AVR_USART_RATE);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)(uartAchievedHz >> 16), DBG_WITNESS_RSN_AVR_USART_RATE_HI);
    Debug_Emit(DBG_COMP_WITNESS, vectors, DBG_WITNESS_RSN_AVR_UART_VECTORS);
    Debug_Emit(DBG_COMP_WITNESS, overruns, DBG_WITNESS_RSN_AVR_UART_RX_OF);
    Debug_Emit(DBG_COMP_WITNESS, frameErrors, DBG_WITNESS_RSN_AVR_UART_RX_FERR);
}
