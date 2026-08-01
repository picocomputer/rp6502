/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * aud_bel against the real bel.c, sample for sample.
 *
 * This coverage used to live inside test_psg, because aud_psg carried a
 * bell and the PSG lockstep swept it up for free. There is one bell for
 * the whole machine now, past the engine mux, so it needs its own
 * lockstep or it would have quietly stopped being checked.
 *
 * Its own is better anyway. The bell went the whole project ringing
 * 2.07x fast on the OPL path — aud_bel's constants were frozen at the
 * PSG's 24 kHz while aud_opl stepped it at 49,704 — and nothing failed,
 * because the only test that drove it drove it at the one rate it was
 * right for. The machine has exactly one bell now and it runs at
 * 48 kHz, so that is the rate elaborated here and the rate this drives.
 *
 * One test, not four, because bel_setup() deliberately does not reset
 * the bell — a rung bell rings through a program stop — so there is no
 * way to hand a second test a quiet C to start from. The phases run in
 * sequence against a single reset, the way test_psg does it.
 */

#include "Vaud_bel.h"
#include "verilated.h"

extern "C"
{
#include "psg_shim.h"
#include "ria/aud/bel.h"
}

#include "utest.h"

UTEST_MAIN();

static Vaud_bel *dut;
static long g_n;

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
    dut = new Vaud_bel;
    dut->clk = 0;
    dut->rst_n = 0;
    dut->strike = 0;
    dut->step = 0;
    dut->eval();
    for (int i = 0; i < 4; i++)
        tick();
    dut->rst_n = 1;
    tick();
    shim_init();
    bel_setup();
    g_n = 0;
}

/* One strike on both machines. bel_add is what the console's BEL scan
 * reaches in the C; strike is the pulse that reaches it here. */
static void ring()
{
    bel_add(&bel_teletype);
    dut->strike = 1;
    tick();
    dut->strike = 0;
}

/* Step both by one sample. The RTL registers its output on the step, so
 * the value is read on the following clock. Returns the pair rather than
 * asserting, because utest's ASSERT_ returns from its enclosing function
 * and the callers want to keep hold of the sample. */
static bool step_pair(uint32_t rate, int16_t *rtl_out)
{
    g_n++;
    const int16_t c = bel_sample(rate);
    dut->step = 1;
    tick();
    dut->step = 0;
    tick();
    const int16_t r = (int16_t)dut->aud_bel_out;
    *rtl_out = r;
    if (r != c)
        fprintf(stderr, "bel diff n=%d rate=%u rtl=%d c=%d\n", g_n, rate, r, c);
    return r == c;
}

static void run(int *utest_result, uint32_t rate, int n)
{
    int16_t v;
    for (int i = 0; i < n; i++)
    {
        const bool agreed = step_pair(rate, &v);
        ASSERT_TRUE(agreed);
    }
}

UTEST(bel, lockstep_bit_exact)
{
    fresh();

    /* Quiet, and quiet has to agree too. */
    run(utest_result, 48000, 50);

    /* One teletype bell. While it is fresh, time the gap between rising
     * zero crossings: bel.c builds the tone from freq/3, so 1760/3 =
     * 586.67 Hz. Timing the gaps rather than counting them in a window
     * matters — the envelope reaches zero partway through and a window
     * wide enough to be comfortable also counts the silence, which an
     * earlier version of this did and read 392.7 Hz off a correct bell.
     * With the constants frozen at 24 kHz it read 1215.3 Hz. */
    ring();
    const int want = 60;
    int seen = 0, prev = 0, n = 0, first = -1, last = -1;
    int16_t v;
    while (n < want && seen < 20000)
    {
        const bool agreed = step_pair(48000, &v);
        ASSERT_TRUE(agreed);
        if (prev <= 0 && v > 0)
        {
            if (first < 0)
                first = seen;
            last = seen;
            n++;
        }
        prev = v;
        seen++;
    }
    ASSERT_EQ(n, want);
    const double hz = (double)(n - 1) * 48000.0 / (double)(last - first);
    fprintf(stderr, "  bel %.1f Hz at 48 kHz\n", hz);
    ASSERT_GT(hz, 560.0);
    ASSERT_LT(hz, 615.0);

    /* Out through attack, decay, sustain, the 20 ms release and the
     * 800 ms end. */
    run(utest_result, 48000, 40000);

    /* Three queued bells restrike at 100 ms strides. */
    ring();
    ring();
    ring();
    run(utest_result, 48000, 8000);

    /* Ten into a ring of eight: the drops have to match too. */
    for (int i = 0; i < 10; i++)
        ring();
    run(utest_result, 48000, 40000);
}
