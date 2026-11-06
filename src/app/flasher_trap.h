/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* flasher_trap.h - the host flasher's request to reset into the bootloader.
 *
 * GCFFlasher cannot reset this board: there is no reset line and no button, and while the app runs it
 * owns the USB device, so no bootloader is listening. Before programming, the flasher therefore sends
 * a fixed 16-byte GCF WRITE_PARAMETER frame on the same OUT byte stream the application uses, and the
 * running image resets itself so the bootloader's window opens.
 *
 * The frame is matched by content rather than parsed, because that stream also carries records or
 * HDLC-framed Spinel and the frame may arrive split across USB packets. Feed() runs in the link
 * interrupt and only touches bytes and a deadline; the reset itself is the main loop's, so it happens
 * after the transfer completes instead of during it. Where the bootloader itself handles that
 * WRITE_PARAMETER frame, the flasher has a second, independent way in - it can arm this app's
 * watchdog - for when the app cannot run this code at all. The ConBee III's de_BTL bootloader has no
 * such handler, so on that board the trap above is the only way in (docs/build-and-flash.md).
 */
#ifndef APP_FLASHER_TRAP_H
#define APP_FLASHER_TRAP_H

#include <stdint.h>

/* The frame, plus a frame's worth behind it, so a frame that straddles packets is still contiguous. */
#define FLASHER_TRAP_TAIL 32u

/* Deferred by this much so the reset lands while the flasher is already waiting for the window. */
#define FLASHER_TRAP_DELAY_MS 300u

/* The reply a vendor firmware gives the frame: a WRITE_PARAMETER response for parameter 0x26, with the
 * request's sequence number, status 0 and the frame's two-byte checksum, SLIP-framed like the request.
 * GCFFlasher waits for it, or for the device to disconnect, before it looks for a bootloader; a link
 * behind an FTDI bridge never disconnects, so on the ConBee I and RaspBee I an image that does not
 * answer leaves the flasher waiting out its timeout while the bootloader's half-second window passes
 * (docs/build-and-flash.md §11). A board that needs it sends it once, from the main loop, before the
 * trap is due. */
#define FLASHER_TRAP_ACK_LEN 12u
const uint8_t *FlasherTrap_Ack(void);

/* Feed OUT bytes, from the link interrupt. Returns 1 when they complete the frame and the trap armed. */
int FlasherTrap_Feed(const uint8_t *buf, uint16_t len);

/* Returns 1 once, when an armed trap is due at `ms`; the caller resets the board. */
int FlasherTrap_Due(uint32_t ms);

#endif
