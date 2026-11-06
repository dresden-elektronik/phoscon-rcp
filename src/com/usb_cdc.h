/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* usb_cdc.h — the ConBee II native USB CDC link's own diagnostics. They are
 * board diagnostics, not pipe semantics, so they sit with the implementation
 * (com/usb_cdc.c) rather than in the portable host-link contract (hal/com.h,
 * docs/boards.md §4). Everything the app calls is COM_*; a link without these
 * is free to omit the header. */
#ifndef USB_CDC_H
#define USB_CDC_H

#include <stdint.h>

/* Publish the USBC registers, the host SOF count and the frame number
 * (hal_diag.h). */
uint32_t COM_SofFrames(void);
uint32_t COM_Frame(void);

/* Arm one CDC ACM notification (CDC120 Table 22 framing) if configured and a
 * link state change waits. Returns the byte count armed, 0 when nothing was
 * armable. Main loop only. */

#endif
