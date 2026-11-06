/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* test_mg21_reset_record.c — the ConBee III boot record's rules (docs/hardware_mg21.md §10.7).
 *
 * The deCONZ bootloader decides what a reset means from one record in SRAM, and an application
 * that wants to be re-flashed over its own link has to write that record by hand. The rules
 * are the shipped bootloader's, recovered from the ELF that built it: the signature is what
 * makes a record count, the reason is what the reason means, and the record is the only
 * channel between the two programs.
 *
 * The module is pure functions over a caller-supplied buffer, so the rules are checkable here;
 * the target code supplies the address and nothing else. */
#include <stdint.h>

#include "mg21_reset_record.h"
#include "tu.h"

static uint16_t rec[MG21_RESET_RECORD_WORDS];

static void stamp(uint16_t reason)
{
    rec[MG21_RESET_WORD_REASON] = reason;
    rec[MG21_RESET_WORD_SIGNATURE] = MG21_RESET_SIGNATURE;
}

TU_TEST(cold_boot_is_no_record)
{
    uint16_t i;

    for (i = 0u; i < MG21_RESET_RECORD_WORDS; i++)
        rec[i] = 0u;

    TU_EQ(ResetRecord_IsValid(rec), 0);
    /* Zero is a plausible-looking reason field; what decides it is the missing signature. */
    TU_EQ(ResetRecord_Reason(rec), MG21_RESET_REASON_NONE);
}

TU_TEST(signature_is_the_gate)
{
    stamp(MG21_RESET_REASON_GO);
    TU_EQ(ResetRecord_IsValid(rec), 1);
    TU_EQ(ResetRecord_Reason(rec), MG21_RESET_REASON_GO);

    rec[MG21_RESET_WORD_SIGNATURE] = MG21_RESET_SIGNATURE - 1u;
    TU_EQ(ResetRecord_IsValid(rec), 0);
    TU_EQ(ResetRecord_Reason(rec), MG21_RESET_REASON_NONE);

    /* A record left behind by something else is not ours to interpret either. */
    stamp(0xDE01u);
    TU_EQ(ResetRecord_Reason(rec), 0xDE01u);
    TU_EQ(ResetRecord_Reason(rec) == MG21_RESET_REASON_GO, 0);
}

TU_TEST(request_dfu_uses_the_stay_in_bootloader_reason)
{
    uint16_t i;

    for (i = 0u; i < MG21_RESET_RECORD_WORDS; i++)
        rec[i] = 0u;

    ResetRecord_RequestDfu(rec);
    TU_EQ(rec[MG21_RESET_WORD_REASON], MG21_RESET_REASON_FORCE);
    TU_EQ(rec[MG21_RESET_WORD_SIGNATURE], MG21_RESET_SIGNATURE);
    TU_EQ(ResetRecord_Reason(rec), MG21_RESET_REASON_FORCE);

    /* The network state the last run left in the record stays: the bootloader's own fast path
     * uses it, and a DFU request has no business changing it. */
    rec[MG21_RESET_WORD_PAN_ID] = 0x1234u;
    ((uint8_t *)rec)[MG21_RESET_BYTE_CHANNEL] = 11u;
    ((uint8_t *)rec)[MG21_RESET_BYTE_TX_POWER] = 100u;
    ResetRecord_RequestDfu(rec);
    TU_EQ(rec[MG21_RESET_WORD_PAN_ID], 0x1234u);
    TU_EQ(((uint8_t *)rec)[MG21_RESET_BYTE_CHANNEL], 11u);
    TU_EQ(((uint8_t *)rec)[MG21_RESET_BYTE_TX_POWER], 100u);

    /* And the reason the CLI uses after an upload is a different request: this tree writes the
     * stay-in-bootloader reason, never the install one. */
    TU_NE(MG21_RESET_REASON_FORCE, MG21_RESET_REASON_UPGRADE);
    TU_NE(MG21_RESET_REASON_FORCE, MG21_RESET_REASON_NONE);
}

TU_TEST(clear_is_not_a_record)
{
    stamp(MG21_RESET_REASON_UPGRADE);
    ResetRecord_Clear(rec);
    TU_EQ(ResetRecord_IsValid(rec), 0);
    TU_EQ(ResetRecord_Reason(rec), MG21_RESET_REASON_NONE);
}

TU_TEST(record_covers_the_bootloader_version_area)
{
    /* 16 words is 32 bytes: 8 bytes of reason/signature/network state plus the 22 reserved
     * bytes the bootloader writes its version into (docs/hardware_mg21.md §10.7). The byte offsets the
     * header names have to land inside that, or the module would touch past the record. */
    TU_EQ(MG21_RESET_RECORD_WORDS * 2u, 32u);
    TU_CHECK(MG21_RESET_BYTE_BTL_VERSION + 3u < MG21_RESET_RECORD_WORDS * 2u);
    TU_EQ(MG21_RESET_WORD_REASON, 0u);
    TU_EQ(MG21_RESET_WORD_SIGNATURE, 1u);
    TU_EQ(MG21_RESET_BYTE_CHANNEL, 6u);
    TU_EQ(MG21_RESET_BYTE_LNA_CAL, 8u);
}

static const Tu_Test tests[] = {
    {"cold_boot_is_no_record", cold_boot_is_no_record},
    {"signature_is_the_gate", signature_is_the_gate},
    {"request_dfu_uses_the_stay_in_bootloader_reason", request_dfu_uses_the_stay_in_bootloader_reason},
    {"clear_is_not_a_record", clear_is_not_a_record},
    {"record_covers_the_bootloader_version_area", record_covers_the_bootloader_version_area},
};

int main(void)
{
    Tu_Run("mg21_reset_record", tests, (int)(sizeof(tests) / sizeof(tests[0])));
    return 0;
}
