/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mg21_reset_record.c — the boot record's rules, written so they can be checked off the
 * target. Every function here works on a caller-supplied buffer; the only place the SRAM
 * address appears is the caller that supplies it (hal/mg21/hal_mg21.c).
 *
 * The rules are the shipped bootloader's, read out of the ELF that built it
 * (docs/hardware_mg21.md §10.7): a record counts only if word 1 is the signature, its reason is word
 * 0, and the reason decides whether the bootloader hands the board to the application, stays
 * and waits on the link, or installs a staged image. */
#include <stdint.h>

#include "mg21_reset_record.h"

uint16_t ResetRecord_Reason(const uint16_t *words)
{
    if (words[MG21_RESET_WORD_SIGNATURE] != MG21_RESET_SIGNATURE) {
        return MG21_RESET_REASON_NONE;
    }
    return words[MG21_RESET_WORD_REASON];
}

int ResetRecord_IsValid(const uint16_t *words)
{
    return (words[MG21_RESET_WORD_SIGNATURE] == MG21_RESET_SIGNATURE) ? 1 : 0;
}

void ResetRecord_RequestDfu(uint16_t *words)
{
    words[MG21_RESET_WORD_REASON] = MG21_RESET_REASON_FORCE;
    words[MG21_RESET_WORD_SIGNATURE] = MG21_RESET_SIGNATURE;
}

/* Through a volatile view: the record is memory the bootloader reads after the reset, and a plain
 * zeroing loop is one the compiler may turn into a memset call this freestanding image does not
 * have. */
void ResetRecord_Clear(uint16_t *words)
{
    volatile uint16_t *w = words;

    for (uint16_t i = 0u; i < MG21_RESET_RECORD_WORDS; i++) {
        w[i] = 0u;
    }
}
