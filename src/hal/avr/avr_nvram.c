/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* avr_nvram.c — the ConBee I / RaspBee I device identity, read from the EEPROM.
 *
 * The factory EUI-64 lives in the EEPROM's NV basic set at 0x1FE0 (docs/hardware_avr.md §11.3), which
 * the vendor writes at production and the fuses keep across a chip erase. This file reads those 32
 * bytes and nothing else; avr_ident.c, a portable core under its host suite, turns them into an
 * address or into "none". Nothing here writes the EEPROM.
 *
 * What the core does with 0 is defined, not assumed: app/rcp_app.c falls back to a host-assigned
 * address when Nvram_FactoryEui64() reports nothing programmed (docs/protocol.md §10). */
#include <stdint.h>

#include "avr_ident.h"
#include "avr_regs.h"
#include "dbg.h"
#include "hal.h"

static uint8_t identityRecorded;

/* One EEPROM byte: wait out a write in progress (there is none: this image never writes), set the
 * address, strobe EERE; the byte is in EEDR on the next instruction and the CPU is halted for the read
 * (datasheet, EEPROM chapter). */
static uint8_t nvramEepromRead(uint16_t addr)
{
    while ((AVR_REG8(AVR_EECR) & AVR_BIT(AVR_EEPE)) != 0u)
    {
    }

    AVR_REG8(AVR_EEARH) = (uint8_t)(addr >> 8);
    AVR_REG8(AVR_EEARL) = (uint8_t)addr;
    AVR_REG8(AVR_EECR) = AVR_BIT(AVR_EERE);
    return AVR_REG8(AVR_EEDR);
}

static int nvramEui64(uint8_t eui8[8])
{
    uint8_t set[AVR_IDENT_BASIC_LEN];
    uint8_t i;
    int ok;

    for (i = 0u; i < AVR_IDENT_BASIC_LEN; ++i)
        set[i] = nvramEepromRead((uint16_t)(AVR_IDENT_BASIC_ADDR + i));

    ok = AvrIdent_Eui64(set, eui8);

    /* Said once per boot: the answer is the same every time the core asks. */
    if (identityRecorded == 0u)
    {
        identityRecorded = 1u;
        Debug_Emit(DBG_COMP_FLASH, (uint16_t)ok, DBG_FLASH_RSN_AVR_IDENTITY);
    }

    return ok;
}

/* The same address in deCONZ order, least significant byte first. */
int Nvram_MacValid(uint8_t *mac8)
{
    uint8_t eui[8];
    uint8_t i;

    if (mac8 == 0 || !nvramEui64(eui))
        return 0;

    for (i = 0u; i < 8u; ++i)
        mac8[i] = eui[7u - i];

    return 1;
}

int Nvram_FactoryEui64(uint8_t *eui8)
{
    if (eui8 == 0)
        return 0;

    return nvramEui64(eui8);
}

/* Nvram_SerialAscii, Nvram_Uid and Nvram_ClearUpdateFlag are not implemented for this board and are
 * not linked by it: the first two are USB descriptor entries and this board's link is a USART, and the
 * third is the SAMR21 bootloader's flag, which this bootloader does not have. A build that reaches for
 * one fails at the link, which is the honest answer. */
