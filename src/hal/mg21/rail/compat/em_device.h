/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* em_device.h — the part identification RAIL's headers read, and nothing else.
 *
 * rail_features.h and rail_chip_specific.h select their per-family constants on the Series 2 macros
 * the SDK's device header defines. That header drags in the whole register map and CMSIS; this tree has
 * its own register map (hal/mg21/mg21_regs.h), so the macros RAIL asks for are stated here for the one
 * part this back-end targets: an EFR32xG21 (Series 2, configuration 1). Used only by the RAIL
 * adapter's translation unit (docs/hardware_mg21.md §10.6). */
#ifndef EM_DEVICE_H
#define EM_DEVICE_H

#include <stdint.h>

#define _SILICON_LABS_32B_SERIES_2
#define _SILICON_LABS_32B_SERIES_2_CONFIG 1
#define _SILICON_LABS_32B_SERIES_2_CONFIG_1
#define _SILICON_LABS_GECKO_INTERNAL_SDID_200
#define EFR32MG21A010F1024IM32 1

#endif /* EM_DEVICE_H */
