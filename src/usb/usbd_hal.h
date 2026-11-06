/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* USB device target bridge — the frozen HAL contract the driver calls.
 * Adopted from deconz-cb2-fw, implemented for our silicon in
 * hal/samr21/samr21_usbd_hal.c. */
#ifndef USBD_HAL_H
#define USBD_HAL_H

#include <stdint.h>

#define USBD_MAX_EP 3

/* Single-bank silicon: IN uses the bank-1 descriptor fields
 * (BK1RDY/ADDR1/PCKSIZE1), OUT and SETUP bank 0 (datasheet 29.6.2, Table
 * 29-15/29-16). */
#define USBD_BANK_OUT 0
#define USBD_BANK_IN 1

typedef enum
{
    USBD_IRQ_EP0_RXSTP = 0,
    USBD_IRQ_EP0_TX,
    USBD_IRQ_EP0_RX,
    USBD_IRQ_EP1_TX,
    USBD_IRQ_EP1_RX,
    USBD_IRQ_EP2_TX,
    USBD_IRQ_EP2_RX,
    USBD_IRQ_EP3_TX,
    USBD_IRQ_EP3_RX,
    USBD_IRQ_RST,
    USBD_IRQ_SUSP,
    USBD_IRQ_WAKEUP,
    USBD_IRQ_COUNT
} usbd_irq_t;

#define USBD_IRQ_ALL ((1u << USBD_IRQ_COUNT) - 1u)

/* Bring-up order: hal_usb_start() (clock, pads, fuse calibration, QOS, DESCADD,
 * clean NVIC), then usbd_init() (hal_usb_swrst(), hal_usb_enable(1), EP0 setup,
 * hal_usb_attach(1)). */
void hal_usb_start(void);
void hal_usb_swrst(void);
void hal_usb_enable(int on);
int hal_usb_enabled(void);
void hal_usb_attach(int on);

uint8_t hal_usb_get_addr(void);
/* Host SOF frames acknowledged since boot (the flag is taken off INTFLAG in
 * hal_irq_pending, so that is where the count is honest). */
uint32_t hal_usb_sofFrames(void);
uint32_t hal_usb_frame(void);
void hal_usb_witnesses(void);
void hal_usb_set_addr(uint8_t addr);

void hal_ep_enable(uint8_t ep, int in, int stall);
void hal_ep_disable(uint8_t ep, int in);
void *hal_ep_buf(uint8_t ep, int in, uint8_t bank);
void hal_ep_arm(uint8_t ep, int in, uint8_t bank, uint16_t len);
void hal_ep_disarm(uint8_t ep, int in, uint8_t bank);
void hal_ep_stall(uint8_t ep, int in);
void hal_ep_clear_stall(uint8_t ep, int in);
int hal_ep_stalled(uint8_t ep, int in);
int hal_ep_armed(uint8_t ep, int in, uint8_t bank);
uint8_t hal_ep_toggle(uint8_t ep, int in);
void hal_ep_reset_toggle(uint8_t ep, int in);
uint16_t hal_ep_rx_len(uint8_t ep, int in);

uint32_t hal_irq_pending(void);
void hal_irq_clear(uint32_t mask);

/* The incoming SETUP is latched into the EP0 OUT buffer only while
 * EPINTFLAG.RXSTP == 0 (datasheet 29.6.2.6): copy the 8 bytes before clearing
 * USBD_IRQ_EP0_RXSTP, else a back-to-back SETUP overwrites the buffer. */
void hal_usb_setup_read(uint8_t *buf8);
void hal_dfll_ready(void);

#endif
