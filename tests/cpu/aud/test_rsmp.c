/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#define _USE_MATH_DEFINES

#include "core/aud/rsmp.h"
#include "utest.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define POCKET_IN 49704  /* The OPL2 core's sample rate is 50.4 MHz / 1014. */
#define POCKET_OUT 48000 /* The output rate of rsmp.sv is 50.4 MHz / 1050. */

UTEST_MAIN();

static int run(int32_t (*gen)(int n, void *), void *ctx, int n_in,
               uint32_t in_rate, uint32_t out_rate, int32_t *out, int max_out)
{
    rsmp_t r;
    rsmp_reset(&r);
    const uint64_t step = rsmp_step(in_rate, out_rate);
    int n = 0;
    for (int i = 0; i < n_in && n < max_out; i++)
    {
        int32_t buf[8];
        int got = rsmp_push(&r, gen(i, ctx), step, buf, 8);
        for (int k = 0; k < got && n < max_out; k++)
            out[n++] = buf[k];
    }
    return n;
}

struct sine
{
    double hz, rate, amp;
};

static int32_t gen_sine(int n, void *ctx)
{
    const struct sine *s = ctx;
    return (int32_t)lround(s->amp * sin(2.0 * M_PI * s->hz * n / s->rate));
}

static int32_t gen_dc(int n, void *ctx)
{
    (void)n;
    return *(int32_t *)ctx;
}

/* A tone that does not complete a whole number of cycles in the ANA-sample
 * window leaks into the residual that residual_db measures, and the leakage
 * is larger than the resampler's own error: an exact float sine at 100 Hz,
 * analysed at 48 kHz, measures about -53 dB. bin_hz therefore snaps every
 * test tone to an exact bin of the window. */
#define ANA 32768
#define SKIP 256

static double bin_hz(double want, double rate)
{
    const long k = lround(want * ANA / rate);
    return k * rate / ANA;
}

static double power_at(const int32_t *x, double hz, double rate)
{
    double re = 0, im = 0;
    for (int i = 0; i < ANA; i++)
    {
        const double t = 2.0 * M_PI * hz * i / rate;
        re += x[SKIP + i] * cos(t);
        im += x[SKIP + i] * sin(t);
    }
    const double a = 2.0 * re / ANA, b = 2.0 * im / ANA;
    return (a * a + b * b) / 2.0;
}

static double residual_db(const int32_t *x, double hz, double rate)
{
    double re = 0, im = 0;
    for (int i = 0; i < ANA; i++)
    {
        const double t = 2.0 * M_PI * hz * i / rate;
        re += x[SKIP + i] * cos(t);
        im += x[SKIP + i] * sin(t);
    }
    const double a = 2.0 * re / ANA, b = 2.0 * im / ANA;
    double res = 0;
    for (int i = 0; i < ANA; i++)
    {
        const double t = 2.0 * M_PI * hz * i / rate;
        const double e = x[SKIP + i] - (a * cos(t) + b * sin(t));
        res += e * e;
    }
    return 10.0 * log10((res / ANA) / ((a * a + b * b) / 2.0));
}

UTEST(rsmp, the_ratio_is_the_ratio)
{
    ASSERT_EQ(rsmp_step(POCKET_OUT, POCKET_OUT), (uint64_t)1 << 32);

    static int32_t out[200000];
    int32_t dc = 1000;
    const int n_in = 190000;
    const int n = run(gen_dc, &dc, n_in, POCKET_IN, POCKET_OUT, out, 200000);

    const double want = (double)n_in * POCKET_OUT / POCKET_IN;
    ASSERT_LT(fabs(n - want), 2.0);
}

UTEST(rsmp, a_constant_survives_as_itself)
{
    static int32_t out[8192];
    int32_t dc = 12345;
    const int n = run(gen_dc, &dc, 8192, POCKET_IN, POCKET_OUT, out, 8192);
    ASSERT_GT(n, 4000);
    for (int i = 32; i < n; i++)
        ASSERT_EQ(out[i], dc);
}

UTEST(rsmp, a_cold_filter_does_not_click)
{
    static int32_t out[512];
    int32_t dc = 20000;
    const int n = run(gen_dc, &dc, 512, POCKET_IN, POCKET_OUT, out, 512);
    ASSERT_GT(n, 100);
    for (int i = 0; i < n; i++)
        ASSERT_EQ(out[i], dc);
}

UTEST(rsmp, a_tone_survives_the_pocket_ratio)
{
    /* Each limit is 4 to 5 dB above the residual this test measures for
     * that tone. */
    static const struct
    {
        double hz, limit;
    } want[] = {
        {100, -86}, {1000, -81}, {4000, -85}, {8000, -84},
        {12000, -83}, {16000, -84}, {20000, -83},
    };
    static int32_t out[SKIP + ANA + 4096];
    for (unsigned k = 0; k < sizeof want / sizeof *want; k++)
    {
        const double hz = bin_hz(want[k].hz, POCKET_OUT);
        struct sine s = {hz, POCKET_IN, 20000.0};
        const int n_in = (int)((SKIP + ANA + 2048) * (double)POCKET_IN / POCKET_OUT);
        const int n = run(gen_sine, &s, n_in, POCKET_IN, POCKET_OUT, out,
                          SKIP + ANA + 4096);
        ASSERT_GE(n, SKIP + ANA);

        const double db = residual_db(out, hz, POCKET_OUT);
        fprintf(stderr, "  %8.1f Hz: %6.1f dB (want < %.0f)\n", hz, db, want[k].limit);
        ASSERT_LT(db, want[k].limit);
    }
}

UTEST(rsmp, it_handles_the_rates_a_sound_card_hands_back)
{
    static int32_t out[SKIP + ANA + 4096];
    const uint32_t rates[] = {44100, 48000, 96000};
    for (unsigned k = 0; k < sizeof rates / sizeof *rates; k++)
    {
        const double hz = bin_hz(1000.0, rates[k]);
        struct sine s = {hz, 49716.0, 20000.0};
        const int n_in = (int)((SKIP + ANA + 2048) * 49716.0 / rates[k]) + 64;
        const int n = run(gen_sine, &s, n_in, 49716, rates[k], out,
                          SKIP + ANA + 4096);
        ASSERT_GE(n, SKIP + ANA);

        const double db = residual_db(out, hz, rates[k]);
        fprintf(stderr, "  49716 -> %5u: %6.1f dB\n", rates[k], db);
        ASSERT_LT(db, -80.0);
    }
}

UTEST(rsmp, upsampling_is_the_same_machine)
{
    static int32_t out[8192];
    int32_t dc = -4096;
    const int n = run(gen_dc, &dc, 2048, 24000, 96000, out, 8192);
    ASSERT_GT(n, 8000);
    for (int i = 32; i < n; i++)
        ASSERT_EQ(out[i], dc);
}
