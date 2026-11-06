/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* ConBee I / RaspBee I product identity for an image built here.

One image serves both products, as the vendor's does (docs/hardware_avr.md §11.8): the two differ in what
carries the UART to the host - an FTDI bridge on the stick, the Pi's header on the module - and not in
the firmware. The NCP version names the radio, as the ConBee II's names its AT86RF233. */
#ifndef PRODUCT_H
#define PRODUCT_H

#include "phoscon_version.h"

#define PRODUCT_NAME "ConBee I / RaspBee I"
#define PRODUCT_NCP_VERSION "DSPINEL/1.0; ATmega256RFR2; phoscon-rcp " PHOSCON_RCP_VERSION

/* The host link's line rate, from the build (src/CMakeLists.txt DS_LINK_BAUD). */
#ifndef PRODUCT_LINK_BAUD
#define PRODUCT_LINK_BAUD 38400u
#endif

/* What may wait for that link (rcp_app.c): a 127-octet frame takes about 30 ms to leave at 57 600, against a
 * neighbour's fragments every 5 ms. */
#define PRODUCT_TX_RING 4096u

#endif /* PRODUCT_H */
