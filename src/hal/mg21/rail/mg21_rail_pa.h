/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mg21_rail_pa.h — the EFR32xG21 transmit power curves, as arithmetic (docs/hardware_mg21.md §10.6).
 *
 * RAIL sets its power amplifier by a raw level, and the conversion from the dBm a host asks for is the
 * application's to provide; the SDK ships it as a plugin that needs its clock manager and its power
 * manager, so this back-end carries the conversion itself: the piecewise-linear curves the SDK
 * publishes for the xG21's 2.4 GHz amplifiers on a battery supply, and the lookup over them. Pure
 * functions over values, no registers and no RAIL, so the host suite pins them. */
#ifndef MG21_RAIL_PA_H
#define MG21_RAIL_PA_H

#include <stdint.h>

/* The amplifier a curve belongs to; the values are RAIL's RAIL_TX_POWER_MODE_2P4_HP/MP/LP. */
enum
{
    MG21_RAIL_PA_HP = 0, /* the 20 dBm amplifier */
    MG21_RAIL_PA_MP = 1, /* the 10 dBm amplifier */
    MG21_RAIL_PA_LP = 2  /* the 0 dBm amplifier */
};

/* The highest and lowest power a mode's curve covers, in deci-dBm; 0 for a mode without a curve. */
int16_t Mg21RailPa_MaxDeciDbm(int mode);
int16_t Mg21RailPa_MinDeciDbm(int mode);

/* The raw level for a power in deci-dBm: the highest level whose power does not exceed the request,
 * the request first clamped to the curve's range. 0 for a mode without a curve. */
uint8_t Mg21RailPa_Level(int mode, int16_t deciDbm);

#endif /* MG21_RAIL_PA_H */
