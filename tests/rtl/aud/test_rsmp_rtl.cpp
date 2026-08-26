/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * aud_rsmp against rsmp.c, sample for sample.
 *
 * test_rsmp measures whether the filter is any good; this measures whether
 * the two implementations of it agree. Two different questions, and only
 * the second one can be answered by comparison — neither side is the
 * other's reference, and nothing in this pair says the filter sounds right.
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
#include "core/aud/rsmp.h"
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
    dut->in_valid = 0;
    dut->in_sample = 0;
    dut->eval();
    for (int i = 0; i < 5; i++)
        tick();
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

/* Both cadences at once, the way the machine runs them: an OPL sample every
 * 1014 clk_sys and the mixer's tick every 1050, off the same clock.
 *
 * The RTL is pulled and the C is pushed, so there is no per-input count to
 * compare any more — an input and an output are simply different events. The
 * streams are still identical, because both walk the same phase against the
 * same taps in the same order, so the stream is what this compares.
 */
static void lockstep(int *utest_result, int32_t (*gen)(int), int n_in)
{
    fresh();
    rsmp_t r;
    rsmp_reset(&r);
    const uint64_t step = rsmp_step(POCKET_IN, POCKET_OUT);

    std::vector<int16_t> cbuf, rbuf;
    long next_in = 0;
    /* The tick starts a whole input behind, which is the slack the pull side
     * needs: an output may never read further than the inputs have reached. */
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
        if (dut->aud_rsmp_valid)
            rbuf.push_back((int16_t)dut->aud_rsmp_out);
    }

    /* The pull side trails the push side by whatever is still in the history
     * when the clock stops, so the common prefix is what agrees. */
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
