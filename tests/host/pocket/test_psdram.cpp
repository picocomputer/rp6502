/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "Vtb_psdram.h"

#include "utest.h"

#include <map>

static Vtb_psdram *dut;
static long g_clocks;

static void clock_cycle()
{
    dut->clk = 1;
    dut->eval();
    dut->clk = 0;
    dut->eval();
    g_clocks++;
}

static uint32_t rng_state = 0x1234567u;
static uint32_t rng()
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void wr(uint32_t addr, uint16_t data)
{
    dut->w_avail = 1;
    dut->w_addr = addr;
    dut->w_data = data;
    int guard = 0;
    while (!dut->tb_psdram_wtake && guard++ < 2000)
        clock_cycle();
    clock_cycle();
    dut->w_avail = 0;
}

/* The read client is asked for a pair of words and answers with both,
 * the one at the even address in the low half. */
static int g_rd_odd;

static void ask(uint32_t addr)
{
    dut->rd_pend = 1;
    dut->rd_addr = addr >> 1;
    g_rd_odd = addr & 1;
}

static uint16_t half()
{
    return (uint16_t)(dut->tb_psdram_rword >> (g_rd_odd * 16));
}

static uint16_t rd(uint32_t addr)
{
    ask(addr);
    dut->eval();
    int guard = 0;
    while (!dut->tb_psdram_rvalid && guard++ < 2000)
        clock_cycle();
    uint16_t v = half();
    dut->rd_pend = 0;
    clock_cycle();
    return v;
}

/* A line is eight words, and word k is bits [16k+15:16k]. */
struct line_t
{
    uint16_t w[8];
};

static line_t random_line()
{
    line_t l;
    for (int k = 0; k < 8; k++)
        l.w[k] = (uint16_t)rng();
    return l;
}

static void line_start(uint32_t line, bool we, const line_t &l)
{
    dut->line_pend = 1;
    dut->line_we = we;
    dut->line_addr = line;
    for (int k = 0; k < 8; k++)
    {
        uint32_t &word = dut->line_wdata[k / 2];
        int shift = (k & 1) * 16;
        word = (word & ~(0xFFFFu << shift)) | ((uint32_t)l.w[k] << shift);
    }
    dut->eval();
}

static line_t line_result()
{
    line_t l;
    for (int k = 0; k < 8; k++)
        l.w[k] = (uint16_t)(dut->tb_psdram_line_rdata[k / 2] >> ((k & 1) * 16));
    return l;
}

/* Returns false if the line port breaks its handshake. */
static bool line_finish()
{
    int guard = 0;
    while (!dut->tb_psdram_line_done && guard++ < 2000)
        clock_cycle();
    if (!dut->tb_psdram_line_done)
        return false;
    dut->line_pend = 0;
    clock_cycle();
    return !dut->tb_psdram_line_done;
}

static bool line_wr(uint32_t line, const line_t &l)
{
    line_start(line, true, l);
    return line_finish();
}

static bool line_rd(uint32_t line, line_t &l)
{
    line_start(line, false, line_t{});
    if (!line_finish())
        return false;
    l = line_result();
    return true;
}

static bool same(const line_t &a, const line_t &b)
{
    for (int k = 0; k < 8; k++)
        if (a.w[k] != b.w[k])
            return false;
    return true;
}

UTEST(psdram, load_soak_and_refresh)
{
    dut->rst_n = 0;
    for (int i = 0; i < 4; i++)
        clock_cycle();
    dut->rst_n = 1;

    int guard = 0;
    while (!dut->tb_psdram_ready && guard++ < 40000)
        clock_cycle();
    ASSERT_TRUE(dut->tb_psdram_ready);
    long soak_start = g_clocks;

    std::map<uint32_t, uint16_t> ref;

    for (uint32_t i = 0; i < 3000; i++)
    {
        uint32_t a = 0x7FF400u + i; /* crosses rows within bank 0 */
        uint16_t v = (uint16_t)rng();
        wr(a, v);
        ref[a] = v;
    }

    for (int i = 0; i < 4000; i++)
    {
        uint32_t a = rng() & 0x1FFFFFF;
        if (rng() & 1)
        {
            uint16_t v = (uint16_t)rng();
            wr(a, v);
            ref[a] = v;
        }
        else if (!ref.empty())
        {
            auto it = ref.lower_bound(a);
            if (it == ref.end())
                it = ref.begin();
            ASSERT_EQ(rd(it->first), it->second);
        }
    }

    uint32_t a = 0x123456u;
    wr(a, 0xAAAA);
    ASSERT_EQ(rd(a), 0xAAAA);
    wr(a, 0x5555);
    ASSERT_EQ(rd(a), 0x5555);

    ask(a);
    dut->eval();
    ASSERT_TRUE(dut->tb_psdram_rvalid);
    dut->rd_pend = 0;

    for (uint32_t i = 0; i < 3000; i += 37)
        ASSERT_EQ(rd(0x7FF400u + i), ref[0x7FF400u + i]);

    /* pocket_sdram refreshes once every 390 clocks, 7.7 us at 50.4 MHz,
     * and the bound checked here is once every 504 clocks, 10 us. */
    long elapsed = g_clocks - soak_start;
    ASSERT_GT((long)dut->tb_psdram_refreshes, elapsed / 504);

    uint32_t slept_before = dut->tb_psdram_sref_clocks;
    uint32_t refreshed_before = dut->tb_psdram_refreshes;
    for (int i = 0; i < 4000; i++)
        clock_cycle();
    ASSERT_GT((long)(dut->tb_psdram_sref_clocks - slept_before), 3000L);
    ASSERT_LT((long)(dut->tb_psdram_refreshes - refreshed_before), 3L);

    ASSERT_EQ(rd(a), 0x5555);
    wr(a, 0x1234);
    ASSERT_EQ(rd(a), 0x1234);
    for (uint32_t i = 0; i < 3000; i += 37)
        ASSERT_EQ(rd(0x7FF400u + i), ref[0x7FF400u + i]);
}

static void idle_for(int n)
{
    dut->rd_pend = 0;
    dut->w_avail = 0;
    for (int i = 0; i < n; i++)
        clock_cycle();
}

/* This test uses the SDRAM that load_soak_and_refresh initialised,
 * because pocket_sdram has no reset and runs its power-up sequence only
 * once. */
UTEST(psdram, sporadic_asset_reads)
{
    std::map<uint32_t, uint16_t> ref;
    const uint32_t rom = 0x000000u;  /* STAGE+0, bank 0: the ROM image */
    const uint32_t oem = 0x1FE8000u; /* byte 0x3FD0000, bank 3 */
    for (uint32_t i = 0; i < 48; i++)
    {
        uint16_t v = (uint16_t)rng();
        wr(rom + i * 0x401u, v); /* strides across rows inside bank 0 */
        ref[rom + i * 0x401u] = v;
        v = (uint16_t)rng();
        wr(oem + i, v);
        ref[oem + i] = v;
    }
    const int gaps[] = {0, 5, 63, 64, 65, 66, 67, 70, 100, 255, 256, 257,
                        258, 259, 260, 261, 262, 263, 264, 265, 266, 300,
                        389, 390, 391, 400, 700, 1200, 4000};
    for (int g = 0; g < (int)(sizeof(gaps) / sizeof(gaps[0])); g++)
        for (uint32_t i = 0; i < 48; i++)
        {
            idle_for(gaps[g]);
            ASSERT_EQ(rd(rom + i * 0x401u), ref[rom + i * 0x401u]);
            idle_for(gaps[g]);
            ASSERT_EQ(rd(oem + i), ref[oem + i]);
        }
}

UTEST(psdram, self_refresh_residency)
{
    wr(0x000100u, 0x1111);
    wr(0x000900u, 0x2222);
    int worst = 1 << 30;
    for (int k = -8; k <= 10; k++)
    {
        (void)rd(0x000100u);
        uint32_t s0 = dut->tb_psdram_sref_clocks;
        int guard = 0;
        while (dut->tb_psdram_sref_clocks == s0 && guard++ < 2000)
            idle_for(1);
        ASSERT_LT(guard, 2000);
        if (k < 0)
        {
            int e = guard;
            (void)rd(0x000100u);
            s0 = dut->tb_psdram_sref_clocks;
            idle_for(e + k);
        }
        else
            idle_for(k);
        ask(0x000900u);
        dut->eval();
        int n = 0;
        while (!dut->tb_psdram_rvalid && n++ < 300)
            clock_cycle();
        ASSERT_EQ((int)half(), 0x2222);
        dut->rd_pend = 0;
        clock_cycle();
        int total = (int)(dut->tb_psdram_sref_clocks - s0);
        if (total > 0 && total < worst)
            worst = total;
    }
    ASSERT_GE(worst, 3); /* minimum self refresh is tRAS, 48 ns or 3 clocks */
}

UTEST(psdram, held_read_vs_sleep)
{
    const uint32_t a = 0x000100u;
    wr(a, 0x1111);
    ASSERT_EQ(rd(a), 0x1111);

    ask(a);
    dut->eval();
    ASSERT_TRUE(dut->tb_psdram_rvalid);

    uint32_t s0 = dut->tb_psdram_sref_clocks;
    int naps = 0, nap_clocks = 0, in_nap = 0;
    uint32_t prev = s0;
    for (int i = 0; i < 3000; i++)
    {
        dut->clk = 1; dut->eval(); dut->clk = 0; dut->eval();
        uint32_t now = dut->tb_psdram_sref_clocks;
        if (now != prev)
        {
            if (!in_nap) { naps++; in_nap = 1; }
            nap_clocks++;
        }
        else
            in_nap = 0;
        prev = now;
    }
    dut->rd_pend = 0;
    clock_cycle();
    ASSERT_TRUE(naps == 0 || (double)nap_clocks / naps >= 3.0);
}

/* The words of a line go to the addresses the word clients use, in
 * every bank, at both ends of a row and at both ends of a bank. */
UTEST(psdram, line_placement)
{
    std::map<uint32_t, line_t> ref;
    for (uint32_t bank = 0; bank < 4; bank++)
    {
        const uint32_t rows[] = {0, 1, 0x0AAA, 0x1555, 8190, 8191};
        for (uint32_t row : rows)
        {
            const uint32_t cols[] = {0, 1, 63, 126, 127};
            for (uint32_t col : cols)
                ref[bank << 20 | row << 7 | col] = random_line();
        }
    }
    for (auto &e : ref)
        ASSERT_TRUE(line_wr(e.first, e.second));
    for (auto &e : ref)
    {
        line_t got;
        ASSERT_TRUE(line_rd(e.first, got));
        ASSERT_TRUE(same(got, e.second));
    }
    for (auto &e : ref)
        for (uint32_t k = 0; k < 8; k++)
            ASSERT_EQ(rd(e.first << 3 | k), e.second.w[k]);

    /* Words written one at a time come back as a line, and the words
     * on either side of a line are not touched by writing it. */
    for (auto &e : ref)
    {
        if (rng() & 3)
            continue;
        uint32_t base = e.first << 3;
        uint16_t below = 0, above = 0;
        bool has_below = (base & 0x3FF) != 0;
        bool has_above = (base & 0x3FF) != 0x3F8;
        if (has_below)
            wr(base - 1, below = (uint16_t)rng());
        if (has_above)
            wr(base + 8, above = (uint16_t)rng());
        for (uint32_t k = 0; k < 8; k++)
            wr(base + (k * 5 & 7), e.second.w[k * 5 & 7] = (uint16_t)rng());
        line_t got;
        ASSERT_TRUE(line_rd(e.first, got));
        ASSERT_TRUE(same(got, e.second));
        e.second = random_line();
        ASSERT_TRUE(line_wr(e.first, e.second));
        if (has_below)
            ASSERT_EQ(rd(base - 1), below);
        if (has_above)
            ASSERT_EQ(rd(base + 8), above);
        ASSERT_TRUE(line_rd(e.first, got));
        ASSERT_TRUE(same(got, e.second));
    }
}

/* The three clients ask at once and at random offsets from each other.
 * The word clients stay off the lines that the line client writes and
 * off each other's word, so the result does not depend on the order
 * the controller picks. */
UTEST(psdram, mixed_clients)
{
    const int LINES = 96;
    uint32_t pool[LINES];
    std::map<uint32_t, uint16_t> ref;
    for (int i = 0; i < LINES; i++)
    {
        /* A third of the pool shares two rows, so rows are hit as
         * well as missed. */
        if (i < LINES / 3)
            pool[i] = (i & 1 ? 0x2ABCDu << 7 : 0x00123u << 7) | (uint32_t)i;
        else
            pool[i] = (rng() & 0x3FFF80) | (uint32_t)i;
        line_t l = random_line();
        ASSERT_TRUE(line_wr(pool[i], l));
        for (uint32_t k = 0; k < 8; k++)
            ref[pool[i] << 3 | k] = l.w[k];
    }

    long start = g_clocks;
    uint32_t refreshed = dut->tb_psdram_refreshes;
    uint32_t slept = dut->tb_psdram_sref_clocks;
    for (int trial = 0; trial < 6000; trial++)
    {
        int li = rng() % LINES, ri, wi;
        do
            ri = rng() % LINES;
        while (ri == li);
        do
            wi = rng() % LINES;
        while (wi == li || wi == ri);
        uint32_t line = pool[li];
        uint32_t ra = pool[ri] << 3 | (rng() & 7);
        uint32_t wa = pool[wi] << 3 | (rng() & 7);
        bool line_we = rng() & 1;
        line_t l = random_line();
        uint16_t wv = (uint16_t)rng();

        uint32_t r = rng();
        bool use_rd = r & 1, use_wr = r & 2, use_line = (r & 12) != 0;
        int rd_at = (r >> 4) % 20, wr_at = (r >> 9) % 20;
        int line_at = (r >> 14) % 20;
        bool rd_done = !use_rd, wr_done = !use_wr, line_done = !use_line;
        uint16_t rv = 0;
        for (int t = 0; !(rd_done && wr_done && line_done); t++)
        {
            ASSERT_LT(t, 4000);
            if (use_rd && t == rd_at)
            {
                ask(ra);
            }
            if (use_wr && t == wr_at)
            {
                dut->w_avail = 1;
                dut->w_addr = wa;
                dut->w_data = wv;
            }
            if (use_line && t == line_at)
                line_start(line, line_we, l);
            dut->eval();
            bool rvalid = dut->rd_pend && dut->tb_psdram_rvalid;
            bool wtake = dut->tb_psdram_wtake;
            bool ldone = dut->line_pend && dut->tb_psdram_line_done;
            if (rvalid)
                rv = half();
            line_t got = line_result();
            clock_cycle();
            if (rvalid)
            {
                dut->rd_pend = 0;
                rd_done = true;
            }
            if (wtake)
            {
                dut->w_avail = 0;
                wr_done = true;
            }
            if (ldone)
            {
                dut->line_pend = 0;
                line_done = true;
                if (!line_we)
                    for (uint32_t k = 0; k < 8; k++)
                        ASSERT_EQ(got.w[k], ref[line << 3 | k]);
            }
        }
        if (use_rd)
            ASSERT_EQ(rv, ref[ra]);
        if (use_wr)
            ref[wa] = wv;
        if (use_line && line_we)
            for (uint32_t k = 0; k < 8; k++)
                ref[line << 3 | k] = l.w[k];
        /* Most trials follow the last one closely. A few leave the
         * controller time to close its rows or to go to sleep. */
        uint32_t gap = rng() % 64;
        idle_for(gap == 0 ? 300 : gap == 1 ? 70 : gap < 12 ? gap : 1);
        ASSERT_FALSE(dut->tb_psdram_line_done);
    }
    /* Refreshes are owed for the clocks spent awake. */
    long awake = g_clocks - start - (long)(dut->tb_psdram_sref_clocks - slept);
    ASSERT_GT((long)(dut->tb_psdram_refreshes - refreshed), awake / 504);

    for (auto &e : ref)
        ASSERT_EQ(rd(e.first), e.second);
    for (int i = 0; i < LINES; i++)
    {
        line_t got;
        ASSERT_TRUE(line_rd(pool[i], got));
        for (uint32_t k = 0; k < 8; k++)
            ASSERT_EQ(got.w[k], ref[pool[i] << 3 | k]);
    }
}

/* Lines back to back with no other client keep the controller as busy
 * as it can be, and the refreshes still come. */
UTEST(psdram, line_soak_and_refresh)
{
    std::map<uint32_t, line_t> ref;
    long start = g_clocks;
    uint32_t refreshed = dut->tb_psdram_refreshes;
    uint32_t slept = dut->tb_psdram_sref_clocks;
    for (int i = 0; i < 6000; i++)
    {
        /* Bank 2, and each line is in a new row half of the time. */
        uint32_t line = 2u << 20 | (rng() & 0x7FFu) << 3 | (rng() & 7);
        if (rng() & 1)
            line = 2u << 20 | 0x155u << 7 | (rng() & 127);
        if (ref.count(line) && (rng() & 1))
        {
            line_start(line, false, line_t{});
            int guard = 0;
            while (!dut->tb_psdram_line_done && guard++ < 2000)
                clock_cycle();
            ASSERT_TRUE(same(line_result(), ref[line]));
        }
        else
        {
            line_start(line, true, ref[line] = random_line());
            int guard = 0;
            while (!dut->tb_psdram_line_done && guard++ < 2000)
                clock_cycle();
        }
        /* The requester drops pend for the one clock the port asks
         * for, then asks again. */
        ASSERT_TRUE(dut->tb_psdram_line_done);
        dut->line_pend = 0;
        clock_cycle();
        ASSERT_FALSE(dut->tb_psdram_line_done);
    }
    long elapsed = g_clocks - start;
    ASSERT_EQ(dut->tb_psdram_sref_clocks, slept);
    ASSERT_GT((long)(dut->tb_psdram_refreshes - refreshed), elapsed / 504);
    /* A line is at most 22 clocks with a refresh in front of it. */
    ASSERT_LT(elapsed, 6000L * 22);

    for (int i = 0; i < 4000; i++)
        clock_cycle();
    slept = dut->tb_psdram_sref_clocks;
    refreshed = dut->tb_psdram_refreshes;
    for (int i = 0; i < 4000; i++)
        clock_cycle();
    ASSERT_EQ(dut->tb_psdram_sref_clocks - slept, 4000u);
    ASSERT_EQ(dut->tb_psdram_refreshes, refreshed);

    for (auto &e : ref)
    {
        line_t got;
        ASSERT_TRUE(line_rd(e.first, got));
        ASSERT_TRUE(same(got, e.second));
    }
}

/* Lines that arrive after every length of idle, which meets the idle
 * close, the refresh and the self refresh at every offset. */
UTEST(psdram, sporadic_lines)
{
    const uint32_t lo = 0x000040u;  /* bank 0 */
    const uint32_t hi = 0x3E0000u;  /* byte 0x3E00000, bank 3 */
    std::map<uint32_t, line_t> ref;
    for (uint32_t i = 0; i < 12; i++)
    {
        ASSERT_TRUE(line_wr(lo + i * 0x81u, ref[lo + i * 0x81u] = random_line()));
        ASSERT_TRUE(line_wr(hi + i, ref[hi + i] = random_line()));
    }
    for (int g = 0; g < 420; g++)
    {
        uint32_t i = (uint32_t)g % 12;
        line_t got;
        idle_for(g);
        ASSERT_TRUE(line_rd(lo + i * 0x81u, got));
        ASSERT_TRUE(same(got, ref[lo + i * 0x81u]));
        idle_for(g);
        if (g & 1)
        {
            ASSERT_TRUE(line_wr(hi + i, ref[hi + i] = random_line()));
        }
        else
        {
            ASSERT_TRUE(line_rd(hi + i, got));
            ASSERT_TRUE(same(got, ref[hi + i]));
        }
    }
}

/* A line wakes the controller, and a self refresh that a line ends is
 * no shorter than one that a word read ends. */
UTEST(psdram, line_self_refresh_residency)
{
    line_t a = random_line(), b = random_line(), got;
    ASSERT_TRUE(line_wr(0x000020u, a));
    ASSERT_TRUE(line_wr(0x000120u, b));
    int worst = 1 << 30;
    for (int k = -8; k <= 10; k++)
    {
        ASSERT_TRUE(line_rd(0x000020u, got));
        uint32_t s0 = dut->tb_psdram_sref_clocks;
        int guard = 0;
        while (dut->tb_psdram_sref_clocks == s0 && guard++ < 2000)
            idle_for(1);
        ASSERT_LT(guard, 2000);
        if (k < 0)
        {
            int e = guard;
            ASSERT_TRUE(line_rd(0x000020u, got));
            s0 = dut->tb_psdram_sref_clocks;
            idle_for(e + k);
        }
        else
            idle_for(k);
        if (k & 1)
        {
            ASSERT_TRUE(line_wr(0x000120u, b = random_line()));
        }
        ASSERT_TRUE(line_rd(0x000120u, got));
        ASSERT_TRUE(same(got, b));
        int total = (int)(dut->tb_psdram_sref_clocks - s0);
        if (total > 0 && total < worst)
            worst = total;
    }
    ASSERT_GE(worst, 3);
    ASSERT_LT(worst, 1 << 30);

    /* A line left pending after it is done keeps the controller awake,
     * as a word read left pending does. */
    line_start(0x000020u, false, line_t{});
    uint32_t s0 = 0;
    for (int i = 0; i < 3000; i++)
    {
        clock_cycle();
        if (i == 100)
        {
            ASSERT_TRUE(dut->tb_psdram_line_done);
            s0 = dut->tb_psdram_sref_clocks;
        }
    }
    ASSERT_EQ(dut->tb_psdram_sref_clocks, s0);
    ASSERT_TRUE(same(line_result(), a));
    dut->line_pend = 0;
    clock_cycle();
}

/* The line a read holds is the read client's answer for as long as it
 * is asked for. A line read leaves it alone, a line written elsewhere
 * leaves it alone, and a line written over it replaces it. */
UTEST(psdram, held_read_vs_lines)
{
    const uint32_t line = 0x155AA5u;
    const uint32_t other = 0x155AA6u;
    line_t l = random_line(), o = random_line(), got;
    ASSERT_TRUE(line_wr(line, l));
    ASSERT_TRUE(line_wr(other, o));

    for (uint32_t k = 0; k < 8; k++)
    {
        uint32_t a = line << 3 | k;
        ASSERT_EQ(rd(a), l.w[k]);
        ask(a);
        dut->eval();
        ASSERT_TRUE(dut->tb_psdram_rvalid);

        /* Lines read and written elsewhere, and this line read: the
         * held word stays valid on every clock. */
        for (int step = 0; step < 4; step++)
        {
            if (step == 0)
                line_start(other, false, line_t{});
            else if (step == 1)
                line_start(other, true, o = random_line());
            else if (step == 2)
                line_start(line, false, line_t{});
            else
                line_start(0x2AA55Au, true, random_line());
            int guard = 0;
            while (!dut->tb_psdram_line_done && guard++ < 2000)
            {
                ASSERT_TRUE(dut->tb_psdram_rvalid);
                ASSERT_EQ(half(), l.w[k]);
                clock_cycle();
            }
            ASSERT_TRUE(dut->tb_psdram_line_done);
            ASSERT_TRUE(dut->tb_psdram_rvalid);
            ASSERT_EQ(half(), l.w[k]);
            if (step == 0)
                ASSERT_TRUE(same(line_result(), o));
            if (step == 2)
                ASSERT_TRUE(same(line_result(), l));
            dut->line_pend = 0;
            clock_cycle();
        }

        /* This line written: the held word is dropped as the line is
         * taken, and the read client then answers with the new word
         * and never again with the old one. */
        line_t fresh = random_line();
        fresh.w[k] = (uint16_t)~l.w[k];
        line_start(line, true, fresh);
        bool dropped = false;
        int guard = 0;
        while (!(dropped && dut->tb_psdram_rvalid) && guard++ < 2000)
        {
            if (!dut->tb_psdram_rvalid)
                dropped = true;
            if (dut->tb_psdram_line_done)
                dut->line_pend = 0;
            clock_cycle();
        }
        ASSERT_TRUE(dropped);
        ASSERT_TRUE(dut->tb_psdram_rvalid);
        ASSERT_EQ(half(), fresh.w[k]);
        ASSERT_FALSE(dut->line_pend);
        dut->rd_pend = 0;
        clock_cycle();
        l = fresh;
    }

    /* A read and a write of its line asked for on the same clock: the
     * read may be answered first with the old word, and once the line
     * is done the answer is the new word. */
    for (uint32_t k = 0; k < 8; k++)
    {
        uint32_t a = line << 3 | k;
        (void)rd(other << 3);
        line_t fresh = random_line();
        fresh.w[k] = (uint16_t)~l.w[k];
        ask(a);
        line_start(line, true, fresh);
        int guard = 0;
        while (!dut->tb_psdram_line_done && guard++ < 2000)
        {
            if (dut->tb_psdram_rvalid)
                ASSERT_EQ(half(), l.w[k]);
            clock_cycle();
        }
        ASSERT_TRUE(dut->tb_psdram_line_done);
        dut->line_pend = 0;
        guard = 0;
        do
            clock_cycle();
        while (!dut->tb_psdram_rvalid && guard++ < 2000);
        ASSERT_EQ(half(), fresh.w[k]);
        dut->rd_pend = 0;
        clock_cycle();
        l = fresh;
    }
    ASSERT_TRUE(line_rd(line, got));
    ASSERT_TRUE(same(got, l));
    ASSERT_TRUE(line_rd(other, got));
    ASSERT_TRUE(same(got, o));

    /* A line that was read stays on the port while the word clients
     * work, on this line and elsewhere. */
    for (uint32_t k = 0; k < 8; k++)
    {
        wr(other << 3 | k, (uint16_t)~o.w[k]);
        wr(0x0AA550u + k, (uint16_t)rng());
        ASSERT_EQ(rd(other << 3 | k), (uint16_t)~o.w[k]);
        ASSERT_EQ(rd(line << 3 | k), l.w[k]);
        ASSERT_TRUE(same(line_result(), o));
    }
}

/* Both words of the pair the read client answers with. */
static bool pair_is(const std::map<uint32_t, uint16_t> &ref)
{
    uint32_t a = dut->rd_addr << 1;
    return dut->tb_psdram_rword
           == (uint32_t)(ref.at(a) | (uint32_t)ref.at(a + 1) << 16);
}

/* One visit fetches a line, and every word of it is then answered on
 * the clock it is asked for. The lines on either side are not held. */
UTEST(psdram, a_visit_holds_the_whole_line)
{
    const uint32_t lines[] = {0x0AB3C1u, 0x0AB3FFu, 0x2AB380u, 0x3FFFFFu};
    std::map<uint32_t, uint16_t> ref;
    for (uint32_t line : lines)
        for (uint32_t k = 0; k < 8; k++)
            wr(line << 3 | k, ref[line << 3 | k] = (uint16_t)rng());
    for (uint32_t line : lines)
        for (uint32_t first = 0; first < 8; first++)
        {
            (void)rd(0x155000u);
            ASSERT_EQ(rd(line << 3 | first), ref[line << 3 | first]);
            long at = g_clocks;
            for (uint32_t k = 0; k < 8; k++)
            {
                ask(line << 3 | (k * 3 & 7));
                dut->eval();
                ASSERT_TRUE(dut->tb_psdram_rvalid);
                ASSERT_EQ(half(), ref[line << 3 | (k * 3 & 7)]);
                ASSERT_TRUE(pair_is(ref));
            }
            ASSERT_EQ(g_clocks, at);
            ask((line - 1) << 3 | 7);
            dut->eval();
            ASSERT_FALSE(dut->tb_psdram_rvalid);
            ask((line + 1) << 3 & 0x1FFFFFFu);
            dut->eval();
            ASSERT_FALSE(dut->tb_psdram_rvalid);
            dut->rd_pend = 0;
            clock_cycle();
        }
}

/* A write taken from the write client drops the held line, whatever the
 * write's address: on the clock the take shows, the read client has no
 * answer, and its next answer was read after the write. The write is
 * asked for at every offset from the read, so it is taken with the line
 * held, with the line being read, and on the clock the line lands. */
UTEST(psdram, any_write_drops_the_held_line)
{
    const uint32_t line = 0x0C5A31u;
    const uint32_t targets[] = {
        line << 3 | 2,        /* the word asked for */
        line << 3 | 3,        /* its pair */
        line << 3 | 7,        /* the same line */
        (line - 1) << 3 | 7,  /* the line below */
        (line + 1) << 3,      /* the line above */
        (line + 0x80) << 3,   /* another row */
        0x1F00000u,           /* another bank */
    };
    std::map<uint32_t, uint16_t> ref;
    for (uint32_t t : targets)
        wr(t, ref[t] = (uint16_t)rng());
    for (uint32_t k = 0; k < 8; k++)
        wr(line << 3 | k, ref[line << 3 | k] = (uint16_t)rng());

    long with_line = 0, in_flight = 0;
    for (uint32_t t : targets)
        for (int off = -6; off < 40; off++)
        {
            /* A negative offset asks for the write with the line held
             * and the read still asking. Otherwise the line is held
             * elsewhere and the write comes off clocks into the read. */
            if (off < 0)
                (void)rd(line << 3 | 2);
            else
                (void)rd(0x155000u);
            idle_for(off < -3 ? 300 : 3);
            ask(line << 3 | 2);
            uint16_t v = (uint16_t)(ref[t] + 1 + (rng() & 0xFF));
            bool taken = false, answered = false;
            for (int c = 0; !(taken && answered); c++)
            {
                ASSERT_LT(c, 400);
                if (c == (off < 0 ? 0 : off))
                {
                    dut->w_avail = 1;
                    dut->w_addr = t;
                    dut->w_data = v;
                }
                dut->eval();
                if (dut->tb_psdram_wtake)
                {
                    ASSERT_FALSE(taken);
                    taken = true;
                    ref[t] = v;
                    dut->w_avail = 0;
                    ASSERT_FALSE(dut->tb_psdram_rvalid);
                }
                else if (dut->tb_psdram_rvalid)
                {
                    ASSERT_TRUE(pair_is(ref));
                    answered = taken;
                    if (!taken && dut->w_avail)
                        with_line++;
                }
                else if (!taken && dut->w_avail)
                    in_flight++;
                clock_cycle();
            }
            dut->rd_pend = 0;
            clock_cycle();
        }
    /* Both windows were met. */
    ASSERT_GT(with_line, 20L);
    ASSERT_GT(in_flight, 200L);
    for (auto &e : ref)
        ASSERT_EQ(rd(e.first), e.second);
}

/* The three clients work the same few lines at once. Every answer of
 * the read client is compared, both words, with what the chip held on
 * that clock: a write counts from the clock its take shows, when the
 * read client must have no answer, and a line written counts from the
 * clock it is done. A write and a line are kept off the same line while
 * both are asked for, since the order they are taken in does not show. */
UTEST(psdram, the_held_line_is_never_stale)
{
    const uint32_t pool[] = {0x0D1240u, 0x0D1241u, 0x0D1242u, 0x0D127Fu,
                             0x0D7740u, 0x3A0001u};
    const int LINES = (int)(sizeof(pool) / sizeof(pool[0]));
    std::map<uint32_t, uint16_t> ref;
    for (int i = 0; i < LINES; i++)
    {
        line_t l = random_line();
        ASSERT_TRUE(line_wr(pool[i], l));
        for (uint32_t k = 0; k < 8; k++)
            ref[pool[i] << 3 | k] = l.w[k];
    }

    long answers = 0, at_once = 0, takes = 0, lines = 0;
    bool fresh_ask = false;
    line_t lw = line_t{};
    int quiet = 0;
    uint32_t slept = dut->tb_psdram_sref_clocks;
    for (long c = 0; c < 400000; c++)
    {
        dut->eval();
        if (dut->tb_psdram_wtake)
        {
            takes++;
            ref[dut->w_addr] = dut->w_data;
            dut->w_avail = 0;
            ASSERT_FALSE(dut->tb_psdram_rvalid);
        }
        else if (dut->rd_pend && dut->tb_psdram_rvalid)
        {
            ASSERT_TRUE(pair_is(ref));
            answers++;
            if (fresh_ask)
                at_once++;
            /* Half of the reads go on to the next pair, as a copy
             * does, into the next line at the end of this one. */
            uint32_t next = (dut->rd_addr + 1) << 1;
            if ((rng() & 1) && ref.count(next))
            {
                ask(next | (rng() & 1));
                dut->eval();
                fresh_ask = true;
                clock_cycle();
                continue;
            }
            dut->rd_pend = 0;
        }
        fresh_ask = false;
        if (dut->line_pend && dut->tb_psdram_line_done)
        {
            lines++;
            line_t got = line_result();
            for (uint32_t k = 0; k < 8; k++)
            {
                if (dut->line_we)
                    ref[dut->line_addr << 3 | k] = lw.w[k];
                else
                    ASSERT_EQ(got.w[k], ref[dut->line_addr << 3 | k]);
            }
            dut->line_pend = 0;
        }

        /* A few stretches are left quiet for long enough to close the
         * rows and to sleep, with a line held through them. */
        if (quiet)
            quiet--;
        else if (rng() % 3000 == 0)
            quiet = 200 + (int)(rng() % 600);
        uint32_t r = rng();
        if (!quiet && !dut->rd_pend && (r & 3) == 0)
        {
            ask(pool[(r >> 2) % LINES] << 3 | ((r >> 8) & 7));
            dut->eval();
            fresh_ask = true;
        }
        if (!quiet && !dut->w_avail && ((r >> 11) & 15) == 0)
        {
            uint32_t wl = pool[(r >> 15) % LINES];
            if (!(dut->line_pend && dut->line_addr == wl))
            {
                dut->w_avail = 1;
                dut->w_addr = wl << 3 | ((r >> 20) & 7);
                dut->w_data = (uint16_t)rng();
            }
        }
        if (!quiet && !dut->line_pend && !dut->tb_psdram_line_done
            && ((r >> 23) & 15) == 0)
        {
            uint32_t ll = pool[(r >> 27) % LINES];
            if (!(dut->w_avail && dut->w_addr >> 3 == ll))
                line_start(ll, (rng() & 3) == 0, lw = random_line());
        }
        clock_cycle();
    }
    dut->rd_pend = 0;
    dut->w_avail = 0;
    if (dut->line_pend)
        ASSERT_TRUE(line_finish());
    idle_for(2);

    /* Every kind of work was done, the look-ahead answered many reads
     * with no visit, and the controller slept. */
    ASSERT_GT(takes, 5000L);
    ASSERT_GT(lines, 3000L);
    ASSERT_GT(answers, 10000L);
    ASSERT_GT(at_once, answers / 4);
    ASSERT_GT(dut->tb_psdram_sref_clocks, slept);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vtb_psdram;
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
