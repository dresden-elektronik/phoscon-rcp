/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* esp32_mac.h — the ESP32 family's 802.15.4 MAC, its register glue (docs/hardware_esp32.md §12.6).
 *
 * The beginning of the radio back-end behind hal/radio.h: reset and configuration, receive, transmit with
 * CCA and Ack, hardware Acks of received frames, energy detection, and the vector. For now every event is
 * reported as records, the bench check against a ConBee II. Needs the PHY up first (hal/esp32/phy/). */
#ifndef ESP32_MAC_H
#define ESP32_MAC_H

#include <stdint.h>

/* Reset the MAC, configure it for promiscuous receive with no Ack, and route its interrupt. Once, in
 * bring-up, with interrupts locked or not yet needed. 1 = routed. */
int Esp32Mac_Init(void);

/* Stop whatever the MAC does and receive on `channel` (11..26). 0 = not a 2.4 GHz channel. */
int Esp32Mac_RxStart(uint8_t channel);

/* Address filter 0: PAN ID and short address. filtered = 1 also leaves promiscuous mode and turns the
 * hardware Acks on, both ways; 0 receives everything and acknowledges nothing. */
void Esp32Mac_SetAddress(uint16_t panId, uint16_t shortAddr, int filtered);

/* Transmit power in dBm, -15..+20, snapped down to the MAC's 3 dB steps. */
void Esp32Mac_SetPower(int8_t dbm);

/* Send `len` octets of PSDU, without the FCS the MAC appends; cca = 1 for a clear-channel assessment
 * first. An Ack Request in the frame makes the MAC wait for the Ack. 0 = not receiving now, or too long. */
int Esp32Mac_Tx(const uint8_t *psdu, uint8_t len, int cca);

/* Measure the channel's energy over `symbols` symbols (16 us each). 0 = not receiving now. */
int Esp32Mac_EdStart(uint32_t symbols);

/* The live counters as witness records: frames, aborts, and the MAC's own error counters. */
void Esp32Mac_Witnesses(void);

#ifdef DS_ESP32_TX_PROBE
/* The transmit probe's address and power, then its step once per heartbeat. */
void Esp32Mac_ProbeInit(void);
void Esp32Mac_ProbeStep(void);
#endif

#endif /* ESP32_MAC_H */
