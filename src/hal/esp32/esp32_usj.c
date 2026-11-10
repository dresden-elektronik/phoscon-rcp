/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* esp32_usj.c — the host link over the USB Serial/JTAG controller, behind hal/uart_drv.h (TRM ch. 32).

The controller is a fixed USB device: a CDC-ACM port and a JTAG bridge that enumerate in hardware, with
no descriptors or endpoints for software to set up. The CDC half is two 64-byte FIFOs behind one register:
a read of EP1 takes an octet the host sent, a write queues one for the host, and WR_DONE hands what was
written to the IN endpoint as a packet. The line rate a host sets means nothing to it, so Uart_Init's
argument is ignored, and there is no framing error to count.

Receive is the interrupt's: a packet from the host raises SERIAL_OUT_RECV_PKT, and the vector drains the
FIFO and hands the octets to the adapter as one run. Transmit is the loop's: a chunk is copied into a
holding buffer and pumped into the IN FIFO as far as it has room, the rest on the next pass. A host that
has the port closed, or no host at all, never empties the FIFO; the chunk then waits, Uart_TxSend reports
busy, and the record queue drops its oldest, so the image never blocks on the link.

The controller also resets the chip on the CDC line state the host drives (TRM Table 32.3-2), which is how
esptool reaches the ROM's download mode with this image running; this file leaves that untouched. */
#include <stdint.h>

#include "com.h"
#include "dbg.h"
#include "esp32_gpio.h"
#include "esp32_irq.h"
#include "esp32_regs.h"
#include "hal.h"
#include "u_bstream.h"
#include "uart_drv.h"

#define USJ(offset) ESP32_REG(ESP32_USJ_BASE, (offset))

#define USJ_PACKET 64u
#define USJ_PRIORITY 1u

static Uart_DataInFn usjRxHandler;
static uint8_t usjTxChunk[COM_TX_MAX];
static uint16_t usjTxAt;
static uint16_t usjTxLen;
static int usjReady;

static volatile uint16_t usjRxOctets;
static volatile uint16_t usjRxVectors;

#ifdef DS_LINK_HEARTBEAT_MS
static uint32_t usjNextBeatMs = DS_LINK_HEARTBEAT_MS;
#endif

static void usjRxIsr(void);

__attribute__((section(".iram.text.esp32_usj"))) static void usjRxIsr(void)
{
    uint8_t run[USJ_PACKET];
    uint16_t n = 0u;

    USJ(ESP32_USJ_INT_CLR) = ESP32_USJ_INT_OUT_RECV_PKT;
    usjRxVectors++;

    while (n < USJ_PACKET && (USJ(ESP32_USJ_EP1_CONF) & ESP32_USJ_EP1_CONF_OUT_EP_DATA_AVAIL) != 0u)
        run[n++] = (uint8_t)USJ(ESP32_USJ_EP1);

    if (n == 0u)
        return;

    usjRxOctets = (uint16_t)(usjRxOctets + n);

    if (usjRxHandler != 0)
        usjRxHandler(run, n);
}

int Uart_Init(uint32_t baud)
{
    (void)baud;

    USJ(ESP32_USJ_INT_ENA) = 0u;
    USJ(ESP32_USJ_INT_CLR) = 0xFFFFFFFFu;

    if (!Esp32Irq_Route(ESP32_SRC_USJ, ESP32_LINE_USJ, USJ_PRIORITY, usjRxIsr))
        return 0;

    USJ(ESP32_USJ_INT_ENA) = ESP32_USJ_INT_OUT_RECV_PKT;
    usjReady = 1;
    return 1;
}

/* The port exists for as long as the chip is powered: the controller enumerates by itself. */
int Uart_Present(void)
{
    return usjReady;
}

void Uart_SetRxHandler(Uart_DataInFn handler)
{
    usjRxHandler = handler;
}

/* Into the IN FIFO as far as it has room; the packet is handed over once the chunk is in. 1 = done. */
static int usjPump(void)
{
    while (usjTxAt < usjTxLen)
    {
        if ((USJ(ESP32_USJ_EP1_CONF) & ESP32_USJ_EP1_CONF_IN_EP_DATA_FREE) == 0u)
            return 0;

        USJ(ESP32_USJ_EP1) = usjTxChunk[usjTxAt++];
    }

    if (usjTxLen != 0u)
    {
        USJ(ESP32_USJ_EP1_CONF) = ESP32_USJ_EP1_CONF_WR_DONE;
        usjTxLen = 0u;
        usjTxAt = 0u;
    }

    return 1;
}

int Uart_TxSend(const uint8_t *bytes, uint16_t len)
{
    U_BStream bs;

    if (bytes == 0 || len == 0u || len > COM_TX_MAX || !usjReady)
        return 0;

    if (!usjPump())
        return 0;

    U_bstream_init(&bs, usjTxChunk, (unsigned long)sizeof(usjTxChunk));
    U_bstream_put_bytes(&bs, bytes, (unsigned long)len);
    usjTxAt = 0u;
    usjTxLen = len;
    (void)usjPump();
    return 1;
}

#ifdef DS_LINK_HEARTBEAT_MS
/* The live link report (DS_LINK_HEARTBEAT_MS, set by the board): whether the loop runs and whether the
 * host's octets arrive, on a board whose only window is this link. */
static void usjHeartbeat(void)
{
    uint32_t ms;

    Tick_GetMs(&ms);

    if ((int32_t)(ms - usjNextBeatMs) < 0)
        return;

    usjNextBeatMs = ms + DS_LINK_HEARTBEAT_MS;
    /* The board's LED changes with every beat: a loop that stops leaves it as it was. */
    Esp32Board_Led((int)((ms / DS_LINK_HEARTBEAT_MS) & 1u));
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)(ms / 1000u), DBG_WITNESS_RSN_ESP32_HEARTBEAT);
    Debug_Emit(DBG_COMP_WITNESS, usjRxOctets, DBG_WITNESS_RSN_ESP32_USJ_RX_OCTETS);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)USJ(ESP32_USJ_FRAM_NUM), DBG_WITNESS_RSN_ESP32_USJ_FRAME);
}
#endif

uint16_t Uart_TxPoll(void)
{
    uint16_t n;

    if (!usjReady)
        return 0u;

#ifdef DS_LINK_HEARTBEAT_MS
    usjHeartbeat();
#endif

    if (!usjPump())
        return 0u;

    n = Debug_QueueTake(usjTxChunk, (uint16_t)sizeof(usjTxChunk));

    if (n == 0u)
        return 0u;

    usjTxAt = 0u;
    usjTxLen = n;
    (void)usjPump();
    return n;
}

void Uart_Witnesses(void)
{
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)ESP32_LINE_USJ, DBG_WITNESS_RSN_ESP32_USJ_LINE);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)Esp32Irq_EnabledLines(), DBG_WITNESS_RSN_ESP32_PLIC_ENABLE);
}
