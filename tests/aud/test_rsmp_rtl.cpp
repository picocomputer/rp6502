/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * aud_rsmp against the real rsmp.c, sample for sample.
 *
 * test_rsmp measures whether the filter is any good; this measures whether
 * the fabric copies it exactly. Two different questions, and only the
 * second one can be answered by comparison — the same way test_psg holds
 * aud_psg to psg.c while nothing in that pair says the PSG sounds right.
 *
 * The interesting part is the phase bookkeeping rather than the multiplies.
 * A resampler whose arithmetic is perfect and whose phase accumulator drifts
 * produces a stream that is the right length and the wrong content, and the
 * only thing that catches it is running both counters side by side for long
 * enough that a one-sample slip has somewhere to show.
 */

#include "Vaud_rsmp.h"
#include "verilated.h"

extern "C"
{
#include "emu/emu/rsmp.h"
}

#include "utest.h"

#include <cstdio>
#include <vector>

UTEST_MAIN();

/* The clock divisors, not the frequencies. rsmp_step just divides one by
 * the other in Q32, and 1050/1014 is the Pocket's ratio exactly where
 * 49704/48000 is only nearly — which is what aud_rsmp is parameterised on,
 * so it is what this has to drive the C with. */
#define POCKET_IN 1050
#define POCKET_OUT 1014
#define POCKET_IN_HZ 49704

static Vaud_rsmp *dut;

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
    dut = new Vaud_rsmp;
    dut->clk = 0;
    dut->rst_n = 0;
    dut->in_valid = 0;
    dut->in_sample = 0;
    dut->eval();
    for (int i = 0; i < 4; i++)
        tick();
    dut->rst_n = 1;
    tick();
}

/* One input into the RTL, and everything it hands back. The engine takes
 * about fifty cycles an output and the inputs are a thousand apart on the
 * real machine, so a hundred and sixty is generous and still bounded. */
static void rtl_push(int16_t x, std::vector<int16_t> &out)
{
    dut->in_sample = x;
    dut->in_valid = 1;
    tick();
    dut->in_valid = 0;
    for (int i = 0; i < 160; i++)
    {
        tick();
        if (dut->aud_rsmp_valid)
            out.push_back((int16_t)dut->aud_rsmp_out);
    }
}

/* The C is not clamped — it answers at full width and lets the platform's
 * sink decide. The fabric IS the sink here, so the comparison clamps the C
 * the way aud_rsmp does rather than pretending neither of them narrows. */
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

    long compared = 0;
    for (int i = 0; i < n_in; i++)
    {
        const int32_t x = gen(i);

        int32_t cbuf[8];
        const int got = rsmp_push(&r, x, step, cbuf, 8);

        std::vector<int16_t> rbuf;
        rtl_push((int16_t)x, rbuf);

        if ((int)rbuf.size() != got)
            fprintf(stderr, "count diff at input %d: rtl=%zu c=%d\n",
                    i, rbuf.size(), got);
        ASSERT_EQ((int)rbuf.size(), got);

        for (int k = 0; k < got; k++)
        {
            const int16_t want = clamp16(cbuf[k]);
            if (rbuf[k] != want)
                fprintf(stderr, "diff at input %d out %d: rtl=%d c=%d\n",
                        i, k, rbuf[k], want);
            ASSERT_EQ(rbuf[k], want);
            compared++;
        }
    }
    fprintf(stderr, "  %ld samples matched\n", compared);
    ASSERT_GT(compared, (long)(n_in / 2));
}

/* Silence has to agree too: a filter with a stuck accumulator passes every
 * signal test and fails this one. */
static int32_t gen_zero(int n) { (void)n; return 0; }

/* A constant exercises the DC constraint on both sides at once. */
static int32_t gen_dc(int n) { (void)n; return 9001; }

/* Full-scale-ish so the sinc's overshoot reaches the clamp on both sides. */
static int32_t gen_sine(int n)
{
    static const double k = 2.0 * 3.14159265358979323846 * 6000.0 / POCKET_IN_HZ;
    return (int32_t)(31000.0 * __builtin_sin(k * n));
}

/* Alternating extremes: the worst case for the ringing, and the one that
 * finds a sign error in a mirrored row. */
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
