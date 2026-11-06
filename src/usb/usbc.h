/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* usbc.h — the ConBee II USB device API (hal/samr21/samr21_usb_device.c) over
 * the adopted usbd driver (usb/). The app reaches it through the host-link
 * contract COM_* (hal/com.h); the board glue and the USB device itself use
 * these symbols directly (docs/architecture.md §2).
 *
 * Context and ownership are part of the contract:
 *   - The receive handler is invoked from the USB IRQ, so it only queues.
 *   - A bulk-IN write is one packet; the buffer is copied, never retained.
 *   - The record queue's only consumer is Usbc_CdcInPoll. */
#ifndef USBC_H
#define USBC_H

#include <stdint.h>

/* Largest single CDC bulk-IN packet: the full-speed endpoint max packet size.
 * A caller with more to send splits it into packets; Usbc_CdcInSend refuses
 * more. */
#define USBC_TX_MAX 64u

/* Bring the device up: descriptors, USBC start, driver init, endpoint callbacks. */
void Usbc_Init(void);

/* ---- USB data (CDC bulk OUT) ---- */
/* Install the CDC data OUT (EP 0x02) callback; invoked from the USB IRQ, so keep
 * it light (no blocking, no WFI). Pass 0 to clear. */
typedef void (*Usbc_DataOutFn)(const uint8_t *buf, uint16_t len);
void Usbc_SetDataOutHandler(Usbc_DataOutFn fn);
/* Clear/set the D+ pull-up (29.8.2.1): the host's connect/disconnect edge. */
void Usbc_Attach(int on);
/* 1 once SET_CONFIGURATION has landed and the data endpoints are open. */
int Usbc_Configured(void);

/* ---- Debug records out (CDC bulk IN, EP 0x81) ---- */
/* Arm one bulk-IN packet from the debug queue (src/hal/dbg.c). Returns the byte
 * count armed (whole 8-byte records) or 0 if empty or a packet is outstanding.
 * Never waits or stalls; main loop only, and the queue's only consumer. */
uint16_t Usbc_CdcInPoll(void);

/* Arm one bulk-IN packet from a caller-owned buffer. 1 = the bank took it (bytes
 * copied), 0 = previous packet unacknowledged or over one packet. The buffer is
 * not retained. Main loop only, like Usbc_CdcInPoll. */
int Usbc_CdcInSend(const uint8_t *buf, uint16_t len);

/* Arm one CDC ACM notification (EP 0x83, interrupt IN) if configured and a link
 * state change is waiting. CDC120 Table 22 framing; NETWORK_CONNECTION today.
 * Main-loop context. Returns the byte count armed, 0 when nothing was armable. */
uint16_t Usbc_CdcNotifyPoll(void);

/* Called by the USBC bridge when it destroys an armed endpoint bank (bus reset
 * or unconfigure): whatever an IN bank held is gone unsent, and the CDC layer
 * owns that account. `ep` = endpoint number, `in` 1 = IN bank, 0 = OUT. */
void Usbc_EpBankLost(uint8_t ep, int in);

/* ---- USB diagnostics (hal_diag.h) ---- */
/* The USBC side: INTEN, INTFLAG, STATUS (speed), FNUM. */
void Usbc_Witnesses(void);
/* Host SOF frames acknowledged since boot (1/ms on an active full-speed bus) and
 * the module's own frame number; diagnostic builds only. */
uint32_t Usbc_SofFrames(void);
uint32_t Usbc_Frame(void);

#endif
