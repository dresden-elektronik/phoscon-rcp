/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mg21_phy.h — the ConBee III radio's arithmetic and record formats (docs/hardware_mg21.md §10.5).
 *
 * What the back-end computes rather than looks up: the synthesiser word for a channel, the 802.15.4
 * FCS the frame controller does not store, the layout of a received frame's record in the receive
 * buffer, and the energy reading of the AGC. Pure functions over values and caller buffers, no
 * registers: hal/mg21/own/mg21_radio.c reads the hardware and asks these. */
#ifndef MG21_PHY_H
#define MG21_PHY_H

#include <stdint.h>

#include "radio.h"

/* SYNTH.FREQ for an 802.15.4 2.4 GHz channel: round(f * 2^19 / f_xo) with f = 2405 + 5 (ch - 11) MHz
 * and this board's 38.4 MHz crystal, which reduces to round(f_MHz * 40960 / 3). 0 for a channel
 * outside 11..26. */
uint32_t Mg21Phy_SynthFreq(uint8_t channel);

/* The 802.15.4 FCS (ITU-T CRC-16, reflected, initial 0) over n bytes, as the PSDU carries it: low
 * octet first. */
uint16_t Mg21Phy_Fcs(const uint8_t *data, uint8_t n);

/* A receive record, as the frame controller writes it with the trailer this back-end configures:
 * the PHY length octet (which counts the FCS), the PSDU without its FCS, an RSSI octet (signed dBm),
 * a status octet (bit 7 CRC good, bits 6:0 a link quality), and a 4-octet timestamp. */
#define MG21_PHY_RECORD_OVERHEAD 5u /* bytes a record has beyond its PHY length */
#define MG21_PHY_LEN_MIN 3u         /* the shortest PHY length a frame can carry: one octet and the FCS */

/* The record's total size for a PHY length octet, or 0 when the octet is not a frame's length. */
uint16_t Mg21Phy_RecordBytes(uint8_t phyLen);

/* Fill len, psdu (with the FCS recomputed and appended), rssi, lqi and crcOk from one complete
 * record of n bytes. channel, timestamp and ackedFp are the caller's. 1 = a frame. */
int Mg21Phy_ParseRecord(const uint8_t *rec, uint16_t n, Radio_Frame *out);

/* AGC.RSSI as dBm, its integer part (a signed byte at bits 15:8). 1 = a reading; 0 when the AGC
 * reports none (-128). */
int Mg21Phy_RssiDbm(uint32_t agcRssi, int8_t *dbm);

#endif /* MG21_PHY_H */
