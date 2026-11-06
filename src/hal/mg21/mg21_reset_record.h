/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mg21_reset_record.h — the boot record a ConBee III shares with its bootloader
 * (docs/hardware_mg21.md §10.7).
 *
 * The deCONZ bootloader on the EFR32MG21 decides what happens after a reset from one
 * record at the SRAM base, and an application that wants a particular outcome has to write
 * it by hand. A cold boot leaves SRAM zero, the signature is absent, and the bootloader
 * runs in full: ID packet, roughly the one-second CLI window, then the application. An image
 * that clears the record before a soft reset gets the same path, and that window is how it is
 * re-flashed over its own link; the "stay in the bootloader" reason is defined below, but on
 * the bench it left the stick silent (docs/hardware_mg21.md §10.7).
 *
 * The record is a fixed layout in uninitialised SRAM, so it survives a soft reset and is
 * lost on a power cycle. Reading it never needs the hardware, and writing it needs nothing
 * but the address, which is why the classification and the request are plain functions over
 * a caller-supplied buffer: they carry a host suite (docs/testing.md §2) and the target code
 * only supplies the address.
 */

#ifndef MG21_RESET_RECORD_H
#define MG21_RESET_RECORD_H

#include <stdint.h>

/* The record's home: the first bytes of SRAM, which startup has not yet cleared when the
 * bootloader reads it. */
#define MG21_RESET_RECORD_ADDR 0x20000000u

/* Number of 16-bit words the record occupies, including the reserved tail the bootloader
 * uses to hand its own version to the application. */
#define MG21_RESET_RECORD_WORDS 16u

/* What the bootloader looks for at word 1. Anything else — a cold boot, an unrelated reset —
 * classifies as "no record". */
#define MG21_RESET_SIGNATURE 0xF00Fu

/* Reason codes, as the shipped bootloader uses them (docs/hardware_mg21.md §10.7). The two install
 * codes are not distinguished here because this tree never stages an image: it hands the
 * board to the bootloader and lets the host tool do the writing. */
#define MG21_RESET_REASON_NONE 0x0200u    /* no valid record */
#define MG21_RESET_REASON_GO 0x0201u      /* the bootloader is starting the application */
#define MG21_RESET_REASON_BADIMAGE 0x0203u /* the staged image was rejected, application runs */
#define MG21_RESET_REASON_FORCE 0x0205u   /* stay in the bootloader and wait for the link */
#define MG21_RESET_REASON_BADAPP 0x0208u  /* no valid application to run */
#define MG21_RESET_REASON_UPGRADE 0x0209u /* install what was staged in the ImageSector */

/* Word offsets of the record's own fields, as the bootloader's `HalBootParamType` lays them
 * out: reason and signature, then the network state the last run left behind. */
#define MG21_RESET_WORD_REASON 0u
#define MG21_RESET_WORD_SIGNATURE 1u
#define MG21_RESET_WORD_PAN_ID 2u
#define MG21_RESET_BYTE_CHANNEL 6u  /* the radio channel minus 11 */
#define MG21_RESET_BYTE_TX_POWER 7u
#define MG21_RESET_BYTE_LNA_CAL 8u
#define MG21_RESET_BYTE_BTL_VERSION 9u /* 0x1D, then the u16 major/minor, then a trailing byte */

/* The reason the bootloader recorded, or MG21_RESET_REASON_NONE when the record is not
 * there. `words` points at MG21_RESET_RECORD_WORDS 16-bit words. */
uint16_t ResetRecord_Reason(const uint16_t *words);

/* 1 when the record carries the signature, so its reason is meaningful. */
int ResetRecord_IsValid(const uint16_t *words);

/* Record "stay in the bootloader and wait for the link", leaving the rest of the record
 * alone. The image does not use it: on the bench a reset with this record left the stick
 * silent, answering no ID request, until a power cycle (docs/hardware_mg21.md §10.7). The way an image
 * here is re-flashed over its own link is ResetRecord_Clear plus a reset, which opens the
 * bootloader's window (hal/mg21/hal_mg21.c HAL_SystemReset). */
void ResetRecord_RequestDfu(uint16_t *words);

/* Clear the record to "no record": the next reset takes the full bootloader path. */
void ResetRecord_Clear(uint16_t *words);

#endif /* MG21_RESET_RECORD_H */
