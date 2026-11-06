/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mg21_ident.c — the factory EUI-64 out of its manufacturing token (mg21_ident.h). */
#include "mg21_ident.h"

int Mg21Ident_Eui64(const uint8_t token[8], uint8_t eui8[8])
{
    uint8_t all = 0xFFu;
    uint8_t any = 0x00u;
    uint8_t i;

    for (i = 0u; i < MG21_IDENT_EUI64_LEN; ++i)
    {
        all = (uint8_t)(all & token[i]);
        any = (uint8_t)(any | token[i]);
    }

    /* The I/G bit is bit 0 of the first octet in display order, the token's last byte. */
    if (all == 0xFFu || any == 0x00u || (token[MG21_IDENT_EUI64_LEN - 1u] & 0x01u) != 0u)
        return 0;

    for (i = 0u; i < MG21_IDENT_EUI64_LEN; ++i)
        eui8[i] = token[MG21_IDENT_EUI64_LEN - 1u - i];

    return 1;
}
