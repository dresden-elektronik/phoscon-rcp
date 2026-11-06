/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mg21_radio_none.c — the probe image's radio: it starts none, and only hands the subsystem back.
 *
 * A bench register write may have left the radio running, so a reset still switches it off. */
#include "mg21_radio.h"

void Mg21Radio_Release(void)
{
    Mg21Radio_Off();
}
