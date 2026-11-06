/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* ConBee II product identity and board constants. The serial is read from the NVM
 * user row (hal.h Nvram_SerialAscii), not hardcoded. */
#ifndef PRODUCT_H
#define PRODUCT_H

#include "phoscon_version.h"

#define PRODUCT_VID 0x1CF1
#define PRODUCT_PID 0x0030
#define PRODUCT_BCD_USB 0x0200
/* Our own bcdDevice makes our app distinguishable from the vendor app and the
 * bootloader on the bus (both report 0x0100); bump it when bring-up changes shape. */
#define PRODUCT_BCD_DEV 0x0101

#define PRODUCT_MANUF "dresden elektronik ingenieurtechnik GmbH"
/* iProduct matches the bootloader and the vendor app, so a host sees a ConBee II. */
#define PRODUCT_NAME "ConBee II"

/* NCP_VERSION on the wire (docs/protocol.md §10): the radio and the release, deliberately no build
 * date. */
#define PRODUCT_NCP_VERSION "DSPINEL/1.0; AT86RF233; phoscon-rcp " PHOSCON_RCP_VERSION

/* GCF packaging: the ConBee II container type (GCFTool -b cb2, docs/build-and-flash.md §4). */
#define PRODUCT_GCF_KEY 0

/* No LED on the CB2 PCB (footprint not populated) — nothing to blink. */

#endif
