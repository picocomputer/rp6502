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
