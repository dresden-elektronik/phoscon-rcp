/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mg21_nvram.c — the ConBee III's device identity, read from the user data page.

The stick's identity is not in the application image: the EUI-64 lives in the user data page at
0x0FE00000, outside the main flash array, as Silicon Labs' manufacturing token TOKEN_MFG_CUSTOM_EUI_64
(docs/hardware_mg21.md §10.2). The page is read with ordinary loads, as the SDK's token reader and the
bootloader read it; writing it takes the SE's user data command, and nothing here writes it.
mg21_ident.c turns the token into an address or into "none".

What the core does with 0 is defined, not assumed: app/rcp_app.c falls back to a host-assigned
address when Nvram_FactoryEui64() reports nothing programmed (docs/protocol.md §10). */
#include <stdint.h>

#include "dbg.h"
#include "hal.h"
#include "mg21_ident.h"
#include "mg21_regs.h"

static int identityRecorded;

/* The factory EUI-64, most significant byte first; 1 = an address. The token is copied byte by byte
 * through a volatile pointer: a plain copy loop becomes a memcpy to GCC, and a -nostdlib image has none. */
static int nvramEui64(uint8_t eui8[8])
{
    const volatile uint8_t *token = (const volatile uint8_t *)(MG21_USERDATA_BASE + MG21_IDENT_EUI64_OFFSET);
    uint8_t raw[MG21_IDENT_EUI64_LEN];
    uint8_t i;
    int ok;

    for (i = 0u; i < MG21_IDENT_EUI64_LEN; ++i)
        raw[i] = token[i];

    ok = Mg21Ident_Eui64(raw, eui8);

    /* Said once per boot: the answer is the same every time the core asks, and a record per question
     * would be noise in the queue the host reads. */
    if (!identityRecorded)
    {
        identityRecorded = 1;
        Debug_Emit(DBG_COMP_FLASH, (uint16_t)ok, DBG_FLASH_RSN_MG21_IDENTITY);
    }

    return ok;
}

/* The same address in deCONZ order, least significant byte first: the token's own order. */
int Nvram_MacValid(uint8_t *mac8)
{
    uint8_t eui[MG21_IDENT_EUI64_LEN];
    uint8_t i;

    if (mac8 == 0 || !nvramEui64(eui))
        return 0;

    for (i = 0u; i < MG21_IDENT_EUI64_LEN; ++i)
        mac8[i] = eui[MG21_IDENT_EUI64_LEN - 1u - i];

    return 1;
}

int Nvram_FactoryEui64(uint8_t *eui8)
{
    if (eui8 == 0)
        return 0;

    return nvramEui64(eui8);
}

/* Nvram_SerialAscii and Nvram_Uid are not implemented for this board and are not linked by it: both
 * are USB descriptor entries (usb/usb_desc.c), and the ConBee III's host link is a USART. If a
 * cb3 build ever reaches for one of them, the link error is the honest answer, and §10.2 is the
 * place its source is written down. */
