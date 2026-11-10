/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* esp32_mac.h — the ESP32 family's 802.15.4 MAC, its register glue (docs/hardware_esp32.md §12.6).
 *
 * The beginning of the radio back-end behind hal/radio.h: reset and configuration, receive on a channel
 * into a DMA buffer, and the vector. For now the receive path reports every frame as records, the
 * bench check that the PHY and the MAC work together. Needs the PHY up first (hal/esp32/phy/). */
#ifndef ESP32_MAC_H
#define ESP32_MAC_H

#include <stdint.h>

/* Reset the MAC, configure it for promiscuous receive with no Ack, and route its interrupt. Once, in
 * bring-up, with interrupts locked or not yet needed. 1 = routed. */
int Esp32Mac_Init(void);

/* Stop whatever the MAC does and receive on `channel` (11..26). 0 = not a 2.4 GHz channel. */
int Esp32Mac_RxStart(uint8_t channel);

/* The live counters as witness records: frames, aborts, and the MAC's own error counters. */
void Esp32Mac_Witnesses(void);

#endif /* ESP32_MAC_H */
