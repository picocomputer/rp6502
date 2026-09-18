/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "Vpocket_fifo.h"

#include "utest.h"

#include <deque>

static Vpocket_fifo *dut;

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

    if (dut)
    {
        dut->final();
        delete dut;
    }
    dut = new Vpocket_fifo;
    dut->w_stb = 0;
    dut->r_take = 0;
    dut->wclk = 0;
    dut->rclk = 0;
    dut->eval();
    for (int i = 0; i < 4; i++)
    {
        dut->wclk = 1;
        dut->rclk = 1;
        dut->eval();
        dut->wclk = 0;
        dut->rclk = 0;
        dut->eval();
    }

    long wnext = wperiod, rnext = rperiod;
    for (long t = 0; pushed < 4000 || !ref.empty(); t++)
    {
        bool wedge = t == wnext;
        bool redge = t == rnext;
        if (!wedge && !redge)
            continue;

        if (wedge)
        {
            bool want = pushed < 4000 && (rng() & 3) != 0;
            dut->w_stb = want;
            dut->w_data = next_val;
        }
        if (redge)
            dut->r_take = (rng() & 7) != 0;

        /* The flags and rdata are read before the edge, because the edge
         * updates the pointers they are computed from. */
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
            ASSERT_LT((int)ref.size(), 8);
            ref.push_back(next_val);
            next_val = next_val * 2654435761u + 1;
            pushed++;
        }
        if (r_acc)
        {
            ASSERT_FALSE(ref.empty());
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
    /* The FIFO in pocket_bridge is written at 74.25 MHz and read at
     * 50.4 MHz. The ratio of 74.25 MHz to 50.4 MHz is 330:224, so the
     * writer's period is 224 and the reader's is 330. */
    run_scenario(utest_result, 224, 330, 0xC0FFEE04u);
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
