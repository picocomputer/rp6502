/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The staging controller soaked against the behavioral chip: a
 * sequential load the size the bridge streams, then randomized reads
 * and writes with read-after-write on the held halfword, the model's
 * $fatal protocol floors armed throughout, and the refresh cadence
 * counted against the wall clock at the end.
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

static uint16_t rd(uint32_t addr)
{
    dut->rd_pend = 1;
    dut->rd_addr = addr;
    dut->eval();
    int guard = 0;
    while (!dut->tb_psdram_rvalid && guard++ < 2000)
        clock_cycle();
    uint16_t v = dut->tb_psdram_rdata;
    dut->rd_pend = 0;
    clock_cycle();
    return v;
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

    /* The bridge's shape: a sequential stream across page and row
     * boundaries, including a bank seam. */
    for (uint32_t i = 0; i < 3000; i++)
    {
        uint32_t a = 0x7FF400u + i; /* crosses rows within bank 0 */
        uint16_t v = (uint16_t)rng();
        wr(a, v);
        ref[a] = v;
    }

    /* Random traffic over sparse addresses in all four banks. */
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

    /* Read-after-write on the held halfword: the hold must drop. */
    uint32_t a = 0x123456u;
    wr(a, 0xAAAA);
    ASSERT_EQ(rd(a), 0xAAAA);
    wr(a, 0x5555); /* invalidates the hold */
    ASSERT_EQ(rd(a), 0x5555);

    /* Repeat fetch inside the hold costs nothing: rvalid immediate. */
    dut->rd_pend = 1;
    dut->rd_addr = a;
    dut->eval();
    ASSERT_TRUE(dut->tb_psdram_rvalid);
    dut->rd_pend = 0;

    /* Sequential verify of the streamed block. */
    for (uint32_t i = 0; i < 3000; i += 37)
        ASSERT_EQ(rd(0x7FF400u + i), ref[0x7FF400u + i]);

    /* Refresh cadence: at least one per 504 clocks on average
     * (7.7 us at 50.4 MHz is one per 390). */
    long elapsed = g_clocks - soak_start;
    ASSERT_GT((long)dut->tb_psdram_refreshes, elapsed / 504);

    /* Left alone, the store sleeps. This is the whole low-power case
     * and it has to be asserted rather than assumed: the machine's
     * pseudo-ROM is idle almost all the time, and awake it draws
     * fifty times what self refresh does. */
    uint32_t slept_before = dut->tb_psdram_sref_clocks;
    uint32_t refreshed_before = dut->tb_psdram_refreshes;
    for (int i = 0; i < 4000; i++)
        clock_cycle();
    ASSERT_GT((long)(dut->tb_psdram_sref_clocks - slept_before), 3000L);
    /* And it refreshes itself while it sleeps — the controller must not
     * be issuing any, or it never really went under. */
    ASSERT_LT((long)(dut->tb_psdram_refreshes - refreshed_before), 3L);

    /* Waking is the other half. The array survived, and the next access
     * reads what was written before the nap. */
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

/* Runs after load_soak_and_refresh, so the chip is already awake and
 * initialised; the controller has no reset port and must not be re-woken. */
UTEST(psdram, sporadic_asset_reads)
{
    std::map<uint32_t, uint16_t> ref;
    const uint32_t rom = 0x000000u;  /* STAGE+0, bank 0: the ROM image */
    const uint32_t oem = 0x1FE8000u; /* byte 0x3FD0000, bank 3: OEMCP */
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

/* Per-trial self-calibration: idle until the chip is observed to go
 * under, then wait k more clocks and ask.  Immune to refresh phase. */
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
        ASSERT_LT(guard, 2000); /* it did go under */
        if (k < 0)
        {
            /* Re-run the same idle, stopping k clocks short of entry, so
             * the ask lands in the entry window itself. */
            int e = guard;
            (void)rd(0x000100u);
            s0 = dut->tb_psdram_sref_clocks;
            idle_for(e + k);
        }
        else
            idle_for(k);
        dut->rd_pend = 1;
        dut->rd_addr = 0x000900u;
        dut->eval();
        int n = 0;
        while (!dut->tb_psdram_rvalid && n++ < 300)
            clock_cycle();
        ASSERT_EQ((int)dut->tb_psdram_rdata, 0x2222);
        dut->rd_pend = 0;
        clock_cycle();
        int total = (int)(dut->tb_psdram_sref_clocks - s0);
        if (total > 0 && total < worst)
            worst = total;
    }
    ASSERT_GE(worst, 3); /* tRAS min 48 ns is the floor a nap must clear */
}

/* A read the hold already answers is not a reason to stay awake, so the
 * store may sleep with rd_pend standing. What it must not do is treat
 * that same request as a reason to wake, because then it sleeps and wakes
 * on one signal forever, and every nap is shorter than the tRAS the chip
 * needs to finish an internal refresh — a residency the model checks but
 * only for naps it is actually given. */
UTEST(psdram, held_read_vs_sleep)
{
    const uint32_t a = 0x000100u;
    wr(a, 0x1111);
    ASSERT_EQ(rd(a), 0x1111);

    dut->rd_pend = 1;
    dut->rd_addr = a; /* already held: rvalid stands, no fetch wanted */
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
