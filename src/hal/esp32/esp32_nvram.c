/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* esp32_nvram.c — an ESP32 board's device identity, read from the eFuse MAC (docs/hardware_esp32.md §12.2).

The factory MAC sits in eFuse block 1 and is read with plain loads from the controller's read registers;
nothing here writes an eFuse, which is irreversible. esp32_ident.c turns the two words into the EUI-64
ESP-IDF gives the 802.15.4 interface, or into "none". */
#include <stdint.h>

#include "dbg.h"
#include "esp32_ident.h"
#include "esp32_regs.h"
#include "hal.h"

static int identityRecorded;

static int nvramEui64(uint8_t eui8[8])
{
    uint32_t mac0 = ESP32_REG(ESP32_EFUSE_BASE, ESP32_EFUSE_MAC0);
    uint32_t mac1 = ESP32_REG(ESP32_EFUSE_BASE, ESP32_EFUSE_MAC1);
    int ok;

    ok = Esp32Ident_Eui64(mac0, mac1, eui8);

    /* Said once per boot, as on the other boards. */
    if (!identityRecorded)
    {
        identityRecorded = 1;
        Debug_Emit(DBG_COMP_FLASH, (uint16_t)ok, DBG_FLASH_RSN_ESP32_IDENTITY);
        Debug_Emit(DBG_COMP_WITNESS, (uint16_t)mac1, DBG_WITNESS_RSN_ESP32_MAC_HI);
        Debug_Emit(DBG_COMP_WITNESS, (uint16_t)(mac1 >> 16), DBG_WITNESS_RSN_ESP32_MAC_EXT);
    }

    return ok;
}

/* The same address least significant byte first, the deCONZ order hal.h asks for. */
int Nvram_MacValid(uint8_t *mac8)
{
    uint8_t eui[ESP32_IDENT_EUI64_LEN];
    uint8_t i;

    if (mac8 == 0 || !nvramEui64(eui))
        return 0;

    for (i = 0u; i < ESP32_IDENT_EUI64_LEN; ++i)
        mac8[i] = eui[ESP32_IDENT_EUI64_LEN - 1u - i];

    return 1;
}

int Nvram_FactoryEui64(uint8_t *eui8)
{
    if (eui8 == 0)
        return 0;

    return nvramEui64(eui8);
}
