/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The platform FIFO under hostile clocking: writer and reader on
 * unrelated periods — fast-to-slow, slow-to-fast, and nearly equal —
 * with randomized bursts against a reference queue. Order and data
 * exact, a push honored under !full never lost, a take under !empty
 * never air, and the depth bound never exceeded.
 */

#include "Vpocket_fifo.h"

#include "utest.h"

#include <deque>

static Vpocket_fifo *dut;

/* Deterministic xorshift, seeded per scenario. */
static uint32_t rng_state;
static uint32_t rng()
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void run_scenario(int *utest_result, int wperiod, int rperiod,
                         uint32_t seed)
{
    rng_state = seed;
    std::deque<uint32_t> ref;
    uint32_t next_val = seed;
    long pushed = 0, popped = 0;

    dut->wrst_n = 0;
    dut->rrst_n = 0;
    dut->w_stb = 0;
    dut->r_take = 0;
    for (int i = 0; i < 4; i++)
    {
        dut->wclk = 1;
        dut->rclk = 1;
        dut->eval();
        dut->wclk = 0;
        dut->rclk = 0;
        dut->eval();
    }
    dut->wrst_n = 1;
    dut->rrst_n = 1;

    long wnext = wperiod, rnext = rperiod;
    for (long t = 0; pushed < 4000 || !ref.empty(); t++)
    {
        bool wedge = t == wnext;
        bool redge = t == rnext;
        if (!wedge && !redge)
            continue;

        /* Inputs settle before the edge, like registered neighbors. */
        if (wedge)
        {
            bool want = pushed < 4000 && (rng() & 3) != 0;
            dut->w_stb = want;
            dut->w_data = next_val;
        }
        if (redge)
            dut->r_take = (rng() & 7) != 0;

        /* The accepted-transaction bookkeeping reads the flags as the
         * edge sees them. */
        bool w_acc = wedge && dut->w_stb && !dut->pocket_fifo_full;
        bool r_acc = redge && dut->r_take && !dut->pocket_fifo_empty;
        uint32_t r_seen = dut->pocket_fifo_rdata;

        if (wedge)
            dut->wclk = 1;
        if (redge)
            dut->rclk = 1;
        dut->eval();
        if (wedge)
        {
            dut->wclk = 0;
            wnext += wperiod;
        }
        if (redge)
        {
            dut->rclk = 0;
            rnext += rperiod;
        }
        dut->eval();

        if (w_acc)
        {
            ASSERT_LT((int)ref.size(), 8); /* never overflows the depth */
            ref.push_back(next_val);
            next_val = next_val * 2654435761u + 1;
            pushed++;
        }
        if (r_acc)
        {
            ASSERT_FALSE(ref.empty()); /* never reads air */
            ASSERT_EQ(r_seen, ref.front());
            ref.pop_front();
            popped++;
        }
        ASSERT_LT(t, 4000000L);
    }
    ASSERT_EQ(pushed, popped);
}

UTEST(fifo, fast_writer_slow_reader)
{
    run_scenario(utest_result, 7, 23, 0xC0FFEE01u);
}

UTEST(fifo, slow_writer_fast_reader)
{
    run_scenario(utest_result, 23, 7, 0xC0FFEE02u);
}

UTEST(fifo, nearly_equal_clocks)
{
    run_scenario(utest_result, 100, 101, 0xC0FFEE03u);
}

UTEST(fifo, true_pocket_ratio)
{
    /* 74.25 MHz against 100.8 MHz is 165:224 — periods 224 and 165. */
    run_scenario(utest_result, 224, 165, 0xC0FFEE04u);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vpocket_fifo;
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
