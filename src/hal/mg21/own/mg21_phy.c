/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* mg21_phy.c — the ConBee III radio's arithmetic and record formats (mg21_phy.h). */
#include <stdint.h>

#include "mg21_phy.h"
#include "radio.h"

#define PHY_CHAN_MHZ_BASE 2405u
#define PHY_CHAN_MHZ_STEP 5u
/* 2^19 / 38.4 MHz in MHz units: 2^19 * 10 / 384 = 2^13 * 5 / 3, so the word is f_MHz * 40960 / 3. */
#define PHY_FREQ_NUM 40960u
#define PHY_FREQ_DEN 3u

#define PHY_STATUS_CRC_OK 0x80u
#define PHY_STATUS_LQI_MASK 0x7Fu
#define PHY_RSSI_NONE (-128)

uint32_t Mg21Phy_SynthFreq(uint8_t channel)
{
    uint32_t mhz;

    if (channel < RADIO_CHAN_MIN || channel > RADIO_CHAN_MAX)
        return 0u;

    mhz = PHY_CHAN_MHZ_BASE + PHY_CHAN_MHZ_STEP * (uint32_t)(channel - RADIO_CHAN_MIN);
    return (mhz * PHY_FREQ_NUM + PHY_FREQ_DEN / 2u) / PHY_FREQ_DEN;
}

uint16_t Mg21Phy_Fcs(const uint8_t *data, uint8_t n)
{
    uint16_t crc = 0u;
    uint8_t i;
    uint8_t b;

    for (i = 0u; i < n; ++i)
    {
        crc ^= data[i];

        for (b = 0u; b < 8u; ++b)
            crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0x8408u) : (uint16_t)(crc >> 1);
    }

    return crc;
}

uint16_t Mg21Phy_RecordBytes(uint8_t phyLen)
{
    if (phyLen < MG21_PHY_LEN_MIN || phyLen > RADIO_PSDU_MAX)
        return 0u;

    return (uint16_t)(phyLen + MG21_PHY_RECORD_OVERHEAD);
}

int Mg21Phy_ParseRecord(const uint8_t *rec, uint16_t n, Radio_Frame *out)
{
    uint8_t phyLen;
    uint8_t body;
    uint8_t status;
    uint16_t fcs;
    uint8_t i;

    if (rec == 0 || out == 0 || n == 0u)
        return 0;

    phyLen = rec[0];

    if (Mg21Phy_RecordBytes(phyLen) == 0u || n < Mg21Phy_RecordBytes(phyLen))
        return 0;

    body = (uint8_t)(phyLen - 2u);

    for (i = 0u; i < body; ++i)
        out->psdu[i] = rec[1u + i];

    /* The frame controller checks the FCS and does not store it; the host is handed the PSDU whole,
     * so the octets are recomputed - for a frame the hardware passed, they are the ones on the air. */
    fcs = Mg21Phy_Fcs(out->psdu, body);
    out->psdu[body] = (uint8_t)(fcs & 0xFFu);
    out->psdu[body + 1u] = (uint8_t)(fcs >> 8);
    out->len = phyLen;

    out->rssi = (int8_t)rec[1u + body];
    status = rec[2u + body];
    out->crcOk = (status & PHY_STATUS_CRC_OK) ? 1 : 0;
    out->lqi = (uint8_t)((status & PHY_STATUS_LQI_MASK) << 1);
    return 1;
}

int Mg21Phy_RssiDbm(uint32_t agcRssi, int8_t *dbm)
{
    int v = (int)((agcRssi >> 8) & 0xFFu);

    if (v >= 0x80)
        v -= 0x100;

    if (v == PHY_RSSI_NONE)
        return 0;

    if (dbm != 0)
        *dbm = (int8_t)v;

    return 1;
}
