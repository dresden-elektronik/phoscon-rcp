/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* Seeed Studio XIAO ESP32-C6 identity for an image built here: an experimental target, not a product of
the family (docs/boards.md §5). The NCP version names the radio, as on every board. */
#ifndef PRODUCT_H
#define PRODUCT_H

#include "phoscon_version.h"

#define PRODUCT_NAME "XIAO ESP32-C6"
#define PRODUCT_NCP_VERSION "DSPINEL/1.0; ESP32-C6; phoscon-rcp " PHOSCON_RCP_VERSION

/* The host link is the chip's USB Serial/JTAG controller: a CDC-ACM port whose line rate is the bus's. */
#ifndef PRODUCT_LINK_BAUD
#define PRODUCT_LINK_BAUD 0u
#endif

/* What may wait for that link: rcp_app.c. */
#define PRODUCT_TX_RING 4096u

#endif /* PRODUCT_H */
