/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* RaspBee II product identity and board constants. The serial is read from the NVM
 * user row (hal.h Nvram_SerialAscii), not hardcoded. */
#ifndef PRODUCT_H
#define PRODUCT_H

#include "phoscon_version.h"

/* No USB on this board's link, so no IDs on the wire: the descriptor carries 0 and
 * the identity that matters is the name and the NCP version (board/board.h). */
#define PRODUCT_VID 0
#define PRODUCT_PID 0

#define PRODUCT_NAME "RaspBee II"

/* NCP_VERSION on the wire (docs/protocol.md §10); the radio is the same AT86RF233 the
 * ConBee II reports, so the version string is theirs alike, with the release. Deliberately no build
 * date. */
#define PRODUCT_NCP_VERSION "DSPINEL/1.0; AT86RF233; phoscon-rcp " PHOSCON_RCP_VERSION

/* The board's build passes the line rate in (board.cmake DS_LINK_BAUD); this is the fallback, the
 * rate the bootloader and the vendor flasher speak (docs/hardware_samr21.md §9). The rate is the
 * board's, not the driver's (board/board.h linkBaud). */
#ifndef PRODUCT_LINK_BAUD
#define PRODUCT_LINK_BAUD 38400u
#endif

/* The link's octets cannot wait out a frame upload with interrupts masked (hal/samr21/samr21_radio.c). */
#define PRODUCT_TX_UPLOAD_UNMASKED 1

/* What may wait for that link: rcp_app.c. A Thread host's fragments fill it. */
#define PRODUCT_TX_RING 4096u

/* GCF packaging: key 30 = RaspBee II, against 0 for the ConBee II - the one difference
 * in the two products' containers, measured in the vendor's own release
 * (docs/hardware_samr21.md §9). */
#define PRODUCT_GCF_KEY 30

/* RTC on this board is a PCF85063 on the host's I2C bus, driven by the host: nothing to
 * blink and nothing to clock here (docs/hardware_samr21.md §9). */

#endif
