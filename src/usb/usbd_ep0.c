/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
#include "usbd_int.h"

#define EP0_MPS 64

static ep0_state_t st;
static struct
{
    const uint8_t *src;
    uint8_t *dst;
    uint16_t total;
    uint16_t sent;
    uint16_t hostLen; /* the request's wLength: what the host will take */
    int zlp;
} x;
static uint8_t next_tg[2];
static void (*commit_fn)(void);

/* No <string.h> on target (coding-rules §1). */
static void ep0_copy(uint8_t *dst, const uint8_t *src, uint16_t n)
{
    uint16_t i;

    for (i = 0; i < n; i++)
        dst[i] = src[i];
}

static void arm_in(uint16_t n)
{
    if (n)
        ep0_copy(hal_ep_buf(0, 1, USBD_BANK_IN), x.src + x.sent, n);

    hal_ep_arm(0, 1, USBD_BANK_IN, n);
    x.sent += n;
    next_tg[0] ^= 1;
}

static void arm_out(void)
{
    uint16_t rem = x.total - x.sent;
    hal_ep_arm(0, 0, USBD_BANK_OUT, rem > EP0_MPS ? EP0_MPS : rem);
    next_tg[1] ^= 1;
}

static void run_commit(void)
{
    void (*f)(void) = commit_fn;
    commit_fn = 0;

    if (f)
        f();
}

void usbd_ep0_init(int fresh)
{
    st = EP0_IDLE;
    x.total = x.sent = 0;
    x.hostLen = 0;
    x.zlp = 0;

    if (fresh)
    {
        /* A fresh EP0 (reset/init) drops any pending commit. */
        commit_fn = 0;
        hal_ep_reset_toggle(0, 1);
        hal_ep_reset_toggle(0, 0);
        next_tg[0] = 0;
        next_tg[1] = 0;
    }
    else
    {
        next_tg[0] = hal_ep_toggle(0, 1);
        next_tg[1] = hal_ep_toggle(0, 0);
    }

    hal_ep_clear_stall(0, 1);
    hal_ep_clear_stall(0, 0);
    hal_ep_enable(0, 1, 0);
    hal_ep_enable(0, 0, 0);
}

void usbd_ep0_recover(void)
{
    hal_ep_clear_stall(0, 1);
    hal_ep_clear_stall(0, 0);
    hal_ep_disarm(0, 1, 0);
    hal_ep_disarm(0, 1, 1);
    hal_ep_disarm(0, 0, 0);
    st = EP0_IDLE;
}

void usbd_ep0_reject(void)
{
    st = EP0_STALLED;
    hal_ep_stall(0, 1);
    hal_ep_clear_stall(0, 0);
}

void usbd_ep0_set_commit(void (*fn)(void))
{
    commit_fn = fn;
}

uint8_t usbd_ep0_state(void)
{
    return st;
}

uint8_t usbd_ep0_toggle(int in)
{
    return next_tg[in ? 0 : 1];
}

void usbd_ep0_setup(const uint8_t *s8)
{
    uint16_t wl;

    hal_ep_clear_stall(0, 1);
    /* SETUP leaves the EP0 OUT bank full (BK0RDY set); re-arm it so the status
     * OUT ZLP of a device-to-host request ACKs. */
    hal_ep_arm(0, 0, USBD_BANK_OUT, 0);

    if (st == EP0_STALLED)
        st = EP0_IDLE;

    x.total = x.sent = 0;
    x.hostLen = 0;
    x.zlp = 0;
    commit_fn = 0;

    if (!usbd_req_dispatch(s8))
    {
        usbd_ep0_reject();
        return;
    }

    wl = (uint16_t)s8[6] | ((uint16_t)s8[7] << 8);

    if (!(s8[0] & 0x80) && wl == 0)
    {
        /* Host-to-device, no data stage: arm the status IN ZLP; a commit
         * (e.g. SET_ADDRESS) fires only on status completion (USB 2.0 9.4.6). */
        arm_in(0);
        st = EP0_STATUS;
    }
}

/* `total` is clamped to wLength by the caller, `hostLen` is that unclamped
 * wLength. A terminating ZLP is needed only when the payload ends before
 * wLength on a packet boundary (USB 2.0 5.3.2). */
void usbd_ep0_begin_in(const uint8_t *data, uint16_t total, uint16_t hostLen)
{
    x.src = data;
    x.dst = 0;
    x.total = total;
    x.hostLen = hostLen;
    st = EP0_DATA_IN;
    arm_in(total > EP0_MPS ? EP0_MPS : total);
}

void usbd_ep0_begin_out(uint8_t *dst, uint16_t total)
{
    x.src = 0;
    x.dst = dst;
    x.total = total;
    st = EP0_DATA_OUT;
    arm_out();
}

void usbd_ep0_zero_in(void)
{
    x.total = x.sent = 0;
    x.hostLen = 0;
    st = EP0_STATUS;
    arm_in(0);
}

void usbd_ep0_tx(void)
{
    if (st == EP0_STATUS)
    {
        st = EP0_IDLE;
        run_commit();
        return;
    }

    if (st != EP0_DATA_IN)
        return;

    if (x.sent < x.total)
    {
        uint16_t rem = x.total - x.sent;
        arm_in(rem > EP0_MPS ? EP0_MPS : rem);
        return;
    }

    if (x.total % EP0_MPS == 0 && !x.zlp && x.total < x.hostLen)
    {
        x.zlp = 1;
        arm_in(0);
        return;
    }
    /* Device-to-host request: the status stage is a status OUT ZLP from the
     * host, not an IN packet. */
    st = EP0_STATUS;
}

void usbd_ep0_rx(void)
{
    uint16_t rem;
    uint16_t n;

    if (st == EP0_STATUS)
    {
        /* Status OUT ZLP of a device-to-host request completed. */
        st = EP0_IDLE;
        run_commit();
        return;
    }

    if (st != EP0_DATA_OUT)
        return;

    rem = x.total - x.sent;
    n = rem > EP0_MPS ? EP0_MPS : rem;
    ep0_copy(x.dst + x.sent, hal_ep_buf(0, 0, USBD_BANK_OUT), n);
    x.sent += n;
    hal_ep_disarm(0, 0, USBD_BANK_OUT);

    if (x.sent < x.total)
    {
        arm_out();
        return;
    }
    /* Host-to-device request with a data stage: send the status IN ZLP. */
    st = EP0_STATUS;
    arm_in(0);
}
