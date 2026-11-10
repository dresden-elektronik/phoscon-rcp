/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* esp32_phy.h — the RF bring-up of an ESP32 802.15.4 part, on Espressif's PHY libraries
 * (docs/hardware_esp32.md §12.5).
 *
 * The libraries are fetched at configure time and never enter the repository (tools/fetch_esp_phy.py,
 * src/lib/PROVENANCE.md); this header is this tree's own, and only esp32_phy.c and the shims call into
 * them. */
#ifndef ESP32_PHY_H
#define ESP32_PHY_H

#include <stdint.h>

/* Open the modem clocks the PHY and the 802.15.4 MAC need, tell the ROM the CPU clock its delays count
 * in, then bring the PHY up with a full calibration and the baseband after it. Thread context, with
 * interrupts open: the calibration takes tens of milliseconds. Its result and durations are records.
 * 1 = the PHY came up: 0, or the calibration-check failure every boot of this firmware reports. */
int Esp32Phy_Init(void);

/* One round of the PLL tracking ESP-IDF repeats every second from a timer for its Bluetooth and 802.15.4
 * side (esp_phy's phy_track_pll). Thread context, after Esp32Phy_Init came up. */
void Esp32Phy_Track(void);

#endif /* ESP32_PHY_H */
