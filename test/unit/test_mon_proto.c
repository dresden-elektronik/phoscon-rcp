/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 dresden elektronik ingenieurtechnik gmbh */
/* test_mon_proto.c — the bench register monitor's command format and address policy
 * (src/app/mon_proto.c). */
#include <stdint.h>

#include "mon_proto.h"
#include "tu.h"

/* A table shaped like the ConBee III's: a read-only hole listed before the writable window that
 * contains it, a read-only memory, and a writable window that ends at the top of the address space. */
static const Mon_Region regions[] = {
    {0x40030000u, 0x4000u, 0},
    {0x40000000u, 0x10000000u, 1},
    {0x00000000u, 0x00100000u, 0},
    {0xFFFFF000u, 0x1000u, 1},
};
#define NREG ((uint16_t)(sizeof(regions) / sizeof(regions[0])))

static uint16_t put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    return 4u;
}

static uint16_t buildRead(uint8_t *p, uint8_t tag, uint32_t addr, uint8_t count)
{
    uint16_t n = 0u;

    p[n++] = MON_OP_READ;
    p[n++] = tag;
    n = (uint16_t)(n + put32(&p[n], addr));
    p[n++] = count;
    return n;
}

static uint16_t buildWrite(uint8_t *p, uint8_t tag, uint32_t addr, uint8_t count, uint32_t first)
{
    uint16_t n = 0u;
    uint8_t i;

    p[n++] = MON_OP_WRITE;
    p[n++] = tag;
    n = (uint16_t)(n + put32(&p[n], addr));
    p[n++] = count;

    for (i = 0u; i < count; ++i)
        n = (uint16_t)(n + put32(&p[n], first + i));

    return n;
}

static uint16_t buildTrace(uint8_t *p, uint32_t waddr, uint32_t taddr, uint8_t count, uint32_t cycles)
{
    uint16_t n = 0u;

    p[n++] = MON_OP_TRACE;
    p[n++] = 0x5Au;
    n = (uint16_t)(n + put32(&p[n], waddr));
    n = (uint16_t)(n + put32(&p[n], 0x00000080u));
    n = (uint16_t)(n + put32(&p[n], taddr));
    n = (uint16_t)(n + put32(&p[n], 0x0F000000u));
    p[n++] = count;
    n = (uint16_t)(n + put32(&p[n], cycles));
    return n;
}

TU_TEST(read_decodes)
{
    uint8_t p[16];
    Mon_Cmd c;
    uint16_t n = buildRead(p, 7u, 0x40008070u, 3u);

    TU_EQ(Mon_Parse(p, n, &c), MON_ST_OK);
    TU_EQ(c.op, MON_OP_READ);
    TU_EQ(c.tag, 7u);
    TU_EQ(c.addr, 0x40008070u);
    TU_EQ(c.count, 3u);
    TU_EQ(Mon_Check(&c, regions, NREG), MON_ST_OK);
}

TU_TEST(write_decodes_every_word_in_order)
{
    uint8_t p[7u + 4u * MON_WRITE_MAX];
    Mon_Cmd c;
    uint16_t n = buildWrite(p, 9u, 0x40001000u, (uint8_t)MON_WRITE_MAX, 0x11223300u);

    TU_EQ(Mon_Parse(p, n, &c), MON_ST_OK);
    TU_EQ(c.count, MON_WRITE_MAX);
    TU_EQ(c.values[0], 0x11223300u);
    TU_EQ(c.values[MON_WRITE_MAX - 1u], 0x11223300u + MON_WRITE_MAX - 1u);
    TU_EQ(Mon_Check(&c, regions, NREG), MON_ST_OK);
}

TU_TEST(trace_decodes)
{
    uint8_t p[32];
    Mon_Cmd c;
    uint16_t n = buildTrace(p, 0x40020000u, 0x40020040u, 4u, 38400u);

    TU_EQ(n, 23u);
    TU_EQ(Mon_Parse(p, n, &c), MON_ST_OK);
    TU_EQ(c.tag, 0x5Au);
    TU_EQ(c.addr, 0x40020000u);
    TU_EQ(c.values[0], 0x80u);
    TU_EQ(c.traceAddr, 0x40020040u);
    TU_EQ(c.traceMask, 0x0F000000u);
    TU_EQ(c.count, 4u);
    TU_EQ(c.traceCycles, 38400u);
    TU_EQ(Mon_Check(&c, regions, NREG), MON_ST_OK);
}

TU_TEST(counts_are_bounded_both_ways)
{
    uint8_t p[32];
    uint8_t w[7u + 4u * (MON_WRITE_MAX + 1u)];
    Mon_Cmd c;

    TU_EQ(Mon_Parse(p, buildRead(p, 1u, 0x40000000u, 0u), &c), MON_ST_BAD_COUNT);
    TU_EQ(Mon_Parse(p, buildRead(p, 1u, 0x40000000u, (uint8_t)(MON_READ_MAX + 1u)), &c),
          MON_ST_BAD_COUNT);
    TU_EQ(Mon_Parse(p, buildRead(p, 1u, 0x40000000u, (uint8_t)MON_READ_MAX), &c), MON_ST_OK);
    TU_EQ(Mon_Parse(w, buildWrite(w, 1u, 0x40000000u, (uint8_t)(MON_WRITE_MAX + 1u), 0u), &c),
          MON_ST_BAD_COUNT);
    TU_EQ(Mon_Parse(p, buildTrace(p, 0x40000000u, 0x40000000u, 0u, 100u), &c), MON_ST_BAD_COUNT);
    TU_EQ(Mon_Parse(p, buildTrace(p, 0x40000000u, 0x40000000u, (uint8_t)(MON_TRACE_MAX + 1u), 100u),
                    &c),
          MON_ST_BAD_COUNT);
}

TU_TEST(trace_budget_is_bounded)
{
    uint8_t p[32];
    Mon_Cmd c;

    TU_EQ(Mon_Parse(p, buildTrace(p, 0x40000000u, 0x40000000u, 1u, 0u), &c), MON_ST_BAD_CYCLES);
    TU_EQ(Mon_Parse(p, buildTrace(p, 0x40000000u, 0x40000000u, 1u, MON_TRACE_CYCLES_MAX + 1u), &c),
          MON_ST_BAD_CYCLES);
    TU_EQ(Mon_Parse(p, buildTrace(p, 0x40000000u, 0x40000000u, 1u, MON_TRACE_CYCLES_MAX), &c),
          MON_ST_OK);
}

TU_TEST(lengths_must_match_exactly)
{
    uint8_t p[40];
    Mon_Cmd c;
    uint16_t n;

    n = buildRead(p, 3u, 0x40000000u, 1u);
    TU_EQ(Mon_Parse(p, (uint16_t)(n - 1u), &c), MON_ST_BAD_LEN);
    TU_EQ(Mon_Parse(p, (uint16_t)(n + 1u), &c), MON_ST_BAD_LEN);

    n = buildWrite(p, 3u, 0x40000000u, 2u, 0u);
    TU_EQ(Mon_Parse(p, (uint16_t)(n - 4u), &c), MON_ST_BAD_LEN);
    TU_EQ(Mon_Parse(p, (uint16_t)(n - 1u), &c), MON_ST_BAD_LEN);
    TU_EQ(Mon_Parse(p, (uint16_t)(n + 1u), &c), MON_ST_BAD_LEN);

    n = buildTrace(p, 0x40000000u, 0x40000000u, 1u, 100u);
    TU_EQ(Mon_Parse(p, (uint16_t)(n - 1u), &c), MON_ST_BAD_LEN);
}

TU_TEST(truncated_payload_keeps_the_tag_it_had)
{
    uint8_t p[16];
    Mon_Cmd c;

    buildRead(p, 0x42u, 0x40000000u, 1u);
    TU_EQ(Mon_Parse(p, 1u, &c), MON_ST_BAD_LEN);
    TU_EQ(c.tag, 0u);
    TU_EQ(Mon_Parse(p, 2u, &c), MON_ST_BAD_LEN);
    TU_EQ(c.tag, 0x42u);
}

TU_TEST(unknown_and_empty_are_bad_op)
{
    uint8_t p[8] = {0x09u, 0x01u};
    Mon_Cmd c;

    TU_EQ(Mon_Parse(p, 2u, &c), MON_ST_BAD_OP);
    TU_EQ(Mon_Parse(p, 0u, &c), MON_ST_BAD_OP);
    TU_EQ(Mon_Parse(0, 4u, &c), MON_ST_BAD_OP);
    TU_EQ(Mon_Parse(p, 2u, 0), MON_ST_BAD_OP);
    TU_EQ(Mon_Check(0, regions, NREG), MON_ST_BAD_OP);
}

TU_TEST(misaligned_addresses_are_refused)
{
    uint8_t p[32];
    Mon_Cmd c;

    TU_EQ(Mon_Parse(p, buildRead(p, 1u, 0x40000002u, 1u), &c), MON_ST_OK);
    TU_EQ(Mon_Check(&c, regions, NREG), MON_ST_BAD_ALIGN);
    TU_EQ(Mon_Parse(p, buildWrite(p, 1u, 0x40000001u, 1u, 0u), &c), MON_ST_OK);
    TU_EQ(Mon_Check(&c, regions, NREG), MON_ST_BAD_ALIGN);
    TU_EQ(Mon_Parse(p, buildTrace(p, 0x40000000u, 0x40000003u, 1u, 100u), &c), MON_ST_OK);
    TU_EQ(Mon_Check(&c, regions, NREG), MON_ST_BAD_ALIGN);
}

TU_TEST(read_only_hole_wins_over_the_window_around_it)
{
    uint8_t p[32];
    Mon_Cmd c;

    /* The flash controller sits inside the writable peripheral window: readable, never written. */
    TU_EQ(Mon_Parse(p, buildRead(p, 1u, 0x40030008u, 1u), &c), MON_ST_OK);
    TU_EQ(Mon_Check(&c, regions, NREG), MON_ST_OK);
    TU_EQ(Mon_Parse(p, buildWrite(p, 1u, 0x40030008u, 1u, 0u), &c), MON_ST_OK);
    TU_EQ(Mon_Check(&c, regions, NREG), MON_ST_DENIED);
    TU_EQ(Mon_Parse(p, buildTrace(p, 0x40031000u, 0x40030000u, 1u, 100u), &c), MON_ST_OK);
    TU_EQ(Mon_Check(&c, regions, NREG), MON_ST_DENIED);
}

TU_TEST(a_range_running_into_the_hole_is_refused)
{
    uint8_t p[7u + 4u * 4u];
    Mon_Cmd c;

    /* Three words below the hole are fine; the fourth is the hole's first word. */
    TU_EQ(Mon_Parse(p, buildWrite(p, 1u, 0x4002FFF4u, 3u, 0u), &c), MON_ST_OK);
    TU_EQ(Mon_Check(&c, regions, NREG), MON_ST_OK);
    TU_EQ(Mon_Parse(p, buildWrite(p, 1u, 0x4002FFF4u, 4u, 0u), &c), MON_ST_OK);
    TU_EQ(Mon_Check(&c, regions, NREG), MON_ST_DENIED);
}

TU_TEST(read_only_memory_and_unlisted_space)
{
    TU_EQ(Mon_RangeAllowed(regions, NREG, 0x00004000u, 4u, 0), 1);
    TU_EQ(Mon_RangeAllowed(regions, NREG, 0x00004000u, 4u, 1), 0);
    TU_EQ(Mon_RangeAllowed(regions, NREG, 0x000FFFFCu, 1u, 0), 1);
    TU_EQ(Mon_RangeAllowed(regions, NREG, 0x000FFFFCu, 2u, 0), 0);
    TU_EQ(Mon_RangeAllowed(regions, NREG, 0x20000000u, 1u, 0), 0);
    TU_EQ(Mon_RangeAllowed(regions, NREG, 0x40000000u, 0u, 0), 0);
    TU_EQ(Mon_RangeAllowed(0, 0u, 0x40000000u, 1u, 0), 0);
}

TU_TEST(a_range_that_wraps_is_refused)
{
    /* The top window is writable to its last word; one more word wraps to 0, which is flash and
     * read-only, and must not be reached by wrapping. */
    TU_EQ(Mon_RangeAllowed(regions, NREG, 0xFFFFFFFCu, 1u, 1), 1);
    TU_EQ(Mon_RangeAllowed(regions, NREG, 0xFFFFFFFCu, 2u, 0), 0);
}

/* Fixed-seed fuzz: every byte string up to 64 bytes decodes to a bounded result, never writes past
 * the command, and a command it accepts is one the policy can judge (coding-rules.md §4.4). */
TU_TEST(random_payloads_stay_bounded)
{
    uint32_t seed = 0x1234567u;
    uint8_t p[64];
    Mon_Cmd c;
    int round;

    for (round = 0; round < 20000; ++round)
    {
        uint16_t len;
        uint16_t i;
        Mon_Status st;

        seed = seed * 1103515245u + 12345u;
        len = (uint16_t)((seed >> 16) % (sizeof(p) + 1u));

        for (i = 0u; i < len; ++i)
        {
            seed = seed * 1103515245u + 12345u;
            p[i] = (uint8_t)(seed >> 24);
        }

        if (len > 0u && (seed & 1u))
            p[0] = (uint8_t)(1u + (seed >> 8) % 3u);

        st = Mon_Parse(p, len, &c);
        TU_CHECK(st <= MON_ST_TIMEOUT);

        if (st == MON_ST_OK)
        {
            TU_CHECK(c.count >= 1u);
            TU_CHECK(c.count <= MON_WRITE_MAX);
            TU_CHECK(Mon_Check(&c, regions, NREG) <= MON_ST_TIMEOUT);
        }
    }
}

int main(void)
{
    static const Tu_Test tests[] = {
        {"read_decodes", read_decodes},
        {"write_decodes_every_word_in_order", write_decodes_every_word_in_order},
        {"trace_decodes", trace_decodes},
        {"counts_are_bounded_both_ways", counts_are_bounded_both_ways},
        {"trace_budget_is_bounded", trace_budget_is_bounded},
        {"lengths_must_match_exactly", lengths_must_match_exactly},
        {"truncated_payload_keeps_the_tag_it_had", truncated_payload_keeps_the_tag_it_had},
        {"unknown_and_empty_are_bad_op", unknown_and_empty_are_bad_op},
        {"misaligned_addresses_are_refused", misaligned_addresses_are_refused},
        {"read_only_hole_wins_over_the_window_around_it",
         read_only_hole_wins_over_the_window_around_it},
        {"a_range_running_into_the_hole_is_refused", a_range_running_into_the_hole_is_refused},
        {"read_only_memory_and_unlisted_space", read_only_memory_and_unlisted_space},
        {"a_range_that_wraps_is_refused", a_range_that_wraps_is_refused},
        {"random_payloads_stay_bounded", random_payloads_stay_bounded}};

    Tu_Run("mon_proto", tests, (int)(sizeof(tests) / sizeof(tests[0])));
    return 0; /* Tu_Run never returns */
}
