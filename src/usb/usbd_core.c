/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
#include "usbd.h"
#include "usbd_int.h"

static const usbd_cfg_t *g_cfg;
static usbd_state_t g_st = USBD_ST_DEFAULT;
static uint8_t g_cfg_value;
static uint32_t g_recoveries;
static int g_suspended;

static void usbd_copy(uint8_t *dst, const uint8_t *src, uint16_t n)
{
    uint16_t i;

    for (i = 0; i < n; i++)
        dst[i] = src[i];
}

void usbd_core_reset(void)
{
    uint8_t ep;

    g_st = USBD_ST_DEFAULT;
    g_cfg_value = 0;
    g_suspended = 0;
    hal_usb_set_addr(0);
    hal_ep_reset_toggle(0, 1);
    hal_ep_reset_toggle(0, 0);

    for (ep = 0; ep <= USBD_MAX_EP; ep++)
    {
        hal_ep_clear_stall(ep, 1);
        hal_ep_clear_stall(ep, 0);
    }

    for (ep = 1; ep <= USBD_MAX_EP; ep++)
    {
        hal_ep_disable(ep, 1);
        hal_ep_disable(ep, 0);
    }

    usbd_ep0_init(1);
}

int usbd_init(const usbd_cfg_t *cfg)
{
    g_cfg = cfg;
    hal_usb_swrst();
    hal_usb_enable(1);
    usbd_ep0_init(1);
    hal_usb_attach(1);
    g_st = USBD_ST_DEFAULT;
    g_cfg_value = 0;
    g_suspended = 0;
    g_recoveries = 0;
    return 0;
}

/* Banks survive suspend, but a reset during suspend did not reach us: rebuild
 * the software state from the address the module still holds. */
int usbd_reinit(void)
{
    uint8_t a;
    uint8_t ep;

    hal_irq_clear(hal_irq_pending());
    a = hal_usb_get_addr();
    g_st = a == 0 ? USBD_ST_DEFAULT : (g_cfg_value ? USBD_ST_CONFIGURED : USBD_ST_ADDRESSED);
    g_suspended = 0;
    usbd_ep0_init(a == 0);

    if (g_st == USBD_ST_CONFIGURED)
    {
        for (ep = 1; ep <= USBD_MAX_EP; ep++)
        {
            hal_ep_enable(ep, 1, 0);
            hal_ep_enable(ep, 0, 0);
        }
    }

    return 0;
}

usbd_state_t usbd_state(void)
{
    return g_st;
}

uint8_t usbd_addr(void)
{
    return hal_usb_get_addr();
}

int usbd_suspended(void)
{
    return g_suspended;
}

uint32_t usbd_recoveries(void)
{
    return g_recoveries;
}

void usbd_core_commit_addr(uint8_t a)
{
    hal_usb_set_addr(a);
    /* A device with address 0 is in Default state (USB 2.0 9.1.1). */
    g_st = (a == 0) ? USBD_ST_DEFAULT : USBD_ST_ADDRESSED;
}

void usbd_core_commit_cfg(uint8_t v)
{
    uint8_t ep;

    g_cfg_value = v;
    g_st = USBD_ST_CONFIGURED;

    for (ep = 1; ep <= USBD_MAX_EP; ep++)
    {
        if (v)
        {
            hal_ep_enable(ep, 1, 0);
            hal_ep_enable(ep, 0, 0);
        }
        else
        {
            hal_ep_disable(ep, 1);
            hal_ep_disable(ep, 0);
        }
    }

    if (g_cfg && g_cfg->on_config)
        g_cfg->on_config(v, g_cfg->on_config_arg);
}

struct bulk_slot
{
    usbd_bulk_cb_t cb;
    void *arg;
};

static struct bulk_slot bulk_tx[USBD_MAX_EP];
static struct bulk_slot bulk_rx[USBD_MAX_EP];

void usbd_bulk_in_cb(uint8_t ep, usbd_bulk_cb_t cb, void *arg)
{
    if (ep < 1 || ep > USBD_MAX_EP)
        return;

    bulk_tx[ep - 1].cb = cb;
    bulk_tx[ep - 1].arg = arg;
}

void usbd_bulk_out_cb(uint8_t ep, usbd_bulk_cb_t cb, void *arg)
{
    if (ep < 1 || ep > USBD_MAX_EP)
        return;

    bulk_rx[ep - 1].cb = cb;
    bulk_rx[ep - 1].arg = arg;
}

int usbd_bulk_in(uint8_t ep, const void *data, uint16_t len)
{
    if (ep < 1 || ep > USBD_MAX_EP || g_st != USBD_ST_CONFIGURED || len == 0 || len > USBD_BULK_MPS)
        return 0;

    usbd_copy(hal_ep_buf(ep, 1, USBD_BANK_IN), data, len);
    hal_ep_arm(ep, 1, USBD_BANK_IN, len);
    return 1;
}

void usbd_bulk_out_rearm(uint8_t ep)
{
    if (ep < 1 || ep > USBD_MAX_EP || g_st != USBD_ST_CONFIGURED)
        return;

    hal_ep_arm(ep, 0, 0, 0);
}

const usbd_cfg_t *usbd_config(void)
{
    return g_cfg;
}

uint8_t usbd_cfg_value(void)
{
    return g_cfg_value;
}

void usbd_irq(void)
{
    uint32_t p;
    int susp;
    uint8_t ep;

    p = hal_irq_pending();

    if (p == 0)
        return;

    if (p & (1u << USBD_IRQ_RST))
    {
        /* A bus reset invalidates all other pending events. */
        hal_irq_clear(p);
        usbd_core_reset();
        return;
    }

    susp = (p & (1u << USBD_IRQ_SUSP)) != 0;

    if (susp)
    {
        hal_irq_clear(1u << USBD_IRQ_SUSP);
        g_suspended = 1;
    }
    /* Resume path (WAKEUP). */
    if (!susp && g_suspended)
    {
        g_suspended = 0;
        hal_dfll_ready();
    }

    if (p & (1u << USBD_IRQ_WAKEUP))
        hal_irq_clear(1u << USBD_IRQ_WAKEUP);

    if (p & (1u << USBD_IRQ_EP0_RXSTP))
    {
        /* EP0 TX/RX + RXSTP: the previous transfer's status completion,
         * ACKed by the host before this SETUP - handle it, else the deferred
         * commit is lost. */
        if ((p & (1u << USBD_IRQ_EP0_TX)) && usbd_ep0_state() == EP0_STATUS)
        {
            hal_irq_clear(1u << USBD_IRQ_EP0_TX);
            usbd_ep0_tx();
        }

        if ((p & (1u << USBD_IRQ_EP0_RX)) && usbd_ep0_state() == EP0_STATUS)
        {
            hal_irq_clear(1u << USBD_IRQ_EP0_RX);
            usbd_ep0_rx();
        }

        if (usbd_ep0_state() != EP0_IDLE && usbd_ep0_state() != EP0_STALLED)
        {
            g_recoveries++;
            usbd_ep0_recover();
        }

        {
            uint8_t s8[8];
            /* Read the SETUP before clearing RXSTP (DS 29.6.2.6). */
            hal_usb_setup_read(s8);
            /* Remaining EP0 TX/RX: the aborted transfer's data stage, dropped
             * (USB 2.0 8.1). */
            hal_irq_clear((1u << USBD_IRQ_EP0_RXSTP) | (1u << USBD_IRQ_EP0_TX) |
                          (1u << USBD_IRQ_EP0_RX));
            usbd_ep0_setup(s8);
        }
    }
    else
    {
        if (p & (1u << USBD_IRQ_EP0_TX))
        {
            hal_irq_clear(1u << USBD_IRQ_EP0_TX);
            usbd_ep0_tx();
        }

        if (p & (1u << USBD_IRQ_EP0_RX))
        {
            hal_irq_clear(1u << USBD_IRQ_EP0_RX);
            usbd_ep0_rx();
        }
    }
    /* Bulk completions run after the EP0 events of the same batch. */
    for (ep = 1; ep <= USBD_MAX_EP; ep++)
    {
        uint32_t tx = 1u << (USBD_IRQ_EP1_TX + (ep - 1) * 2);
        uint32_t rx = 1u << (USBD_IRQ_EP1_RX + (ep - 1) * 2);

        if (p & tx)
        {
            hal_irq_clear(tx);

            if (bulk_tx[ep - 1].cb)
                bulk_tx[ep - 1].cb(ep, 0, bulk_tx[ep - 1].arg);
        }

        if (p & rx)
        {
            hal_irq_clear(rx);

            if (bulk_rx[ep - 1].cb)
                bulk_rx[ep - 1].cb(ep, hal_ep_rx_len(ep, 0), bulk_rx[ep - 1].arg);
        }
    }
}
