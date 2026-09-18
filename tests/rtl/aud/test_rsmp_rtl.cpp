/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "Vrsmp.h"
#include "verilated.h"

extern "C"
{
#include "core/aud/rsmp.h"
}

#include "utest.h"

#include <cstdio>
#include <vector>

UTEST_MAIN();

/* The OPL makes a sample every 1014 clocks of 50.4 MHz and psg_tick fires
 * every 1050. rsmp.sv computes its Q32 step from those two divisors, since
 * 50.4 MHz / 1014 is not a whole number of hertz, and rsmp_step is given
 * the same two numbers so that the C steps by exactly the same amount. */
#define POCKET_IN 1050
#define POCKET_OUT 1014
#define POCKET_IN_HZ 49704

static Vrsmp *dut;

static void tick()
{
    dut->clk = 1;
    dut->eval();
    dut->clk = 0;
    dut->eval();
}

static void fresh()
{
    if (dut)
    {
        dut->final();
        delete dut;
    }
    dut = new Vrsmp;
    dut->clk = 0;
    dut->in_valid = 0;
    dut->in_sample = 0;
    dut->eval();
    for (int i = 0; i < 5; i++)
        tick();
}

/* rsmp_push writes its outputs at full width and rsmp.sv saturates to
 * sixteen bits, so the C output is clamped the same way before it is
 * compared. */
static int16_t clamp16(int32_t v)
{
    if (v < -32768)
        return -32768;
    if (v > 32767)
        return 32767;
    return (int16_t)v;
}

static void lockstep(int *utest_result, int32_t (*gen)(int), int n_in)
{
    fresh();
    rsmp_t r;
    rsmp_reset(&r);
    const uint64_t step = rsmp_step(POCKET_IN, POCKET_OUT);

    std::vector<int16_t> cbuf, rbuf;
    long next_in = 0;
    long next_step = POCKET_OUT;
    int in_i = 0;

    const long clocks = (long)(n_in + 2) * POCKET_OUT;
    for (long t = 0; t < clocks; t++)
    {
        const bool do_in = in_i < n_in && t == next_in;
        const bool do_step = t == next_step;

        dut->in_valid = do_in ? 1 : 0;
        dut->step = do_step ? 1 : 0;
        if (do_in)
            dut->in_sample = (int16_t)gen(in_i);
        tick();
        dut->in_valid = 0;
        dut->step = 0;

        if (do_in)
        {
            int32_t c[8];
            const int got = rsmp_push(&r, gen(in_i), step, c, 8);
            for (int k = 0; k < got; k++)
                cbuf.push_back(clamp16(c[k]));
            in_i++;
            next_in += POCKET_OUT;
        }
        if (do_step)
            next_step += POCKET_IN;
        if (dut->rsmp_valid)
            rbuf.push_back((int16_t)dut->rsmp_out);
    }

    const size_t n = rbuf.size() < cbuf.size() ? rbuf.size() : cbuf.size();
    for (size_t k = 0; k < n; k++)
    {
        if (rbuf[k] != cbuf[k])
            fprintf(stderr, "diff at out %zu: rtl=%d c=%d\n",
                    k, rbuf[k], cbuf[k]);
        ASSERT_EQ(rbuf[k], cbuf[k]);
    }
    fprintf(stderr, "  %zu samples matched (rtl %zu, c %zu)\n",
            n, rbuf.size(), cbuf.size());
    ASSERT_GT((long)n, (long)(n_in / 2));
}

static int32_t gen_zero(int n) { (void)n; return 0; }

static int32_t gen_dc(int n) { (void)n; return 9001; }

static int32_t gen_sine(int n)
{
    static const double k = 2.0 * 3.14159265358979323846 * 6000.0 / POCKET_IN_HZ;
    return (int32_t)(31000.0 * __builtin_sin(k * n));
}

static int32_t gen_square(int n) { return (n / 7) & 1 ? 30000 : -30000; }

UTEST(rsmp_rtl, silence_agrees)
{
    lockstep(utest_result, gen_zero, 200);
}

UTEST(rsmp_rtl, a_constant_agrees)
{
    lockstep(utest_result, gen_dc, 400);
}

UTEST(rsmp_rtl, a_tone_agrees)
{
    lockstep(utest_result, gen_sine, 4000);
}

UTEST(rsmp_rtl, the_hard_edges_agree)
{
    lockstep(utest_result, gen_square, 2000);
}
