/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* ConBee II USB device glue: the app-facing Usbc_* API over the adopted usbd
 * driver. The driver keeps its own usbd_/hal_ lower_snake naming; everything
 * here is ours (provenance: usb/usbd.h).
 */
#include <stdint.h>

#include "hal.h"
#include "hal_diag.h"
#include "usbc.h"
#include "dbg.h"
#include "usbd.h"
#include "usbd_int.h"
#include "usbd_cdc.h"
#include "usb_desc.h"

/* EP map (usb_desc.c reports it, samr21_usbd_hal.c arms it): EP0 control,
 * EP1 bulk IN (records + app TX), EP2 bulk OUT, EP3 interrupt IN. */
#define CDC_DATA_IN_EP 1u
#define CDC_DATA_OUT_EP 2u
#define CDC_NOTIF_EP 3u

/* Scratchpad stage (samr21_nvram.c): USB-internal band 0x20 = module up,
 * attached, endpoint topology armed. */
#define USBC_STG_EPS_DONE 0x20u

/* ------------------------------------------------------------------ */
/* Driver configuration                                                */
/* ------------------------------------------------------------------ */

/* Static, one full-speed packet each, as the descriptor table promises the host;
 * no allocation in this tree (coding-rules §7). */
static uint8_t ep0Buf[64];     /* SETUP + EP0 data-out stage */
static uint8_t dataInBuf[64];  /* EP1 IN bank target */
static uint8_t dataOutBuf[64]; /* EP2 OUT bank target */
static uint8_t notifInBuf[64]; /* EP3 IN bank target */
static uint8_t dbgDrain[64];   /* record queue drain */
/* One CDC IN packet; the record drain and the RCP transport are both bounded by
 * it (one packet per IN completion). */
#define USB_CDC_IN_MAX ((uint16_t)USBC_TX_MAX)

typedef char usbDeviceBuffersAreOnePacket
    [(sizeof(ep0Buf) == sizeof(dataInBuf)) && (sizeof(dataInBuf) == USBD_BULK_MPS) ? 1 : -1];
typedef char usbCdcInMaxIsOnePacket[(USBC_TX_MAX == sizeof(dbgDrain)) ? 1 : -1];

static void usbDeviceBulkInDone(uint8_t ep, uint16_t len, void *arg);
static void usbDeviceBulkOutDone(uint8_t ep, uint16_t len, void *arg);
static void usbDeviceOnConfig(uint8_t cfgValue, void *arg);
static int usbDeviceClassRequest(const uint8_t *s8, void *arg);

static const usbd_cfg_t usbDeviceCfg = {.ep0_rx = ep0Buf,
                                        .ep1_rx = 0, /* EP1 has no OUT direction on this device */
                                        .ep2_rx = dataOutBuf,
                                        .ep1_tx = dataInBuf,
                                        .ep2_tx = 0, /* EP2 has no IN direction on this device */
                                        .ep3_tx = notifInBuf,
                                        .ep3_rx = 0,
                                        .serial_u16 = 0, /* see UsbDesc_Serial in usb/usb_desc.c */
                                        .serial_chars = 0,
                                        .class_request = usbDeviceClassRequest,
                                        .class_arg = 0,
                                        .on_config = usbDeviceOnConfig,
                                        .on_config_arg = 0};

/* Installed by the app (hal.h); written from the main loop, read from the USB
 * vector. */
static Usbc_DataOutFn dataOutHandler;

/* ------------------------------------------------------------------ */
/* CDC pipes                                                           */
/* ------------------------------------------------------------------ */

/* One state machine per IN pipe: DOWN = no configuration (nothing armable),
 * READY = bank empty, ARMED = a packet outstanding; only the vector or a link
 * event takes ARMED out again. */
typedef enum
{
    USB_PIPE_DOWN = 0,
    USB_PIPE_READY,
    USB_PIPE_ARMED
} Usbc_PipeState;

static volatile Usbc_PipeState cdcInPipe;
static volatile Usbc_PipeState cdcNotifPipe;
/* Bytes handed to the IN bank, not yet ACKed: what a link event destroys.
 * Written by the vector and the polls. */
static volatile uint16_t cdcInArmedBytes;
static int usbConfigured;        /* SET_CONFIGURATION with a non-zero value */
static uint16_t usbSetupChanges; /* distinct SETUP contents latched since boot */

/* One DBG_CDC_RSN_* marker per record lost (dbg.h), so the host counts the loss
 * by counting frames. */
static void cdcAccountInLoss(void)
{
    uint16_t lost = (uint16_t)(cdcInArmedBytes / DBG_FRAME_SIZE);

    cdcInArmedBytes = 0;

    while (lost-- != 0u)
        DBG(DBG_COMP_CDC, DBG_CDC_RSN_IN_DISCARDED);
}

/* Called when the endpoint stops existing (bus reset, unconfigure): the one
 * moment this layer learns armed bytes are gone unsent. */
void Usbc_EpBankLost(uint8_t ep, int in)
{
    if (!in)
        return;

    if (ep == CDC_DATA_IN_EP)
    {
        cdcAccountInLoss();
        cdcInPipe = USB_PIPE_DOWN;
    }
    else if (ep == CDC_NOTIF_EP)
    {
        if (cdcNotifPipe == USB_PIPE_ARMED)
            DBG(DBG_COMP_CDC, DBG_CDC_RSN_NOTIF_DISCARDED);

        cdcNotifPipe = USB_PIPE_DOWN;
    }
}

/* SET_CONFIGURATION commit: endpoints just enabled/disabled; usbd_cdc_on_config
 * queues the link state on the notify pipe. */
static void usbDeviceOnConfig(uint8_t cfgValue, void *arg)
{
    (void)arg;

    cdcInArmedBytes = 0;
    usbConfigured = (cfgValue != 0u) ? 1 : 0;
    cdcInPipe = (cfgValue != 0u) ? USB_PIPE_READY : USB_PIPE_DOWN;
    cdcNotifPipe = (cfgValue != 0u) ? USB_PIPE_READY : USB_PIPE_DOWN;
    usbd_cdc_on_config(cfgValue, 0);

    if (cfgValue != 0u)
    {
        DBG(DBG_COMP_USB, DBG_USB_RSN_ENUMERATED);
        Clock_Stamp(DBG_TIME_RSN_CONFIGURED);
        /* This record's line field is the SETUP count, not a source line. */
        Debug_Emit(DBG_COMP_USB, usbSetupChanges, DBG_USB_RSN_SETUP_COUNT);
    }
}

/* Unhandled class request: the driver stalls EP0 IN, the CDC120 answer. */
static int usbDeviceClassRequest(const uint8_t *s8, void *arg)
{
    int handled = usbd_cdc_class_request(s8, arg);

    if (!handled)
        DBG(DBG_COMP_USB, DBG_USB_RSN_SETUP_UNHANDLED);

    return handled;
}

/* ------------------------------------------------------------------ */
/* Bring-up                                                            */
/* ------------------------------------------------------------------ */


void Usbc_Init(void)
{
    cdcInPipe = USB_PIPE_DOWN;
    cdcNotifPipe = USB_PIPE_DOWN;
    cdcInArmedBytes = 0;

    /* Before attach: the descriptors carry the NVM device identity. */
    UsbDesc_Init();
    Clock_Stamp(DBG_TIME_RSN_USBC_DESCS);

    /* GCLK_USB is routed by Clock_Init onto GEN5 and is not touched here. */
    hal_usb_start();
    Clock_Stamp(DBG_TIME_RSN_USBC_HAL);

    /* Driver runs the vendor sequence on top: SWRST, ENABLE in its own bounded
   * store, INTENSET, the EP0 banks, then the attach. */
    usbd_init(&usbDeviceCfg);
    Clock_Stamp(DBG_TIME_RSN_USBC_DRV);
    usbd_bulk_in_cb(CDC_DATA_IN_EP, usbDeviceBulkInDone, 0);
    usbd_bulk_in_cb(CDC_NOTIF_EP, usbDeviceBulkInDone, 0);
    usbd_bulk_out_cb(CDC_DATA_OUT_EP, usbDeviceBulkOutDone, 0);

    Nvram_StatusStage(USBC_STG_EPS_DONE);
}

/* ------------------------------------------------------------------ */
/* Main-loop polls                                                     */
/* ------------------------------------------------------------------ */

int Usbc_Configured(void)
{
    return usbConfigured;
}

uint32_t Usbc_SofFrames(void)
{
    return hal_usb_sofFrames();
}

uint32_t Usbc_Frame(void)
{
    return hal_usb_frame();
}

void Usbc_Attach(int on)
{
    hal_usb_attach(on);
}

void Usbc_Witnesses(void)
{
    hal_usb_witnesses();
}

/* Arm one CDC bulk-IN packet (EP1) from the queued records. Main-loop context
 * only, never a handler: records are produced in the USB vector and consumed
 * here. Whole 8-byte LE records back to back (dbg.h); one packet per call, no
 * waiting. */
uint16_t Usbc_CdcInPoll(void)
{
    uint16_t n;

    if (cdcInPipe == USB_PIPE_DOWN || cdcInPipe == USB_PIPE_ARMED)
        return 0;

    if (usbd_state() != USBD_ST_CONFIGURED)
        return 0;

    n = Debug_QueueTake(dbgDrain, (uint16_t)sizeof(dbgDrain));

    if (n == 0u)
        return 0;
    /* Claim the bank before arming: a completion can only follow BK1RDY. */
    cdcInArmedBytes = n;
    cdcInPipe = USB_PIPE_ARMED;

    if (!usbd_bulk_in(CDC_DATA_IN_EP, dbgDrain, n))
    {
        /* Refused (not configured, too big): the records are out of the queue, so
     * they are lost and reported. */
        cdcAccountInLoss();
        cdcInPipe = USB_PIPE_READY;
        return 0;
    }

    return n;
}

/* Arm one packet from a caller's buffer (usbc.h). usbd_bulk_in copies into the
 * bank, so nothing is retained. */
int Usbc_CdcInSend(const uint8_t *buf, uint16_t len)
{
    if (buf == 0 || len == 0u || len > USB_CDC_IN_MAX)
        return 0;

    if (cdcInPipe == USB_PIPE_DOWN || cdcInPipe == USB_PIPE_ARMED)
        return 0;

    if (usbd_state() != USBD_ST_CONFIGURED)
        return 0;

    cdcInArmedBytes = len;
    cdcInPipe = USB_PIPE_ARMED;

    if (!usbd_bulk_in(CDC_DATA_IN_EP, buf, len))
    {
        cdcAccountInLoss();
        cdcInPipe = USB_PIPE_READY;
        return 0;
    }

    return 1;
}

/* Arm one CDC120 Table 22 notification (EP3, built by usbd_cdc_notify_build)
 * when configured and a link state change waits. Main-loop context. */
uint16_t Usbc_CdcNotifyPoll(void)
{
    uint16_t n;

    if (cdcNotifPipe == USB_PIPE_DOWN || cdcNotifPipe == USB_PIPE_ARMED)
        return 0;

    if (usbd_state() != USBD_ST_CONFIGURED)
        return 0;

    n = usbd_cdc_notify_build(notifInBuf, (uint16_t)sizeof(notifInBuf));

    if (n == 0u)
        return 0;

    cdcNotifPipe = USB_PIPE_ARMED;

    if (!usbd_bulk_in(CDC_NOTIF_EP, notifInBuf, n))
    {
        cdcNotifPipe = USB_PIPE_READY;
        DBG(DBG_COMP_CDC, DBG_CDC_RSN_NOTIF_DISCARDED);
        return 0;
    }

    return n;
}

void Usbc_SetDataOutHandler(Usbc_DataOutFn fn)
{
    dataOutHandler = fn;
}

/* ------------------------------------------------------------------ */
/* Endpoint events, driver context (inside the USB vector)             */
/* ------------------------------------------------------------------ */

static void usbDeviceBulkInDone(uint8_t ep, uint16_t len, void *arg)
{
    (void)len;
    (void)arg;

    /* The module already cleared BK1RDY on the host's ACK (29.6.2.9): the event
   * only frees the pipe; re-arming is the main loop's job. */
    if (ep == CDC_DATA_IN_EP)
    {
        cdcInArmedBytes = 0;

        if (cdcInPipe == USB_PIPE_ARMED)
            cdcInPipe = USB_PIPE_READY;
    }
    else if (ep == CDC_NOTIF_EP)
    {
        if (cdcNotifPipe == USB_PIPE_ARMED)
            cdcNotifPipe = USB_PIPE_READY;
    }
}

static void usbDeviceBulkOutDone(uint8_t ep, uint16_t len, void *arg)
{
    (void)arg;

    if (ep != CDC_DATA_OUT_EP)
        return;
    /* Consume the packet BEFORE re-arming. dataOutBuf is this endpoint's OUT bank
   * (usbDeviceCfg.ep2_rx), so re-arming first lets the next packet's DMA overwrite
   * the bytes this handler has not read yet: a frame split across two packets then
   * loses its first half and is dropped as a bad FCS, with no reply. A packet that
   * arrives while the bank is unarmed is NAKed and retried by the host, so nothing
   * is lost by arming late. */
    if (dataOutHandler)
        dataOutHandler(dataOutBuf, len);

    usbd_bulk_out_rearm(CDC_DATA_OUT_EP);
}

/* ------------------------------------------------------------------ */
/* The vector (IRQ 7: startup/startup_samr21.S)                        */
/* ------------------------------------------------------------------ */

/* HAL_IrqEnter marks handler mode (no MRS IPSR: the no-inline-asm rule), which
 * makes NVRAM status writes queue instead of program flash inside IRQ 7 (defect
 * H-K, samr21_nvram.c). Reads flags and shadow copies only. */
void USB_Handler(void)
{
    uint32_t pending = hal_irq_pending();
    int sawSetup = (pending & (1u << USBD_IRQ_EP0_RXSTP)) != 0;
    int wasHalted = hal_ep_stalled(0, 1);
    uint8_t setupFirst[8];

    HAL_IrqEnter();

    if (pending == 0u)
    {
        /* A latched NVIC request from the bootloader's USB session, with a clean
     * INTFLAG: nothing to do. */
        HAL_IrqExit();
        return;
    }

    if (sawSetup)
    {
        /* Read SETUP before its latch clears (29.6.2.6); this copy is the collision
     * witness below. */
        hal_usb_setup_read(setupFirst);
    }

    if (pending & (1u << USBD_IRQ_RST))
        DBG(DBG_COMP_USB, DBG_USB_RSN_BUS_RESET);

    usbd_irq();

    if (sawSetup)
    {
        uint8_t setupNow[8];
        uint8_t i;
        int same = 1;

        /* SETUP latches into the EP0 OUT bank only while RXSTP is clear (29.6.2.6):
     * a changed buffer means a second SETUP arrived during the driver's read. */
        hal_usb_setup_read(setupNow);

        for (i = 0; i < 8; ++i)
        {
            if (setupNow[i] != setupFirst[i])
            {
                same = 0;
                break;
            }
        }

        if (!same && usbSetupChanges < 0xFFFFu)
        {
            /* Counted, not recorded: dbg.h DBG_USB_RSN_SETUP_COUNT says why. */
            usbSetupChanges++;
        }
    }

    if (!wasHalted && hal_ep_stalled(0, 1))
    {
        /* Only a rejected request leaves EP0 IN halted (cleared on every new
     * SETUP). */
        DBG(DBG_COMP_USB, DBG_USB_RSN_SETUP_UNHANDLED);
    }

    HAL_IrqExit();
}
