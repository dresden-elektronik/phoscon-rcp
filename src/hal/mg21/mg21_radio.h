/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mg21_radio.h — what the ConBee III radio back-ends offer this board's glue, beside hal/radio.h.
 *
 * Two back-ends implement hal/radio.h and this header for the board, one per build (-DDS_MG21_RADIO,
 * docs/build-and-flash.md §10): rail/mg21_rail.c, on the vendor's RAIL library and the rcp image's
 * default, and own/mg21_radio.c, the from-scratch route. The probe image drives no radio and has only
 * this header's hand-back, from mg21_radio_none.c. The board's glue (hal_mg21.c) is written against
 * this header alone. */
#ifndef MG21_RADIO_H
#define MG21_RADIO_H

/* Hand the radio back before a reset, whatever drives it: the back-end stops its own activity, then
 * the radio subsystem is switched off (Mg21Radio_Off), so the bootloader that runs next finds it in
 * its reset state (docs/hardware_mg21.md §10.5).
 * Does nothing while the radio clock is off. Called with interrupts masked, from
 * hal/mg21/hal_mg21.c HAL_SystemReset. */
void Mg21Radio_Release(void);

/* The register-level half of the hand-back, shared by both back-ends (mg21_radio_off.c): every receive
 * source withdrawn, the state machine forced to OFF, every radio block disabled, the sequencer's vector
 * address and mode back to reset, and the radio clock gated. Does nothing while the radio clock is off,
 * because then every radio access faults. */
void Mg21Radio_Off(void);

#endif /* MG21_RADIO_H */
