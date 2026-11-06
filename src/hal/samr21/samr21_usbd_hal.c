/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* SAMR21 USBC bridge implementing the frozen hal_* contract of usbd_hal.h.
 * Register fields and offsets come from samr21_regs.h (29.8.1/29.8.2/29.8.3).
 * Every hardware wait is bounded and feeds the WDT; a sync that does not settle
 * is recorded and the attach continues. Naming provenance: usb/usbd.h. */
#include <stdint.h>

#include "usbd_int.h" /* usbd_cfg_t / usbd_config(): the buffer contract */
#include "hal.h"
#include "hal_diag.h"
#include "usbc.h"
#include "dbg.h"
#include "samr21_regs.h"

/* ------------------------------------------------------------------ */
/* Bring-up                                                            */
/* ------------------------------------------------------------------ */

/* SRAM endpoint descriptor table: (USBD_MAX_EP+1) x 2 banks x 16 bytes
 * (29.8.3.1, Table 29-15); DESCADD points at it, 4-byte aligned for the packet
 * DMA (29.6.2). */
static UsbDescBank usbHalDescBank[USBD_MAX_EP + 1][2] __attribute__((aligned(4)));

typedef char usbdHalBankTableIsTwoBanksPerEndpoint
    [sizeof(usbHalDescBank) == (USBD_MAX_EP + 1) * 2u * sizeof(UsbDescBank) ? 1 : -1];

/* DESCADD must name internal SRAM (RAM_START/RAM_SIZE from the linker script,
 * samr21_regs.h): anywhere else the packet DMA cannot fetch. */
#define USB_RAM_MASK 0xFFF00000u

/* Host frames since boot. hal_irq_pending is the only place SOF is taken off
 * INTFLAG, so the only place a frame can honestly be counted. */
static volatile uint32_t usbdHalSofFrames;

/* FNUM is 11 bits but not the register's low 11: FNUM[10:5] at bits 13:8,
 * FNUM[4:0] at bits 7:3, MFNUM at 2:0 (Table 29-16, samr21.txt:37456-37466). */
#define USB_FNUM_HI_SHIFT 8
#define USB_FNUM_HI_MASK 0x3Fu
#define USB_FNUM_LO_SHIFT 3
#define USB_FNUM_LO_MASK 0x1Fu

static uint32_t usbdHalFrame(void)
{
    uint16_t r = SAMR21_USB->FNUM;

    return ((uint32_t)(r >> USB_FNUM_HI_SHIFT) & USB_FNUM_HI_MASK) << 5 |
           ((uint32_t)(r >> USB_FNUM_LO_SHIFT) & USB_FNUM_LO_MASK);
}

/* DBG_COMP_WITNESS side (dbg.h). INTEN read from INTENSET (the SAM R21 has no
 * plain INTEN); STATUS.SPEED matters because the clock proof divides cycles by
 * frames, 1 ms each only at full speed (Table 29-13). */
void hal_usb_witnesses(void)
{
    Samr21Usb *hw = SAMR21_USB;

    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)hw->INTENSET, DBG_WITNESS_RSN_USB_INTEN);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)hw->INTFLAG, DBG_WITNESS_RSN_USB_INTFLAG);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)hw->STATUS, DBG_WITNESS_RSN_USB_STATUS);
    Debug_Emit(DBG_COMP_WITNESS, (uint16_t)usbdHalFrame(), DBG_WITNESS_RSN_USB_FNUM);
}

#define USB_SYNCBUSY_SWRST (1u << 0)  /* 29.8.1.1 */
#define USB_SYNCBUSY_ENABLE (1u << 1) /* 29.8.1.3 */

/* Bounded waits (same concern as CLK_WAIT_LIMIT in samr21_clock.c): a SYNCBUSY
 * bit rises within a few bus cycles, so asserting is cheap; settling is what can
 * genuinely never happen — a USBC without its GCLK channel latches SWRST
 * forever. 4096 polls is ~5 ms at 8 MHz, ~0.9 ms at 48 MHz, orders of magnitude
 * above a write-synchronisation window (29.8.1.1). */
#define USBC_SYNC_SET_LIMIT 0x1000u
#define USBC_SYNC_CLR_LIMIT 0x1000u

/* 4/5/6 keep the codes the replaced driver used, so a captured record stays
 * readable; dbg.h owns 7. */
#define DBG_USB_RSN_SYNC_TIMEOUT 4u
#define DBG_USB_RSN_ENABLE_UNSET 5u /* ENABLE sync unsettled: recorded, not fatal */
#define DBG_USB_RSN_ENABLE_NOT_LATCHED                                                             \
    6u /* ENABLE never reads back set: the host cannot see this USBC */
#define DBG_USB_RSN_DESCADD_OUT_OF_RAM 8u

static int usbcWaitSyncSet(uint8_t bit, uint32_t limit)
{
    uint32_t n = 0;

    while (!(SAMR21_USB->SYNCBUSY & bit))
    {
        Clock_BringupPump();
        Wdt_FeedPoll(n);

        if (++n >= limit)
            return 0;
    }

    return 1;
}

static int usbcWaitSyncClear(uint8_t bit, uint32_t limit)
{
    uint32_t n = 0;

    while (SAMR21_USB->SYNCBUSY & bit)
    {
        Clock_BringupPump();
        Wdt_FeedPoll(n);

        if (++n >= limit)
            return 0;
    }

    return 1;
}

/* Assert, then settle. A missed assert is not fatal; a settle that never
 * happened is reported because the next core-register write may be discarded
 * (12.3.2.2). 1 = settled, 0 = the bounded window ran out. */
static int usbcSync(uint8_t trigger)
{
    if (!usbcWaitSyncSet(trigger, USBC_SYNC_SET_LIMIT))
        DBG(DBG_COMP_USB, DBG_USB_RSN_SYNC_TIMEOUT);

    if (!usbcWaitSyncClear(trigger, USBC_SYNC_CLR_LIMIT))
    {
        DBG(DBG_COMP_USB, DBG_USB_RSN_SYNC_TIMEOUT);
        return 0;
    }

    return 1;
}

/* PADCAL from the NVM fuses (29.8.1.6); a fuse field reading all-ones means no
 * valid calibration, so the proven values substitute for it. */
static uint16_t usbcPadCal(void)
{
    const uint32_t fuses = FUSES_OTP4_WORD;
    uint32_t transp = FUSE_USB_TRANSP(fuses);
    uint32_t transn = FUSE_USB_TRANSN(fuses);
    uint32_t trim = FUSE_USB_TRIM(fuses);

    if (transp == 0x1Fu)
        transp = 29u;

    if (transn == 0x1Fu)
        transn = 5u;

    if (trim == 0x7u)
        trim = 3u;

    return (uint16_t)(USB_PADCAL_TRANSP(transp) | USB_PADCAL_TRANSN(transn) |
                      USB_PADCAL_TRIM(trim));
}

/* USB D+/D- pads PA24/PA25, mux position G (6): routes the internal D+ pull-up
 * to the connector. No INEN: the PHY drives these pads. */
static void usbcDpDmPins(void)
{
    Samr21PortGroup *g0 = SAMR21_PORT_G(0);

    /* PA24 = bit 8, PA25 = bit 9 of the upper half-word (pins 16-31) selected
   * by HWSEL. */
    g0->DIRCLR = PORT_PIN(24) | PORT_PIN(25);
    g0->WRCONFIG = PORT_WRCONFIG_HWSEL | PORT_WRCONFIG_WRPINCFG | PORT_WRCONFIG_WRPMUX |
                   PORT_WRCONFIG_PMUX(PORT_MUX_PA24G_USB_DM) | PORT_WRCONFIG_PMUXEN |
                   PORT_WRCONFIG_PINMASK(PORT_PIN(24) >> 16);
    g0->WRCONFIG = PORT_WRCONFIG_HWSEL | PORT_WRCONFIG_WRPINCFG | PORT_WRCONFIG_WRPMUX |
                   PORT_WRCONFIG_PMUX(PORT_MUX_PA25G_USB_DP) | PORT_WRCONFIG_PMUXEN |
                   PORT_WRCONFIG_PINMASK(PORT_PIN(25) >> 16);
}

static void usbcBanksClear(void)
{
    /* Shared with the packet DMA (STATUS_BK / PCKSIZE.BYTE_COUNT, 29.6.2), so
   * volatile; a byte loop also avoids a libc memset a -nostdlib image cannot
   * call. */
    volatile uint8_t *p = (volatile uint8_t *)usbHalDescBank;
    uint32_t i;

    for (i = 0; i < sizeof(usbHalDescBank); ++i)
        p[i] = 0u;
}

static void usbcDescAdd(void)
{
    uint32_t addr = (uint32_t)usbHalDescBank;

    if ((addr & USB_RAM_MASK) != RAM_START || addr + sizeof(usbHalDescBank) > RAM_START + RAM_SIZE)
        DBG(DBG_COMP_USB, DBG_USB_RSN_DESCADD_OUT_OF_RAM);

    SAMR21_USB->DESCADD = addr;
    usbcBanksClear();
}

/* Bring-up before the module is reset: interface clock, pads, PADCAL, QOSCTRL,
 * CTRLA.MODE/RUNSTDBY, CTRLB.SPDCONF, DESCADD (halUsb.c:920-991). GCLK_USB is
 * routed by Clock_Init and deliberately not written here — re-routing a channel
 * during an open synchronisation is the errata-10416 stall. */
void hal_usb_start(void)
{
    Samr21Usb *hw = SAMR21_USB;

    /* Digital interface clock: on by reset/bootloader, but the init must be
   * self-contained on any entry path. */
    SAMR21_PM->APBBMASK |= PM_APBBMASK_USB;

    usbcDpDmPins();

    hw->PADCAL = usbcPadCal();
    /* Medium quality, the vendor bootloader's proven values. */
    hw->QOSCTRL = USB_QOSCTRL_CQOS(2) | USB_QOSCTRL_DQOS(2);
    /* CTRLA.MODE stays 0 = device. RUNSTDBY in its own store while the module
   * is still disabled (Enable-Protected, 29.8.1.1). */
    hw->CTRLA = USB_CTRLA_RUNSTDBY;
    /* CTRLB.SPDCONF = 00 (FS-capable PHY running at full speed, 29.8.2.1) and
   * DETACH set: no pull-up until the module is enabled and EP0 is ready. */
    hw->CTRLB = USB_CTRLB_DETACH;
    usbcDescAdd();

    /* The bootloader jumps with the USB IRQ enabled and possibly pending; take it
   * down now, hal_usb_enable() re-arms it once INTFLAG is clean. */
    NVIC_ICER0 = 1u << IRQN_USB;
}

/* SWRST wipes the bring-up config: carry PADCAL/QOSCTRL/CTRLA/CTRLB across
 * while the module is still disabled. */
void hal_usb_swrst(void)
{
    Samr21Usb *hw = SAMR21_USB;
    uint16_t padcal = hw->PADCAL;
    uint8_t qosctrl = hw->QOSCTRL;
    uint8_t ctrla = hw->CTRLA & (uint8_t)(USB_CTRLA_RUNSTDBY | USB_CTRLA_MODE_HOST);
    uint8_t ctrlb = hw->CTRLB;

    hw->CTRLA = USB_CTRLA_SWRST;
    (void)usbcSync(USB_SYNCBUSY_SWRST);

    hw->PADCAL = padcal;
    hw->QOSCTRL = qosctrl;
    hw->CTRLA = ctrla;
    hw->CTRLB = (uint8_t)(ctrlb | USB_CTRLB_DETACH);
    usbcDescAdd();
    Clock_Stamp(DBG_TIME_RSN_USBC_SWRST);
}

/* Vendor sequencing: RUNSTDBY in its own store while disabled (hal_usb_swrst),
 * then ENABLE alone — a store of ENABLE|RUNSTDBY would put an Enable-Protected
 * bit into the very write that enables the module (halUsb.c:985/:875). 29.8.1.1
 * promises ENABLE reads back immediately, so read it back; 12.3.2.2 discards a
 * core-register write while a synchronisation runs, so issue it once more. */
void hal_usb_enable(int on)
{
    Samr21Usb *hw = SAMR21_USB;
    int latched = 0;

    if (!on)
    {
        hw->CTRLA = (uint8_t)(hw->CTRLA & (uint8_t)~USB_CTRLA_ENABLE);
        (void)usbcSync(USB_SYNCBUSY_ENABLE);
        return;
    }

    hw->CTRLA |= USB_CTRLA_ENABLE;

    if (!usbcSync(USB_SYNCBUSY_ENABLE))
    {
        DBG(DBG_COMP_USB, DBG_USB_RSN_ENABLE_UNSET); /* recorded, not fatal */
    }

    if (hw->CTRLA & USB_CTRLA_ENABLE)
        latched = 1;
    else
    {
        hw->CTRLA |= USB_CTRLA_ENABLE;
        (void)usbcSync(USB_SYNCBUSY_ENABLE);

        if (hw->CTRLA & USB_CTRLA_ENABLE)
            latched = 1;
    }

    if (!latched)
    {
        DBG(DBG_COMP_USB, DBG_USB_RSN_ENABLE_NOT_LATCHED);
        return;
    }

    /* EORST, the suspend/resume pair, and SOF — the DFLL48M in USBCRM recovers
   * its 48 MHz from SOF. */
    hw->INTENSET = USB_INTFLAG_EORST | USB_INTFLAG_SUSPEND | USB_INTFLAG_WAKEUP |
                   USB_INTFLAG_EORSM | USB_INTFLAG_UPRSM | USB_INTFLAG_SOF;
    /* RXSTP is the SETUP latch (29.6.2.6), TRCPT0/TRCPT1 the EP0 stages; endpoint
   * events go through EPINTENSET (INTENSET has no EPINT bit). */
    SAMR21_USB_EP(0)->EPINTENSET = USB_EPINT_RXSTP | USB_EPINT_TRCPT0 | USB_EPINT_TRCPT1;
    NVIC_ISER0 = 1u << IRQN_USB;
    Clock_Stamp(DBG_TIME_RSN_USBC_ENABLE);
}

int hal_usb_enabled(void)
{
    return (SAMR21_USB->CTRLA & USB_CTRLA_ENABLE) ? 1 : 0;
}

/* Attach = clear DETACH (29.8.2.1): the internal D+ pull-up reaches the
 * connector. A whole-register write would reset SPDCONF, so read-modify-write
 * both directions. */
void hal_usb_attach(int on)
{
    Samr21Usb *hw = SAMR21_USB;

    if (on)
        hw->CTRLB = (uint8_t)(hw->CTRLB & (uint8_t)~USB_CTRLB_DETACH);
    else
        hw->CTRLB = (uint8_t)(hw->CTRLB | USB_CTRLB_DETACH);

    Clock_Stamp(DBG_TIME_RSN_USBC_ATTACH);
}

/* Host SOF frames since boot, one per 1 ms: the trigger half of the clock
 * proof in app/main.c (cycles per frame x 1000 = Hz); FNUM is the divisor. */
uint32_t hal_usb_sofFrames(void)
{
    return usbdHalSofFrames;
}

/* Module frame number, 11 bits, wrapping every 2048 frames (2.05 s), advanced
 * once per detected SOF: the host's 1 kHz as the device sees it. */
uint32_t hal_usb_frame(void)
{
    return usbdHalFrame();
}

uint8_t hal_usb_get_addr(void)
{
    return (uint8_t)(SAMR21_USB->DADD & 0x7Fu);
}

void hal_usb_set_addr(uint8_t addr)
{
    SAMR21_USB->DADD = USB_DADD_ADDEN | USB_DADD_DADD(addr);
}

/* ------------------------------------------------------------------ */
/* Endpoint banks                                                      */
/* ------------------------------------------------------------------ */

/* EPTYPE code per endpoint direction, 0 = no such direction; the map usb_desc.c
 * reports to the host. */
static const uint8_t epType[USBD_MAX_EP + 1][2] = {
    {1, 1}, /* EP0 control */
    {0, 3}, /* EP1 bulk IN */
    {3, 0}, /* EP2 bulk OUT */
    {0, 4}  /* EP3 interrupt IN */
};

static UsbEndpoint *epReg(uint8_t ep)
{
    if (ep > USBD_MAX_EP)
        return 0;

    return SAMR21_USB_EP(ep); /* +0x100 + 0x20*n, samr21_regs.h */
}

static UsbDescBank *bankOf(uint8_t ep, int in)
{
    if (ep > USBD_MAX_EP)
        return 0;

    return &usbHalDescBank[ep][in ? USBD_BANK_IN : USBD_BANK_OUT];
}

/* One shared buffer per endpoint direction (usbd_cfg_t contract): the bank's
 * ADDR points at it, so driver and module touch the same place. */
static uint8_t *bufOf(uint8_t ep, int in)
{
    const usbd_cfg_t *cfg = usbd_config();

    if (!cfg)
        return 0;

    switch (ep)
    {
        case 0:
            return in ? cfg->ep1_tx : cfg->ep0_rx;
        case 1:
            return in ? cfg->ep1_tx : cfg->ep1_rx;
        case 2:
            return in ? cfg->ep2_tx : cfg->ep2_rx;
        case 3:
            return in ? cfg->ep3_tx : cfg->ep3_rx;
        default:
            return 0;
    }
}

static void epTypeSet(uint8_t ep, int in, uint8_t type)
{
    UsbEndpoint *e = epReg(ep);

    if (!e)
        return;
    /* Read-modify-write on one direction's nibble: EPCFG is one byte holding
   * both, and EP0 has two directions (29.6.2.4). */
    if (in)
        e->EPCFG = (uint8_t)((e->EPCFG & (uint8_t)~USB_EPCFG_EPTYPE1(7)) | USB_EPCFG_EPTYPE1(type));
    else
        e->EPCFG = (uint8_t)((e->EPCFG & (uint8_t)~USB_EPCFG_EPTYPE0(7)) | USB_EPCFG_EPTYPE0(type));
}

void hal_ep_enable(uint8_t ep, int in, int stall)
{
    UsbEndpoint *e = epReg(ep);
    UsbDescBank *b = bankOf(ep, in);
    uint8_t *buf = bufOf(ep, in);
    uint8_t t;

    if (!e || !b || !buf)
        return;

    t = epType[ep][in ? 1 : 0];

    if (t == 0u)
    {
        return; /* this endpoint has no such direction */
    }

    epTypeSet(ep, in, t);
    b->ADDR = (uint32_t)buf;
    /* SIZE code 3 = 64 bytes (Table 29-16), the descriptor's max packet; AUTO_ZLP
   * stays 0 (the driver sends its own ZLPs). */
    b->PCKSIZE = USB_PCKSIZE_SIZE(3);

    if (in)
    {
        e->EPSTATUSCLR = USB_EPSTATUS_BK1RDY; /* IN ready == filled: start NAK */
        e->EPINTENSET = USB_EPINT_TRCPT1;
    }
    else
    {
        /* OUT ready == empty: clearing BK0RDY arms the bank (29.6.2). */
        e->EPSTATUSCLR = USB_EPSTATUS_BK0RDY;
        e->EPINTENSET = USB_EPINT_TRCPT0;
    }

    if (stall)
        hal_ep_stall(ep, in);
}

void hal_ep_disable(uint8_t ep, int in)
{
    UsbEndpoint *e = epReg(ep);

    if (!e)
        return;
    /* The one place an armed bank is destroyed unread (bus reset, unconfigure):
   * where the CDC layer learns its outstanding records are gone. */
    Usbc_EpBankLost(ep, in);
    epTypeSet(ep, in, 0u);

    if (in)
    {
        e->EPSTATUSCLR = USB_EPSTATUS_BK1RDY; /* drop whatever the bank held */
    }
    else
    {
        e->EPSTATUSSET = USB_EPSTATUS_BK0RDY; /* an unusable OUT must not ACK */
    }
}

void *hal_ep_buf(uint8_t ep, int in, uint8_t bank)
{
    (void)bank;
    return bufOf(ep, in);
}

void hal_ep_arm(uint8_t ep, int in, uint8_t bank, uint16_t len)
{
    UsbEndpoint *e = epReg(ep);
    UsbDescBank *b = bankOf(ep, in);
    uint8_t *buf = bufOf(ep, in);

    (void)bank;

    if (!e || !b || !buf)
        return;

    b->ADDR = (uint32_t)buf;

    if (in)
    {
        b->PCKSIZE = USB_PCKSIZE_SIZE(3) | USB_PCKSIZE_BYTE_COUNT(len);
        e->EPSTATUSSET = USB_EPSTATUS_BK1RDY;
    }
    else
    {
        b->PCKSIZE =
            USB_PCKSIZE_SIZE(3) | USB_PCKSIZE_MULTI_PACKET_SIZE(64) | USB_PCKSIZE_BYTE_COUNT(0);
        e->EPSTATUSCLR = USB_EPSTATUS_BK0RDY;
    }
    /* A NAK before this arm leaves TRFAIL behind; clearing it with the arm makes
   * the next completion describe this transfer. TRFAIL is not in EPINTENSET. */
    e->EPINTFLAG = in ? USB_EPINT_TRFAIL1 : USB_EPINT_TRFAIL0;
}

void hal_ep_disarm(uint8_t ep, int in, uint8_t bank)
{
    UsbEndpoint *e = epReg(ep);

    (void)bank;

    if (!e)
        return;

    if (in)
        e->EPSTATUSCLR = USB_EPSTATUS_BK1RDY;
    else
        e->EPSTATUSSET = USB_EPSTATUS_BK0RDY;
}

void hal_ep_stall(uint8_t ep, int in)
{
    UsbEndpoint *e = epReg(ep);

    if (!e)
        return;

    e->EPSTATUSSET = in ? USB_EPSTATUS_STALLRQ1 : USB_EPSTATUS_STALLRQ0;
}

void hal_ep_clear_stall(uint8_t ep, int in)
{
    UsbEndpoint *e = epReg(ep);

    if (!e)
        return;

    if (!(e->EPSTATUS & (in ? USB_EPSTATUS_STALLRQ1 : USB_EPSTATUS_STALLRQ0)))
    {
        return; /* nothing halts this direction: a clear would only churn its toggle */
    }

    e->EPSTATUSCLR = in ? USB_EPSTATUS_STALLRQ1 : USB_EPSTATUS_STALLRQ0;

    if (e->EPINTFLAG & (in ? USB_EPINT_STALL1 : USB_EPINT_STALL0))
        e->EPINTFLAG = in ? USB_EPINT_STALL1 : USB_EPINT_STALL0;
    /* USB 2.0 8.5/8.6: a halted endpoint's data toggle restarts from data0 on
   * the next transfer; clear it explicitly. */
    e->EPSTATUSCLR = in ? USB_EPSTATUS_DTGLIN : USB_EPSTATUS_DTGLOUT;
}

int hal_ep_stalled(uint8_t ep, int in)
{
    UsbEndpoint *e = epReg(ep);

    if (!e)
        return 0;

    return (e->EPSTATUS & (in ? USB_EPSTATUS_STALLRQ1 : USB_EPSTATUS_STALLRQ0)) ? 1 : 0;
}

int hal_ep_armed(uint8_t ep, int in, uint8_t bank)
{
    UsbEndpoint *e = epReg(ep);

    (void)bank;

    if (!e)
        return 0;

    if (in)
        return (e->EPSTATUS & USB_EPSTATUS_BK1RDY) ? 1 : 0;
    /* OUT armed means empty, i.e. BK0RDY cleared. */
    return (e->EPSTATUS & USB_EPSTATUS_BK0RDY) ? 0 : 1;
}

uint8_t hal_ep_toggle(uint8_t ep, int in)
{
    UsbEndpoint *e = epReg(ep);

    if (!e)
        return 0;

    return (e->EPSTATUS & (in ? USB_EPSTATUS_DTGLIN : USB_EPSTATUS_DTGLOUT)) ? 1u : 0u;
}

void hal_ep_reset_toggle(uint8_t ep, int in)
{
    UsbEndpoint *e = epReg(ep);

    if (!e)
        return;
    /* EPSTATUSCLR is write-0-to-ignore: both bits at once is one store. */
    e->EPSTATUSCLR = in ? (USB_EPSTATUS_DTGLIN | USB_EPSTATUS_STALLRQ1)
                        : (USB_EPSTATUS_DTGLOUT | USB_EPSTATUS_STALLRQ0);
}

uint16_t hal_ep_rx_len(uint8_t ep, int in)
{
    UsbDescBank *b = bankOf(ep, 0);

    (void)in;

    if (!b)
        return 0;

    return (uint16_t)(b->PCKSIZE & 0x3FFFu); /* BYTE_COUNT [13:0], 29.8.3.1 */
}

/* ------------------------------------------------------------------ */
/* Interrupts                                                          */
/* ------------------------------------------------------------------ */

/* INTFLAG/EPINTFLAG are polled, never acked by hardware: a latched event needs
 * an explicit write to clear (29.8.4.1, 29.6.2.11). The mask is the driver's
 * own event numbering, not the module's bits.
 * SOF has no driver numbering, so the bridge acks it here — the one place every
 * vector is seen; an enabled-and-never-cleared SOF holds the IRQ line forever. */
uint32_t hal_irq_pending(void)
{
    Samr21Usb *hw = SAMR21_USB;
    uint32_t p = 0;
    uint16_t d = hw->INTFLAG;
    uint8_t ep;

    if (d & USB_INTFLAG_SOF)
    {
        hw->INTFLAG = USB_INTFLAG_SOF;
        d = hw->INTFLAG;
        usbdHalSofFrames++;
    }

    if (d & USB_INTFLAG_EORST)
        p |= 1u << USBD_IRQ_RST;

    if (d & USB_INTFLAG_SUSPEND)
        p |= 1u << USBD_IRQ_SUSP;

    if (d & (USB_INTFLAG_WAKEUP | USB_INTFLAG_EORSM | USB_INTFLAG_UPRSM))
        p |= 1u << USBD_IRQ_WAKEUP;

    for (ep = 0; ep <= USBD_MAX_EP; ep++)
    {
        UsbEndpoint *e = epReg(ep);
        uint16_t f;

        if (!e)
            continue;

        f = e->EPINTFLAG;

        if (ep == 0)
        {
            if (f & USB_EPINT_RXSTP)
                p |= 1u << USBD_IRQ_EP0_RXSTP;

            if (f & USB_EPINT_TRCPT1)
                p |= 1u << USBD_IRQ_EP0_TX;

            if (f & USB_EPINT_TRCPT0)
                p |= 1u << USBD_IRQ_EP0_RX;
        }
        else
        {
            if (f & USB_EPINT_TRCPT1)
                p |= 1u << (USBD_IRQ_EP1_TX + (ep - 1) * 2);

            if (f & USB_EPINT_TRCPT0)
                p |= 1u << (USBD_IRQ_EP1_RX + (ep - 1) * 2);
        }
    }

    return p;
}

void hal_irq_clear(uint32_t mask)
{
    Samr21Usb *hw = SAMR21_USB;
    uint8_t ep;

    if (mask & (1u << USBD_IRQ_RST))
        hw->INTFLAG = USB_INTFLAG_EORST;

    if (mask & (1u << USBD_IRQ_SUSP))
    {
        hw->INTFLAG = USB_INTFLAG_SUSPEND;
        /* SUSPEND is raised once; arm the resume events instead until the
     * resume actually arrives (29.8.4.3). */
        hw->INTENCLR = USB_INTFLAG_SUSPEND;
        hw->INTENCLR = USB_INTFLAG_WAKEUP | USB_INTFLAG_EORSM | USB_INTFLAG_UPRSM;
        hw->INTENSET = USB_INTFLAG_WAKEUP | USB_INTFLAG_EORSM | USB_INTFLAG_UPRSM;
    }

    if (mask & (1u << USBD_IRQ_WAKEUP))
    {
        hw->INTFLAG = USB_INTFLAG_WAKEUP | USB_INTFLAG_EORSM | USB_INTFLAG_UPRSM;
        hw->INTENCLR = USB_INTFLAG_WAKEUP | USB_INTFLAG_EORSM | USB_INTFLAG_UPRSM;
        hw->INTENSET = USB_INTFLAG_SUSPEND;
    }

    if (mask & (1u << USBD_IRQ_EP0_RXSTP))
        SAMR21_USB_EP(0)->EPINTFLAG = USB_EPINT_RXSTP;

    if (mask & (1u << USBD_IRQ_EP0_TX))
        SAMR21_USB_EP(0)->EPINTFLAG = USB_EPINT_TRCPT1;

    if (mask & (1u << USBD_IRQ_EP0_RX))
        SAMR21_USB_EP(0)->EPINTFLAG = USB_EPINT_TRCPT0;

    for (ep = 1; ep <= USBD_MAX_EP; ep++)
    {
        if (mask & (1u << (USBD_IRQ_EP1_TX + (ep - 1) * 2)))
            SAMR21_USB_EP(ep)->EPINTFLAG = USB_EPINT_TRCPT1;

        if (mask & (1u << (USBD_IRQ_EP1_RX + (ep - 1) * 2)))
            SAMR21_USB_EP(ep)->EPINTFLAG = USB_EPINT_TRCPT0;
    }

    if (mask & (1u << USBD_IRQ_RST))
    {
        /* Disabling an endpoint clears its EPINTEN (29.6.2.4); re-arm EP0's sets
     * defensively after the reset. */
        hw->INTENSET = USB_INTFLAG_EORST | USB_INTFLAG_SUSPEND | USB_INTFLAG_WAKEUP |
                       USB_INTFLAG_EORSM | USB_INTFLAG_UPRSM | USB_INTFLAG_SOF;
        SAMR21_USB_EP(0)->EPINTENSET = USB_EPINT_RXSTP | USB_EPINT_TRCPT0 | USB_EPINT_TRCPT1;
    }
}

void hal_usb_setup_read(uint8_t *buf8)
{
    const uint8_t *src = usbd_config()->ep0_rx;
    uint8_t i;

    /* Read-only: the driver clears the RXSTP latch separately. */
    for (i = 0; i < 8; ++i)
        buf8[i] = src[i];
}

/* The DFLL48M in USBCRM recovers its 48 MHz from the SOF stream (15.6.7) and
 * the SYSCTRL PCLKSR lock bits are unreliable there (errata 11938): no lock to
 * poll. GCLK_USB stays routed by Clock_Init. */
void hal_dfll_ready(void)
{
}
