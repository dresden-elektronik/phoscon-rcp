/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* test_mg21_phy.c — the ConBee III radio's arithmetic and record formats (hal/mg21/own/mg21_phy.h).

   The synthesiser words are the ones the vendor's own channel tune computes for each channel, run
   in an emulator (docs/hardware_mg21.md §10.5), so the closed form is held to sixteen independent
   answers, not to itself. The record is one the bench's receive buffer held, byte for byte, of a frame a
   ConBee II sent; its FCS is computed by tools/rcp_pair.py, an implementation that shares no code
   with this one. */

#include <stdint.h>

#include "mg21_phy.h"
#include "radio.h"
#include "tu.h"

/* SYNTH.FREQ per channel 11..26, as the emulated vendor tune writes it. */
static const uint32_t tuneFreq[16] = {
    0x01F50AABu, 0x01F61555u, 0x01F72000u, 0x01F82AABu, 0x01F93555u, 0x01FA4000u,
    0x01FB4AABu, 0x01FC5555u, 0x01FD6000u, 0x01FE6AABu, 0x01FF7555u, 0x02008000u,
    0x02018AABu, 0x02029555u, 0x0203A000u, 0x0204AAABu,
};

/* A record from the bench: PHY length 20, an 18-octet PSDU without its FCS, RSSI +9 dBm, status
 * 0xAC, and a timestamp of zeros (the radio timer is not configured). */
static const uint8_t benchRecord[25] = {
    0x14, 0x41, 0x88, 0x00, 0xE5, 0xBE, 0xFF, 0xFF, 0x01, 0x00, 0x63, 0x62, 0x33,
    0x2D, 0x72, 0x78, 0x2D, 0x25, 0x00, 0x09, 0xAC, 0x00, 0x00, 0x00, 0x00,
};

TU_TEST(synth_word_matches_the_vendor_tune_on_every_channel)
{
    uint8_t ch;

    for (ch = RADIO_CHAN_MIN; ch <= RADIO_CHAN_MAX; ++ch)
        TU_EQ(Mg21Phy_SynthFreq(ch), tuneFreq[ch - RADIO_CHAN_MIN]);

    TU_EQ(Mg21Phy_SynthFreq(10u), 0u);
    TU_EQ(Mg21Phy_SynthFreq(27u), 0u);
    TU_EQ(Mg21Phy_SynthFreq(0u), 0u);
}

TU_TEST(fcs_is_the_one_on_the_air)
{
    /* tools/rcp_pair.py fcs() over the bench frame's 18 octets gives a9 c9. */
    TU_EQ(Mg21Phy_Fcs(&benchRecord[1], 18u), 0xC9A9u);
    /* The CRC of nothing is the initial value. */
    TU_EQ(Mg21Phy_Fcs(benchRecord, 0u), 0u);
}

TU_TEST(a_bench_record_parses)
{
    Radio_Frame f;

    TU_EQ(Mg21Phy_RecordBytes(benchRecord[0]), 25u);
    TU_CHECK(Mg21Phy_ParseRecord(benchRecord, sizeof(benchRecord), &f));
    TU_EQ(f.len, 20u);
    TU_EQ(f.psdu[0], 0x41u);
    TU_EQ(f.psdu[2], 0x00u);
    TU_EQ(f.psdu[17], 0x00u);
    TU_EQ(f.psdu[18], 0xA9u);
    TU_EQ(f.psdu[19], 0xC9u);
    TU_EQ(f.rssi, 9);
    TU_EQ(f.crcOk, 1);
    TU_EQ(f.lqi, (0xACu & 0x7Fu) << 1);
}

TU_TEST(incomplete_and_impossible_records_are_refused)
{
    Radio_Frame f;
    uint8_t rec[140];
    unsigned i;

    /* One byte short of the whole record. */
    TU_CHECK(!Mg21Phy_ParseRecord(benchRecord, sizeof(benchRecord) - 1u, &f));
    TU_CHECK(!Mg21Phy_ParseRecord(benchRecord, 0u, &f));
    TU_CHECK(!Mg21Phy_ParseRecord(0, 25u, &f));
    TU_CHECK(!Mg21Phy_ParseRecord(benchRecord, 25u, 0));

    /* Lengths a frame cannot have: below one octet plus the FCS, and above the PHY's 127. */
    TU_EQ(Mg21Phy_RecordBytes(0u), 0u);
    TU_EQ(Mg21Phy_RecordBytes(2u), 0u);
    TU_EQ(Mg21Phy_RecordBytes(128u), 0u);
    TU_EQ(Mg21Phy_RecordBytes(255u), 0u);

    for (i = 0; i < sizeof(rec); ++i)
        rec[i] = 0u;

    rec[0] = 128u;
    TU_CHECK(!Mg21Phy_ParseRecord(rec, sizeof(rec), &f));

    /* The extremes that are frames: the shortest, and a full 127 that fills the PSDU exactly. */
    rec[0] = 3u;
    rec[1] = 0x02u;
    rec[2] = (uint8_t)-40;
    rec[3] = 0x00u;
    TU_EQ(Mg21Phy_RecordBytes(3u), 8u);
    TU_CHECK(Mg21Phy_ParseRecord(rec, 8u, &f));
    TU_EQ(f.len, 3u);
    TU_EQ(f.rssi, -40);
    TU_EQ(f.crcOk, 0);

    rec[0] = 127u;
    TU_EQ(Mg21Phy_RecordBytes(127u), 132u);
    TU_CHECK(Mg21Phy_ParseRecord(rec, 132u, &f));
    TU_EQ(f.len, 127u);
}

TU_TEST(agc_rssi_reads_as_dbm)
{
    int8_t dbm = 0;

    /* The integer part is a signed byte at bits 15:8; the quarters below it are dropped. */
    TU_CHECK(Mg21Phy_RssiDbm(0x0000B4C0u, &dbm));
    TU_EQ(dbm, -76);
    TU_CHECK(Mg21Phy_RssiDbm(0x000009C0u, &dbm));
    TU_EQ(dbm, 9);
    /* -128 is the AGC's "no reading". */
    dbm = 5;
    TU_CHECK(!Mg21Phy_RssiDbm(0x00008000u, &dbm));
    TU_EQ(dbm, 5);
}

static const Tu_Test tests[] = {
    {"synth_word_matches_the_vendor_tune_on_every_channel", synth_word_matches_the_vendor_tune_on_every_channel},
    {"fcs_is_the_one_on_the_air", fcs_is_the_one_on_the_air},
    {"a_bench_record_parses", a_bench_record_parses},
    {"incomplete_and_impossible_records_are_refused", incomplete_and_impossible_records_are_refused},
    {"agc_rssi_reads_as_dbm", agc_rssi_reads_as_dbm},
};

int main(void)
{
    Tu_Run("mg21_phy", tests, (int)(sizeof(tests) / sizeof(tests[0])));
    return 0;
}
