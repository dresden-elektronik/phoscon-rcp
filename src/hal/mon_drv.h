/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mon_drv.h — what a board supplies to the bench register monitor (app/monitor_app.c).

The monitor's core (app/mon_proto.c) decides what a command may touch from the region table a board
publishes here; the board then performs the access. Only a board that sets DS_MONITOR implements
this, and only its probe image calls it (docs/hardware_mg21.md §10.5). */
#ifndef MON_DRV_H
#define MON_DRV_H

#include <stdint.h>

#include "mon_proto.h"

/* One trace observation: the masked value, and the cycles since the traced write. */
typedef struct
{
    uint32_t value;
    uint32_t cycles;
} Mon_Sample;

/* The regions this board allows, first match wins (mon_proto.h). */
const Mon_Region *MonDrv_Regions(uint16_t *count);

uint32_t MonDrv_Read32(uint32_t addr);

void MonDrv_Write32(uint32_t addr, uint32_t value);

/* Perform an accepted TRACE: write, then record the first masked value of the traced word and every
 * change of it, into out[0 .. cmd->count). Returns the number of samples; *timedOut is 1 when the
 * cycle budget ended the trace before the sample count did. */
uint8_t MonDrv_Trace(const Mon_Cmd *cmd, Mon_Sample *out, int *timedOut);

#endif /* MON_DRV_H */
