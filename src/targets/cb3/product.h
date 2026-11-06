/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* ConBee III product identity for an image built here.

The product identity of the stick itself lives in its user data page and its DeviceConfig (the
DeviceID 0xDEC0DE03 the bootloader matches a container against, the production serial, the EUI-64):
this file is the identity a firmware written here puts on the wire, next to the EUI-64 that
hal/mg21/mg21_nvram.c reads from the user data page (docs/hardware_mg21.md §10.2).

The NCP version names the radio and the release, as on every board (docs/protocol.md §10). */
#ifndef PRODUCT_H
#define PRODUCT_H

#include "phoscon_version.h"

#define PRODUCT_NAME "ConBee III"
#define PRODUCT_NCP_VERSION "DSPINEL/1.0; EFR32MG21; phoscon-rcp " PHOSCON_RCP_VERSION

/* GCF packaging: the ConBee III's nested container, outer type 60 with the inner AES-256-CBC image
 * (docs/build-and-flash.md §9, GCFTool -b cb3). */
#define PRODUCT_GCF_KEY 61

/* The host link's line rate, from the build (src/CMakeLists.txt DS_LINK_BAUD). */
#ifndef PRODUCT_LINK_BAUD
#define PRODUCT_LINK_BAUD 115200u
#endif

/* What may wait for that link: rcp_app.c. */
#define PRODUCT_TX_RING 4096u

#endif /* PRODUCT_H */
