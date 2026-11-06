/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* test_mac.c — the 802.15.4 header and Ack decision suite (docs/testing.md §2). The frame
 * vectors are the shapes that actually reach this radio: a PAN-compressed short Data Request
 * (the poll a sleepy child sends), the same with extended addresses, a data frame, and the
 * Ack the firmware composes. The filter cases mirror the two-stick rig's matrix, so a change
 * here that the rig would catch is caught in a second instead of after a flash. */
#include "mac.h"
#include "tu.h"

static uint16_t frame(uint8_t *buf, uint8_t mode, uint16_t pan)
{
    uint16_t fcf = (uint16_t)((uint16_t)mode << 10);

    buf[0] = (uint8_t)(fcf & 0xFFu);
    buf[1] = (uint8_t)(fcf >> 8);
    buf[2] = 0x42u;
    buf[3] = (uint8_t)(pan & 0xFFu);
    buf[4] = (uint8_t)(pan >> 8);
    return 5u;
}

/* A short-addressed Data Request as a polling child sends it: PAN-compressed, AR set,
 * version 1, destination 0x0001 on PAN 0xBEE5, source 0x0002, command 0x04. 12 octets. */
static const uint8_t pollShort[] = {0x63u, 0x98u, 0x00u, 0xE5u, 0xBEu,
                                    0x01u, 0x00u, 0x02u, 0x00u, 0x04u,
                                    0x00u, 0x00u};

/* The same request with extended addresses (FCF 0xDC63: dst ext, src ext). */
static const uint8_t pollExt[] = {0x63u, 0xDCu, 0x00u, 0xE5u, 0xBEu, 0xCCu, 0xBBu, 0xAAu,
                                  0x1Cu, 0x00u, 0x4Bu, 0x12u, 0x00u, 0x11u, 0x22u, 0x33u,
                                  0x44u, 0x55u, 0x66u, 0x77u, 0x88u, 0x04u, 0x00u, 0x00u};

/* A data frame to 0x0001 on the same PAN, AR set, short source (FCF 0x9861). */
static const uint8_t dataFrame[] = {0x61u, 0x98u, 0x07u, 0xE5u, 0xBEu, 0x01u, 0x00u,
                                    0x02u, 0x00u, 0xAAu, 0xBBu, 0x00u, 0x00u};

/* A data frame to a third address 0x0003: delivered by a promiscuous mode, dropped by the
 * filtered one. */
static const uint8_t dataThird[] = {0x61u, 0x98u, 0x08u, 0xE5u, 0xBEu, 0x03u, 0x00u,
                                    0x02u, 0x00u, 0xAAu, 0xBBu, 0x00u, 0x00u};

/* A data frame on a foreign PAN. */
static const uint8_t dataForeign[] = {0x61u, 0x98u, 0x09u, 0x11u, 0x22u, 0x01u, 0x00u,
                                      0x02u, 0x00u, 0xAAu, 0xBBu, 0x00u, 0x00u};

/* A broadcast data frame to 0xFFFF. */
static const uint8_t dataBroadcast[] = {0x61u, 0x98u, 0x0Au, 0xE5u, 0xBEu, 0xFFu, 0xFFu,
                                        0x02u, 0x00u, 0xAAu, 0xBBu, 0x00u, 0x00u};

/* An ACK: type 2, no addressing, sequence 0x00. */
static const uint8_t ackFrame[] = {0x02u, 0x00u, 0x00u, 0x00u, 0x00u};

/* A Beacon as one is actually sent: broadcast destination, our PAN as the source PAN, PAN
 * not compressed so that field is present (FCF 0x8800), AR clear. */
static const uint8_t beaconFrame[] = {0x00u, 0x88u, 0x01u, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
                                      0xE5u, 0xBEu, 0x00u, 0x00u};

static void cfgDefault(Mac_AckCfg *cfg)
{
    uint8_t k;

    cfg->filter.mode = MAC_FILTER_OFF;
    cfg->filter.panId = 0xBEE5u;
    cfg->filter.shortAddr = 0x0001u;

    for (k = 0; k < SRCMATCH_EXT_LEN; ++k)
        cfg->filter.extAddr[k] = 0u;

    /* The extended destination of pollExt, in on-air order. */
    cfg->filter.extAddr[0] = 0xCCu;
    cfg->filter.extAddr[1] = 0xBBu;
    cfg->filter.extAddr[2] = 0xAAu;
    cfg->filter.extAddr[3] = 0x1Cu;
    cfg->filter.extAddr[4] = 0x00u;
    cfg->filter.extAddr[5] = 0x4Bu;
    cfg->filter.extAddr[6] = 0x12u;
    cfg->filter.extAddr[7] = 0x00u;
    cfg->isCoordinator = 0u;
}

TU_TEST(destination_pan_is_read_for_both_addressing_modes)
{
    uint8_t buf[16];
    uint16_t pan = 0u;
    uint16_t len = frame(buf, 2u, 0xDE20u);

    TU_EQ(Mac_DstPan(buf, len, &pan), 1);
    TU_EQ(pan, 0xDE20u);

    len = frame(buf, 3u, 0xBEE5u); /* 64-bit: same PAN offset */
    TU_EQ(Mac_DstPan(buf, len, &pan), 1);
    TU_EQ(pan, 0xBEE5u);

    len = frame(buf, 2u, 0xFFFFu); /* broadcast PAN is a value, not a case */
    TU_EQ(Mac_DstPan(buf, len, &pan), 1);
    TU_EQ(pan, 0xFFFFu);
}

TU_TEST(a_frame_without_destination_addressing_has_no_pan)
{
    /* An ACK has no addressing, so there is no PAN to compare and it must not be dropped
   * (docs/hardware_samr21.md §6). */
    uint8_t buf[16];
    uint16_t pan = 0x1234u;

    buf[0] = 0x02u; /* frame type 2 (ACK) */
    buf[1] = 0x00u;
    buf[2] = 0x42u;
    TU_EQ(Mac_DstPan(buf, 3u, &pan), 0);
    TU_EQ(pan, 0x1234u);

    /* Reserved mode 1 cannot be interpreted, so it is not a PAN claim. */
    TU_EQ(Mac_DstPan(buf, frame(buf, 1u, 0x1111u), &pan), 0);
}

TU_TEST(the_ack_request_bit_is_read_from_the_first_octet)
{
    uint8_t buf[8];

    buf[0] = 0x01u;
    TU_EQ(Mac_AckRequested(buf, 4u), 0);
    buf[0] = 0x21u;
    TU_EQ(Mac_AckRequested(buf, 4u), 1);
    TU_EQ(Mac_AckRequested(buf, 0u), 0);
    TU_EQ(Mac_AckRequested(0, 4u), 0);
}

TU_TEST(short_and_malformed_frames_are_not_interpreted)
{
    uint8_t buf[16];
    uint16_t pan = 0x5678u;

    (void)frame(buf, 2u, 0xDE20u);
    TU_EQ(Mac_DstPan(buf, 0u, &pan), 0); /* empty */
    TU_EQ(Mac_DstPan(buf, 4u, &pan), 0); /* one byte short of the PAN field */
    TU_EQ(Mac_DstPan(0, 8u, &pan), 0);   /* no PSDU */
    TU_EQ(Mac_DstPan(buf, 8u, 0), 0);    /* no output */
    TU_EQ(pan, 0x5678u);
}

/* Version 2 data frames as Thread sends them: PAN-compressed short addresses, and extended addresses,
 * which compression leaves no PAN at all. FCS last. */
static const uint8_t v2Short[] = {0x61u, 0xA8u, 0x07u, 0xEFu, 0xBEu, 0x01u, 0x00u, 0x02u, 0x00u, 0xAAu, 0x00u, 0x00u};
static const uint8_t v2ExtCompressed[] = {0x61u, 0xECu, 0x07u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u, 0x77u, 0x88u,
                                          0x21u, 0x22u, 0x23u, 0x24u, 0x25u, 0x26u, 0x27u, 0x28u, 0xAAu, 0x00u, 0x00u};
static const uint8_t v2ExtPan[] = {0x21u, 0xECu, 0x07u, 0xEFu, 0xBEu, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u, 0x77u,
                                   0x88u, 0x21u, 0x22u, 0x23u, 0x24u, 0x25u, 0x26u, 0x27u, 0x28u, 0xAAu, 0x00u, 0x00u};
/* Short addresses, each PAN present, no sequence number. */
static const uint8_t v2NoSeq[] = {0x21u, 0xA9u, 0xEFu, 0xBEu, 0x01u, 0x00u, 0xEEu, 0xBEu, 0x02u, 0x00u, 0x00u, 0x00u};

TU_TEST(a_version_2_frame_follows_its_own_pan_rules)
{
    Mac_Hdr hdr;
    uint16_t pan = 0u;

    TU_EQ(Mac_ParseHeader(v2Short, sizeof(v2Short), &hdr), 1);
    TU_EQ(hdr.version, 2);
    TU_EQ(hdr.seq, 7);
    TU_EQ(hdr.hasDstPan, 1);
    TU_EQ(hdr.hasSrcPan, 0);
    TU_EQ(hdr.dstPan, 0xBEEFu);
    TU_EQ(hdr.dstShort, 1);
    TU_EQ(hdr.srcShort, 2);
    TU_EQ(hdr.headerLen, 9);

    TU_EQ(Mac_ParseHeader(v2ExtCompressed, sizeof(v2ExtCompressed), &hdr), 1);
    TU_EQ(hdr.hasDstPan, 0);
    TU_EQ(hdr.hasSrcPan, 0);
    TU_EQ(hdr.dstExt[0], 0x11u);
    TU_EQ(hdr.srcExt[7], 0x28u);
    TU_EQ(hdr.headerLen, 19);
    TU_EQ(Mac_DstPan(v2ExtCompressed, sizeof(v2ExtCompressed), &pan), 0);

    /* Both extended and not compressed: the destination's PAN, and never a source PAN. */
    TU_EQ(Mac_ParseHeader(v2ExtPan, sizeof(v2ExtPan), &hdr), 1);
    TU_EQ(hdr.hasDstPan, 1);
    TU_EQ(hdr.hasSrcPan, 0);
    TU_EQ(hdr.dstPan, 0xBEEFu);
    TU_EQ(hdr.dstExt[0], 0x11u);
    TU_EQ(hdr.srcExt[0], 0x21u);
    TU_EQ(hdr.headerLen, 21);
    TU_EQ(Mac_DstPan(v2ExtPan, sizeof(v2ExtPan), &pan), 1);
    TU_EQ(pan, 0xBEEFu);
}

TU_TEST(a_version_2_frame_may_suppress_its_sequence_number)
{
    Mac_Hdr hdr;
    uint16_t pan = 0u;

    TU_EQ(Mac_ParseHeader(v2NoSeq, sizeof(v2NoSeq), &hdr), 1);
    TU_EQ(hdr.seq, 0);
    TU_EQ(hdr.dstPan, 0xBEEFu);
    TU_EQ(hdr.dstShort, 1);
    TU_EQ(hdr.srcPan, 0xBEEEu);
    TU_EQ(hdr.srcShort, 2);
    TU_EQ(hdr.headerLen, 10);
    TU_EQ(Mac_DstPan(v2NoSeq, sizeof(v2NoSeq), &pan), 1);
    TU_EQ(pan, 0xBEEFu);
}

TU_TEST(a_version_2_extended_frame_to_us_is_acknowledged_without_a_pan)
{
    Mac_Hdr hdr;
    Mac_AckCfg cfg;
    SrcMatch_Table table;
    uint8_t k;
    int fp = 1;

    SrcMatch_Init(&table);
    cfg.isCoordinator = 0u;
    cfg.filter.mode = MAC_FILTER_OFF;
    cfg.filter.panId = 0xBEEFu;
    cfg.filter.shortAddr = 0x0001u;

    for (k = 0u; k < SRCMATCH_EXT_LEN; ++k)
        cfg.filter.extAddr[k] = v2ExtCompressed[3u + k];

    TU_EQ(Mac_ParseHeader(v2ExtCompressed, sizeof(v2ExtCompressed), &hdr), 1);
    TU_EQ(Mac_Filter(&hdr, &cfg.filter), 1);
    TU_EQ(Mac_AckDecide(&hdr, v2ExtCompressed, sizeof(v2ExtCompressed), 1, &cfg, &table, &fp), 1);
    TU_EQ(fp, 0);
}

TU_TEST(a_short_data_request_parses_octet_for_octet)
{
    Mac_Hdr hdr;

    TU_EQ(Mac_ParseHeader(pollShort, sizeof(pollShort), &hdr), 1);
    TU_EQ(hdr.type, MAC_TYPE_COMMAND);
    TU_EQ(hdr.version, 1);
    TU_EQ(hdr.security, 0);
    TU_EQ(hdr.framePending, 0);
    TU_EQ(hdr.ackRequest, 1);
    TU_EQ(hdr.panCompression, 1);
    TU_EQ(hdr.seq, 0x00u);
    TU_EQ(hdr.dstMode, MAC_ADDR_MODE_SHORT);
    TU_EQ(hdr.srcMode, MAC_ADDR_MODE_SHORT);
    TU_EQ(hdr.hasDstPan, 1);
    TU_EQ(hdr.hasSrcPan, 0); /* compressed away */
    TU_EQ(hdr.dstPan, 0xBEE5u);
    TU_EQ(hdr.srcPan, 0xBEE5u); /* inherited from the destination */
    TU_EQ(hdr.dstShort, 0x0001u);
    TU_EQ(hdr.srcShort, 0x0002u);
    TU_EQ(hdr.headerLen, 9); /* FCF 2 + DSN 1 + PAN 2 + dst 2 + src 2 */
    TU_EQ(Mac_IsDataRequest(&hdr, pollShort, sizeof(pollShort)), 1);
}

TU_TEST(an_extended_data_request_parses_and_its_address_is_on_air_order)
{
    Mac_Hdr hdr;

    TU_EQ(Mac_ParseHeader(pollExt, sizeof(pollExt), &hdr), 1);
    TU_EQ(hdr.dstMode, MAC_ADDR_MODE_EXT);
    TU_EQ(hdr.srcMode, MAC_ADDR_MODE_EXT);
    TU_EQ(hdr.headerLen, 21); /* 3 + PAN 2 + dst 8 + src 8 */
    TU_EQ(hdr.dstExt[0], 0xCCu); /* first octet on air */
    TU_EQ(hdr.dstExt[7], 0x00u);
    TU_EQ(hdr.srcExt[0], 0x11u);
    TU_EQ(hdr.srcExt[7], 0x88u);
    TU_EQ(Mac_IsDataRequest(&hdr, pollExt, sizeof(pollExt)), 1);
}

TU_TEST(a_data_frame_is_not_a_data_request)
{
    Mac_Hdr hdr;

    TU_EQ(Mac_ParseHeader(dataFrame, sizeof(dataFrame), &hdr), 1);
    TU_EQ(hdr.type, MAC_TYPE_DATA);
    TU_EQ(hdr.headerLen, 9);
    TU_EQ(Mac_IsDataRequest(&hdr, dataFrame, sizeof(dataFrame)), 0);
}

TU_TEST(a_truncated_frame_never_parses)
{
    Mac_Hdr hdr;
    uint16_t len;

    /* Nine octets is the whole header of this frame: one octet less and the addressing
     * field is short, one octet more and the command is present. */
    for (len = 0; len < 9u; ++len)
        TU_EQ(Mac_ParseHeader(pollShort, len, &hdr), 0);

    TU_EQ(Mac_ParseHeader(pollShort, sizeof(pollShort), &hdr), 1);
    TU_EQ(Mac_ParseHeader(0, sizeof(pollShort), &hdr), 0);
    TU_EQ(Mac_ParseHeader(pollShort, sizeof(pollShort), 0), 0);
}

TU_TEST(a_reserved_addressing_mode_is_not_parsed)
{
    uint8_t buf[sizeof(pollShort)];
    Mac_Hdr hdr;

    for (uint8_t k = 0; k < sizeof(buf); ++k)
        buf[k] = pollShort[k];

    buf[0] = (uint8_t)(pollShort[0]);
    buf[1] = (uint8_t)((pollShort[1] & 0xC3u) | 0x04u); /* destination mode 1 */
    TU_EQ(Mac_ParseHeader(buf, sizeof(buf), &hdr), 0);
}

/* The auxiliary security header follows the addressing fields, so a secured frame is filtered
 * on the same fields as a plain one: a secured frame the hardware filter delivered must not
 * disappear because the filter moved into software. */
TU_TEST(a_secured_frame_parses_its_addressing_and_filters_like_any_other)
{
    uint8_t buf[sizeof(pollShort)];
    Mac_FilterCfg cfg;
    Mac_Hdr hdr;
    Mac_AckCfg ack;

    cfgDefault(&ack);
    cfg = ack.filter;

    for (uint8_t k = 0; k < sizeof(buf); ++k)
        buf[k] = pollShort[k];

    buf[0] = (uint8_t)(buf[0] | 0x08u); /* security enabled */
    TU_EQ(Mac_ParseHeader(buf, sizeof(buf), &hdr), 1);
    TU_EQ(hdr.security, 1);
    TU_EQ(hdr.headerLen, 9);
    TU_EQ(hdr.dstShort, 0x0001u);
    TU_EQ(hdr.srcShort, 0x0002u);
    TU_EQ(Mac_Filter(&hdr, &cfg), 1);
    TU_EQ(Mac_IsDataRequest(&hdr, buf, sizeof(buf)), 0); /* the identifier is not readable */

    cfg.shortAddr = 0x0005u;
    TU_EQ(Mac_Filter(&hdr, &cfg), 0);
}

/* A Thread poll is secured, so its command identifier cannot be read: the table answers it. */
TU_TEST(a_secured_command_frame_is_answered_by_the_table)
{
    uint8_t buf[sizeof(pollShort)];
    Mac_AckCfg cfg;
    SrcMatch_Table t;
    Mac_Hdr hdr;
    int fp = -1;

    cfgDefault(&cfg);
    SrcMatch_Init(&t);

    for (uint8_t k = 0; k < sizeof(buf); ++k)
        buf[k] = pollShort[k];

    buf[0] = (uint8_t)(buf[0] | 0x08u);
    TU_EQ(Mac_ParseHeader(buf, sizeof(buf), &hdr), 1);

    /* The table off: a coordinator that cannot know claims pending. */
    TU_EQ(Mac_AckDecide(&hdr, buf, sizeof(buf), 1, &cfg, &t, &fp), 1);
    TU_EQ(fp, 1);

    /* The table on and the source absent: nothing is queued, whatever the frame was. */
    t.enabled = 1u;
    fp = -1;
    TU_EQ(Mac_AckDecide(&hdr, buf, sizeof(buf), 1, &cfg, &t, &fp), 1);
    TU_EQ(fp, 0);

    /* The table on and the source held: a frame is queued. */
    TU_EQ(SrcMatch_InsertShort(&t, hdr.srcShort), SRCMATCH_OK);
    fp = -1;
    TU_EQ(Mac_AckDecide(&hdr, buf, sizeof(buf), 1, &cfg, &t, &fp), 1);
    TU_EQ(fp, 1);
}

TU_TEST(the_header_length_comes_from_the_fcf_alone)
{
    uint16_t fcf;

    fcf = (uint16_t)(pollShort[0] | ((uint16_t)pollShort[1] << 8));
    TU_EQ(Mac_HeaderLenFromFcf(fcf), 9);

    fcf = (uint16_t)(pollExt[0] | ((uint16_t)pollExt[1] << 8));
    TU_EQ(Mac_HeaderLenFromFcf(fcf), 21);

    /* PAN not compressed: the source PAN field is present. */
    fcf = (uint16_t)(pollShort[0] & ~0x40u);
    fcf = (uint16_t)(fcf | ((uint16_t)pollShort[1] << 8));
    TU_EQ(Mac_HeaderLenFromFcf(fcf), 11);

    /* Extended destination, short source. */
    fcf = 0x8C63u; /* dst ext (3), src short (2) */
    TU_EQ(Mac_HeaderLenFromFcf(fcf), 3 + 2 + 8 + 2);

    /* A beacon with short addressing and a source PAN. */
    fcf = (uint16_t)(beaconFrame[0] | ((uint16_t)beaconFrame[1] << 8));
    TU_EQ(Mac_HeaderLenFromFcf(fcf), 11);

    /* An ACK has no addressing at all. */
    fcf = 0x0002u;
    TU_EQ(Mac_HeaderLenFromFcf(fcf), 3);

    /* Unknown: a security header. */
    TU_EQ(Mac_HeaderLenFromFcf(0x000Au), 0);
    /* Unknown: a reserved addressing mode. */
    TU_EQ(Mac_HeaderLenFromFcf(0x0402u), 0);

    /* The computed length agrees with the parse for every vector. */
    {
        Mac_Hdr hdr;

        (void)Mac_ParseHeader(pollShort, sizeof(pollShort), &hdr);
        TU_EQ(Mac_HeaderLenFromFcf((uint16_t)(pollShort[0] | ((uint16_t)pollShort[1] << 8))),
              hdr.headerLen);
        (void)Mac_ParseHeader(pollExt, sizeof(pollExt), &hdr);
        TU_EQ(Mac_HeaderLenFromFcf((uint16_t)(pollExt[0] | ((uint16_t)pollExt[1] << 8))),
              hdr.headerLen);
    }
}

/* The rig's matrix (docs/testing.md §3): what the filtered mode delivers. */
TU_TEST(the_filtered_mode_matches_the_rig_matrix)
{
    Mac_FilterCfg cfg;
    Mac_Hdr hdr;
    Mac_AckCfg ack;

    cfgDefault(&ack);
    cfg = ack.filter;

    TU_EQ(Mac_ParseHeader(dataFrame, sizeof(dataFrame), &hdr), 1);
    TU_EQ(Mac_Filter(&hdr, &cfg), 1); /* to us, our PAN */

    TU_EQ(Mac_ParseHeader(dataBroadcast, sizeof(dataBroadcast), &hdr), 1);
    TU_EQ(Mac_Filter(&hdr, &cfg), 1); /* broadcast destination */

    TU_EQ(Mac_ParseHeader(dataThird, sizeof(dataThird), &hdr), 1);
    TU_EQ(Mac_Filter(&hdr, &cfg), 0); /* third address: dropped */

    TU_EQ(Mac_ParseHeader(dataForeign, sizeof(dataForeign), &hdr), 1);
    TU_EQ(Mac_Filter(&hdr, &cfg), 0); /* foreign PAN: dropped */

    TU_EQ(Mac_ParseHeader(pollShort, sizeof(pollShort), &hdr), 1);
    TU_EQ(Mac_Filter(&hdr, &cfg), 1); /* a poll to us */
}

TU_TEST(a_promiscuous_mode_accepts_what_the_filtered_one_drops)
{
    Mac_FilterCfg cfg;
    Mac_Hdr hdr;

    cfg.mode = MAC_FILTER_PAN;
    cfg.panId = 0xBEE5u;
    cfg.shortAddr = 0x0001u;

    TU_EQ(Mac_ParseHeader(dataThird, sizeof(dataThird), &hdr), 1);
    TU_EQ(Mac_Filter(&hdr, &cfg), 1);
    TU_EQ(Mac_ParseHeader(dataForeign, sizeof(dataForeign), &hdr), 1);
    TU_EQ(Mac_Filter(&hdr, &cfg), 1);

    cfg.mode = MAC_FILTER_ALL;
    TU_EQ(Mac_Filter(&hdr, &cfg), 1);
    TU_EQ(Mac_ParseHeader(ackFrame, sizeof(ackFrame), &hdr), 1);
    TU_EQ(Mac_Filter(&hdr, &cfg), 1); /* the sniffer sees Acks */
}

TU_TEST(an_ack_and_a_reserved_type_are_not_delivered_by_the_filtered_mode)
{
    Mac_FilterCfg cfg;
    Mac_Hdr hdr;
    Mac_AckCfg ack;
    uint8_t buf[sizeof(dataFrame)];

    cfgDefault(&ack);
    cfg = ack.filter;

    TU_EQ(Mac_ParseHeader(ackFrame, sizeof(ackFrame), &hdr), 1);
    TU_EQ(hdr.type, MAC_TYPE_ACK);
    TU_EQ(Mac_Filter(&hdr, &cfg), 0);

    for (uint8_t k = 0; k < sizeof(buf); ++k)
        buf[k] = dataFrame[k];

    buf[0] = (uint8_t)((buf[0] & 0xF8u) | 0x04u); /* frame type 4: reserved */
    TU_EQ(Mac_ParseHeader(buf, sizeof(buf), &hdr), 1);
    TU_EQ(Mac_Filter(&hdr, &cfg), 0);

    /* A frame with no addressing at all is rejected by rule 8. */
    buf[0] = 0x01u;
    buf[1] = 0x00u;
    TU_EQ(Mac_ParseHeader(buf, 3u, &hdr), 1);
    TU_EQ(Mac_Filter(&hdr, &cfg), 0);
}

TU_TEST(a_beacon_from_a_foreign_pan_is_dropped_unless_the_pan_is_unconfigured)
{
    Mac_FilterCfg cfg;
    Mac_Hdr hdr;
    uint8_t buf[sizeof(beaconFrame)];

    cfg.mode = MAC_FILTER_OFF;
    cfg.panId = 0xBEE5u;
    cfg.shortAddr = 0x0001u;

    for (uint8_t k = 0; k < sizeof(buf); ++k)
        buf[k] = beaconFrame[k];

    TU_EQ(Mac_ParseHeader(buf, sizeof(buf), &hdr), 1);
    TU_EQ(Mac_Filter(&hdr, &cfg), 1); /* dst PAN matches */

    /* The beacon's own source PAN is the foreign one. */
    buf[7] = 0x11u;
    buf[8] = 0x22u;
    TU_EQ(Mac_ParseHeader(buf, sizeof(buf), &hdr), 1);
    TU_EQ(hdr.hasSrcPan, 1);
    TU_EQ(Mac_Filter(&hdr, &cfg), 0);

    /* PAN 0xFFFF is the permissive default: every beacon is accepted. */
    cfg.panId = 0xFFFFu;
    TU_EQ(Mac_Filter(&hdr, &cfg), 1);
}

TU_TEST(a_data_request_from_an_address_in_the_table_claims_pending)
{
    Mac_AckCfg cfg;
    SrcMatch_Table t;
    Mac_Hdr hdr;
    int fp = -1;

    cfgDefault(&cfg);
    SrcMatch_Init(&t);
    t.enabled = 1u;

    TU_EQ(Mac_ParseHeader(pollShort, sizeof(pollShort), &hdr), 1);

    TU_EQ(Mac_AckDecide(&hdr, pollShort, sizeof(pollShort), 1, &cfg, &t, &fp), 1);
    TU_EQ(fp, 0); /* nothing queued */

    TU_EQ(SrcMatch_InsertShort(&t, 0x0002u), SRCMATCH_OK);
    TU_EQ(Mac_AckDecide(&hdr, pollShort, sizeof(pollShort), 1, &cfg, &t, &fp), 1);
    TU_EQ(fp, 1);

    /* A different peer's address does not leak across. */
    TU_EQ(SrcMatch_SetShortList(&t, 0, 0), SRCMATCH_OK);
    TU_EQ(SrcMatch_InsertShort(&t, 0x0003u), SRCMATCH_OK);
    TU_EQ(Mac_AckDecide(&hdr, pollShort, sizeof(pollShort), 1, &cfg, &t, &fp), 1);
    TU_EQ(fp, 0);
}

TU_TEST(a_data_request_with_extended_source_uses_the_extended_table)
{
    Mac_AckCfg cfg;
    SrcMatch_Table t;
    Mac_Hdr hdr;
    int fp = -1;
    uint8_t src[SRCMATCH_EXT_LEN];

    cfgDefault(&cfg);
    SrcMatch_Init(&t);
    t.enabled = 1u;

    for (uint8_t k = 0; k < SRCMATCH_EXT_LEN; ++k)
        src[k] = pollExt[13u + k]; /* the source address as it appears on air */

    TU_EQ(Mac_ParseHeader(pollExt, sizeof(pollExt), &hdr), 1);
    TU_EQ(Mac_AckDecide(&hdr, pollExt, sizeof(pollExt), 1, &cfg, &t, &fp), 1);
    TU_EQ(fp, 0);

    TU_EQ(SrcMatch_InsertExt(&t, src), SRCMATCH_OK);
    TU_EQ(Mac_AckDecide(&hdr, pollExt, sizeof(pollExt), 1, &cfg, &t, &fp), 1);
    TU_EQ(fp, 1);
}

TU_TEST(a_disabled_table_claims_pending_for_every_poll)
{
    Mac_AckCfg cfg;
    SrcMatch_Table t;
    Mac_Hdr hdr;
    int fp = -1;

    cfgDefault(&cfg);
    SrcMatch_Init(&t); /* enabled = 0, empty */

    TU_EQ(Mac_ParseHeader(pollShort, sizeof(pollShort), &hdr), 1);
    TU_EQ(Mac_AckDecide(&hdr, pollShort, sizeof(pollShort), 1, &cfg, &t, &fp), 1);
    TU_EQ(fp, 1); /* the standard's fallback: cannot know, so claim pending */

    /* And the same with the table enabled but empty: the opposite answer, which is what
     * makes the two settings distinguishable on the wire. */
    t.enabled = 1u;
    TU_EQ(Mac_AckDecide(&hdr, pollShort, sizeof(pollShort), 1, &cfg, &t, &fp), 1);
    TU_EQ(fp, 0);
}

TU_TEST(a_data_frame_never_claims_pending)
{
    Mac_AckCfg cfg;
    SrcMatch_Table t;
    Mac_Hdr hdr;
    int fp = -1;

    cfgDefault(&cfg);
    SrcMatch_Init(&t);
    t.enabled = 1u;
    (void)SrcMatch_InsertShort(&t, 0x0002u);

    TU_EQ(Mac_ParseHeader(dataFrame, sizeof(dataFrame), &hdr), 1);
    TU_EQ(Mac_AckDecide(&hdr, dataFrame, sizeof(dataFrame), 1, &cfg, &t, &fp), 1);
    TU_EQ(fp, 0); /* the transceiver sets 0 for anything but a Data Request */
}

TU_TEST(no_ack_is_due_for_a_broadcast_a_bad_fcs_or_a_foreign_destination)
{
    Mac_AckCfg cfg;
    SrcMatch_Table t;
    Mac_Hdr hdr;
    int fp = -1;

    cfgDefault(&cfg);
    SrcMatch_Init(&t);
    t.enabled = 1u;

    TU_EQ(Mac_ParseHeader(dataBroadcast, sizeof(dataBroadcast), &hdr), 1);
    TU_EQ(Mac_AckDecide(&hdr, dataBroadcast, sizeof(dataBroadcast), 1, &cfg, &t, &fp), 0);

    TU_EQ(Mac_ParseHeader(pollShort, sizeof(pollShort), &hdr), 1);
    TU_EQ(Mac_AckDecide(&hdr, pollShort, sizeof(pollShort), 0, &cfg, &t, &fp), 0); /* bad FCS */

    TU_EQ(Mac_ParseHeader(dataThird, sizeof(dataThird), &hdr), 1);
    TU_EQ(Mac_AckDecide(&hdr, dataThird, sizeof(dataThird), 1, &cfg, &t, &fp), 0);

    /* AR clear: nothing to answer. */
    {
        uint8_t buf[sizeof(dataFrame)];

        for (uint8_t k = 0; k < sizeof(buf); ++k)
            buf[k] = dataFrame[k];

        buf[0] = (uint8_t)(buf[0] & ~0x20u);
        TU_EQ(Mac_ParseHeader(buf, sizeof(buf), &hdr), 1);
        TU_EQ(Mac_AckDecide(&hdr, buf, sizeof(buf), 1, &cfg, &t, &fp), 0);
    }
}

TU_TEST(a_promiscuous_mode_never_acknowledges)
{
    Mac_AckCfg cfg;
    SrcMatch_Table t;
    Mac_Hdr hdr;
    int fp = -1;

    cfgDefault(&cfg);
    SrcMatch_Init(&t);
    t.enabled = 1u;
    (void)SrcMatch_InsertShort(&t, 0x0002u);

    TU_EQ(Mac_ParseHeader(pollShort, sizeof(pollShort), &hdr), 1);

    cfg.filter.mode = MAC_FILTER_PAN;
    TU_EQ(Mac_AckDecide(&hdr, pollShort, sizeof(pollShort), 1, &cfg, &t, &fp), 0);
    cfg.filter.mode = MAC_FILTER_ALL;
    TU_EQ(Mac_AckDecide(&hdr, pollShort, sizeof(pollShort), 1, &cfg, &t, &fp), 0);
}

TU_TEST(the_ack_is_four_octets_and_carries_the_sequence_number)
{
    uint8_t ack[MAC_ACK_LEN];

    Mac_BuildImmAck(0x42u, 0, ack);
    TU_EQ(ack[0], 5); /* PHR: FCF, DSN and the two FCS octets the radio appends */
    TU_EQ(ack[1], 0x02u); /* FCF: acknowledgment, pending clear */
    TU_EQ(ack[2], 0x00u);
    TU_EQ(ack[3], 0x42u);

    Mac_BuildImmAck(0x42u, 1, ack);
    TU_EQ(ack[1], 0x12u); /* pending set */

    /* The octets the radio puts on air are these plus an FCS it computes itself, which is
     * the same shape the transceiver composes: 5 octets, type 2, no addressing. */
    Mac_BuildImmAck(0x00u, 0, ack);
    TU_EQ(ack[1], ackFrame[0]);
    TU_EQ(ack[2], ackFrame[1]);
    TU_EQ(ack[3], ackFrame[2]);
}

int main(void)
{
    static const Tu_Test tests[] = {{"destination_pan_is_read_for_both_addressing_modes",
                                     destination_pan_is_read_for_both_addressing_modes},
                                    {"a_frame_without_destination_addressing_has_no_pan",
                                     a_frame_without_destination_addressing_has_no_pan},
                                    {"the_ack_request_bit_is_read_from_the_first_octet",
                                     the_ack_request_bit_is_read_from_the_first_octet},
                                    {"short_and_malformed_frames_are_not_interpreted",
                                     short_and_malformed_frames_are_not_interpreted},
                                    {"a_short_data_request_parses_octet_for_octet",
                                     a_short_data_request_parses_octet_for_octet},
                                    {"a_version_2_frame_follows_its_own_pan_rules", a_version_2_frame_follows_its_own_pan_rules},
                                    {"a_version_2_frame_may_suppress_its_sequence_number", a_version_2_frame_may_suppress_its_sequence_number},
                                    {"a_version_2_extended_frame_to_us_is_acknowledged_without_a_pan", a_version_2_extended_frame_to_us_is_acknowledged_without_a_pan},
                                    {"an_extended_data_request_parses_and_its_address_is_on_air_order",
                                     an_extended_data_request_parses_and_its_address_is_on_air_order},
                                    {"a_data_frame_is_not_a_data_request",
                                     a_data_frame_is_not_a_data_request},
                                    {"a_truncated_frame_never_parses", a_truncated_frame_never_parses},
                                    {"a_reserved_addressing_mode_is_not_parsed",
                                     a_reserved_addressing_mode_is_not_parsed},
                                    {"a_secured_frame_parses_its_addressing_and_filters_like_any_other",
                                     a_secured_frame_parses_its_addressing_and_filters_like_any_other},
                                    {"a_secured_command_frame_is_answered_by_the_table",
                                     a_secured_command_frame_is_answered_by_the_table},
                                    {"the_header_length_comes_from_the_fcf_alone",
                                     the_header_length_comes_from_the_fcf_alone},
                                    {"the_filtered_mode_matches_the_rig_matrix",
                                     the_filtered_mode_matches_the_rig_matrix},
                                    {"a_promiscuous_mode_accepts_what_the_filtered_one_drops",
                                     a_promiscuous_mode_accepts_what_the_filtered_one_drops},
                                    {"an_ack_and_a_reserved_type_are_not_delivered_by_the_filtered_mode",
                                     an_ack_and_a_reserved_type_are_not_delivered_by_the_filtered_mode},
                                    {"a_beacon_from_a_foreign_pan_is_dropped_unless_the_pan_is_unconfigured",
                                     a_beacon_from_a_foreign_pan_is_dropped_unless_the_pan_is_unconfigured},
                                    {"a_data_request_from_an_address_in_the_table_claims_pending",
                                     a_data_request_from_an_address_in_the_table_claims_pending},
                                    {"a_data_request_with_extended_source_uses_the_extended_table",
                                     a_data_request_with_extended_source_uses_the_extended_table},
                                    {"a_disabled_table_claims_pending_for_every_poll",
                                     a_disabled_table_claims_pending_for_every_poll},
                                    {"a_data_frame_never_claims_pending", a_data_frame_never_claims_pending},
                                    {"no_ack_is_due_for_a_broadcast_a_bad_fcs_or_a_foreign_destination",
                                     no_ack_is_due_for_a_broadcast_a_bad_fcs_or_a_foreign_destination},
                                    {"a_promiscuous_mode_never_acknowledges",
                                     a_promiscuous_mode_never_acknowledges},
                                    {"the_ack_is_four_octets_and_carries_the_sequence_number",
                                     the_ack_is_four_octets_and_carries_the_sequence_number}};

    Tu_Run("mac", tests, (int)(sizeof(tests) / sizeof(tests[0])));
    return 0;
}
