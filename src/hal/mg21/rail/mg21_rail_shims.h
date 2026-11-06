/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mg21_rail_shims.h — what hal/mg21/rail/rail_shims.c offers the RAIL adapter beside the symbols the
 * archive asks for (docs/hardware_mg21.md §10.6). */
#ifndef MG21_RAIL_SHIMS_H
#define MG21_RAIL_SHIMS_H

/* From now on RAIL's critical sections no longer unmask interrupts when they close. The reset path
 * (Mg21Radio_Release) calls into RAIL with interrupts already masked and has to leave them masked;
 * there is no way to read the mask back, so the section nesting that RAIL's CORE_* calls are counted
 * with ends masked instead. */
void Mg21RailShim_StayMasked(void);

/* RAIL's nesting critical section, for the rest of the back-end. */
uint32_t CORE_EnterAtomic(void);
void CORE_ExitAtomic(uint32_t depth);

#endif /* MG21_RAIL_SHIMS_H */
