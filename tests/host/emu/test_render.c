/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The sink asks and the mixer fills: every sample handed over was made for
 * that call, pulled through the resampler from the machine's rate to the
 * sink's. A device that writes its own call count is a level the test can
 * read back, so what reaches the sink says exactly how many samples were
 * made, and in what order.
 */

#include "core/aud/mix.h"
#include "core/dap/dbg.h"
#include "emu_boot.h"

static int g_calls;

static void counting(int16_t *l, int16_t *r)
{
    g_calls++;
    *l = (int16_t)g_calls;
    *r = (int16_t)-g_calls;
}

static float g_out[800 * 2];

/* A sink frame at 48000 is 49716/48000 machine samples, so 800 of them is
 * 828 or 829 calls as the resampler's phase carries. */
UTEST(render, a_sink_frame_is_a_frame_of_calls_at_the_machine_rate)
{
    aud_setup(counting);
    g_calls = 0;
    ASSERT_EQ(aud_render(g_out, 800), 800);
    ASSERT_GE(g_calls, 828);
    ASSERT_LE(g_calls, 829);
    /* A ramp in is a ramp out, past the filter's history of whatever the
     * last case left in it. */
    ASSERT_GT(g_out[799 * 2], g_out[400 * 2]);
    ASSERT_LT(g_out[799 * 2 + 1], g_out[400 * 2 + 1]);
    aud_stop();
}

/* Nothing is made between calls. Frames of the machine make no sound on
 * their own; the count moves only when the sink asks. */
UTEST(render, nothing_is_made_until_the_sink_asks)
{
    aud_setup(counting);
    g_calls = 0;
    emu_frames(10);
    ASSERT_EQ(g_calls, 0);
    ASSERT_EQ(aud_render(g_out, 800), 800);
    ASSERT_GE(g_calls, 828);
    aud_stop();
}

/* Under the debugger the machine makes nothing, and every sample is the
 * last one -- never silence, which is a click. Let go, and the count picks
 * up where it stopped. */
UTEST(render, a_held_machine_repeats_its_last_level)
{
    aud_setup(counting);
    g_calls = 0;
    ASSERT_EQ(aud_render(g_out, 800), 800);
    const float last_l = g_out[799 * 2];
    const float last_r = g_out[799 * 2 + 1];

    dbg_note_stop(0);
    ASSERT_TRUE(dbg_is_stopped());
    const int made = g_calls;
    ASSERT_EQ(aud_render(g_out, 800), 0);
    ASSERT_EQ(g_calls, made);
    for (int i = 0; i < 800; i++)
    {
        ASSERT_EQ(g_out[i * 2], last_l);
        ASSERT_EQ(g_out[i * 2 + 1], last_r);
    }

    dbg_continue();
    ASSERT_EQ(aud_render(g_out, 800), 800);
    ASSERT_GT(g_calls, made);
    aud_stop();
}

UTEST_MAIN_EMU();
