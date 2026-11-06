/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mg21_radio_off.c — switching the radio subsystem off again, for both radio back-ends (mg21_radio.h).
 *
 * Register-level only: it neither knows which back-end ran the radio nor leaves any state behind. The
 * back-end stops its own activity first (the from-scratch route has nothing to stop; the RAIL route
 * aborts RAIL's receive and withdraws its interrupts), then calls this from the reset path with
 * interrupts masked. */
#include <stdint.h>

#include "mg21_radio.h"
#include "mg21_regs.h"

/* Bounded wait for the RAC to reach OFF, in polls of RAC.STATUS: a power-down measures 5 us, so this is
 * far longer than any measured transition and still finite. */
#define RADIO_OFF_WAIT_LIMIT 0x4000u

#define RAC(off) MG21_RADIO_REG(MG21_RAC_BASE, (off))

void Mg21Radio_Off(void)
{
    static const uint32_t blocks[] = {MG21_FRC_BASE, MG21_MODEM_BASE, MG21_SYNTH_BASE,
                                      MG21_AGC_BASE, MG21_PROTIMER_BASE, MG21_RFCRC_BASE,
                                      MG21_BUFC_BASE, MG21_RAC_BASE};
    uint32_t n;

    if ((MG21_CMU->RADIOCLKCTRL & MG21_CMU_RADIOCLKCTRL_EN) == 0u)
        return;

    RAC(MG21_RAC_RXENSRCEN_OFFSET) = 0u;
    RAC(MG21_RADIO_SET_ALIAS + MG21_RAC_CTRL_OFFSET) = MG21_RAC_CTRL_FORCEDISABLE;

    for (n = 0u; n < RADIO_OFF_WAIT_LIMIT; ++n)
    {
        if (((RAC(MG21_RAC_STATUS_OFFSET) & MG21_RAC_STATUS_STATE_MASK) >> MG21_RAC_STATUS_STATE_SHIFT) ==
            MG21_RAC_STATUS_STATE_OFF)
            break;
    }

    /* RAC last: the other blocks are disabled while the controller that sequences them is still on. */
    for (n = 0u; n < (uint32_t)(sizeof(blocks) / sizeof(blocks[0])); ++n)
        MG21_RADIO_REG(blocks[n], MG21_RADIO_EN_OFFSET) = 0u;

    /* Configuration registers, written while RAC is disabled (docs/hardware_mg21.md §10.5). */
    RAC(MG21_RAC_VECTADDR_OFFSET) = 0u;
    RAC(MG21_RAC_SEQCTRL_OFFSET) = 0u;

    MG21_CMU->LOCK = MG21_CMU_LOCKKEY_UNLOCK;
    MG21_CMU->RADIOCLKCTRL = 0u;
}
