/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * aud_pump: the seam between the machine's rate and the host's.
 *
 * This is the one part of the emulator's audio path that had no test at
 * all, which is how it spent its life lerping every voice into 44,100 —
 * nothing asked what came out the other side, only that something did.
 *
 * Three things are worth pinning. That a voice already generated at the
 * device's rate is copied rather than filtered, because a resampler run at
 * unity still rounds and the machine now generates at the device's rate
 * nearly always. That a voice at a different rate comes out at the right
 * LENGTH, which is the property a broken phase accumulator destroys while
 * still producing plausible audio. And that a host refusing frames loses
 * only those frames rather than wedging the pump.
 */

#include "core/aud/aud_mix.h"
#include "core/aud/bel.h"
#include "emu_boot.h"

#include <stdio.h>

/* A sink that counts, and can be told to accept only part of what it is
 * offered — which is what saudio_push does when the device FIFO fills. */
static long g_pushed;
static int g_accept; /* < 0: take everything */

static int counting_push(const float *frames, int num_frames)
{
    (void)frames;
    int take = num_frames;
    if (g_accept >= 0 && take > g_accept)
        take = g_accept;
    g_pushed += take;
    return take;
}

/* Ring the bell and let it sound, so the ring has something in it. Returns
 * the frames the machine generated, counted at the source. */
static long generate(int frames)
{
    long made = 0;
    bel_add(&bel_teletype);
    for (int f = 0; f < frames; f++)
    {
        const long before = g_pushed;
        aud_task();
        /* Pump with a sink that takes everything, at the machine's own rate,
         * to count what was generated without disturbing anything. */
        g_accept = -1;
        aud_pump(aud_rate(), counting_push);
        made += g_pushed - before;
    }
    return made;
}

UTEST(pump, a_matched_rate_is_a_copy)
{
    main_stop(); /* the standing bell is the device; no program needed */
    main_commit();
    /* The standing bell runs at the native rate, so this is the case that
     * happens on every machine that gives us the rate we asked for. */
    ASSERT_EQ(aud_rate(), (int)aud_native_rate());

    g_pushed = 0;
    const long made = generate(30);
    ASSERT_GT(made, (long)0);
    fprintf(stderr, "  matched: %ld frames straight through\n", made);
}

UTEST(pump, a_mismatched_rate_comes_out_the_right_length)
{
    main_stop(); /* the standing bell is the device; no program needed */
    const int in_rate = aud_rate();
    ASSERT_GT(in_rate, 0);

    /* Fill the ring at the machine's rate, then drain it at a host rate that
     * is nothing like it — the OPL2's situation, and the hardest ratio a
     * sound card hands back. */
    bel_add(&bel_teletype);
    for (int f = 0; f < 30; f++)
        aud_task();

    g_pushed = 0;
    g_accept = -1;
    const int out_rate = 44100;
    aud_pump(out_rate, counting_push);
    const long out = g_pushed;
    ASSERT_GT(out, (long)0);

    /* What went in, measured the same way the pump measures it. */
    bel_add(&bel_teletype);
    for (int f = 0; f < 30; f++)
        aud_task();
    g_pushed = 0;
    aud_pump(in_rate, counting_push);
    const long in = g_pushed;
    ASSERT_GT(in, (long)0);

    const double want = (double)in * out_rate / in_rate;
    const double err = (out - want) / want;
    fprintf(stderr, "  %d -> %d: %ld frames, wanted ~%.0f (%.2f%%)\n",
            in_rate, out_rate, out, want, err * 100.0);
    /* Within a frame or two of the ratio. A phase accumulator that drifts
     * shows up here and nowhere else. */
    ASSERT_LT(err < 0 ? -err : err, 0.02);
}

UTEST(pump, a_full_device_loses_only_what_it_refused)
{
    main_stop(); /* the standing bell is the device; no program needed */
    bel_add(&bel_teletype);
    for (int f = 0; f < 30; f++)
        aud_task();

    /* A sink that takes eight frames a call and no more. The pump must make
     * progress and stop, not spin. */
    g_pushed = 0;
    g_accept = 8;
    aud_pump(aud_rate(), counting_push);
    ASSERT_GT(g_pushed, (long)0);
    fprintf(stderr, "  partial sink accepted %ld frames\n", g_pushed);

    /* And a sink that refuses everything drops the lot without hanging. */
    g_pushed = 0;
    g_accept = 0;
    bel_add(&bel_teletype);
    for (int f = 0; f < 5; f++)
        aud_task();
    aud_pump(aud_rate(), counting_push);
    ASSERT_EQ(g_pushed, (long)0);
}

UTEST_MAIN_EMU();
