/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
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

/* A frame is 828.6 samples at the machine's rate, so it makes 828 or 829
 * calls, and 800 frames at the sink's 48000 Hz give or take the resampler's
 * phase. */
UTEST(render, a_frame_is_a_frame_of_calls_at_the_machine_rate)
{
    aud_setup_probe(counting);
    emu_frames(1);
    aud_render(g_out, 800);
    g_calls = 0;
    emu_frames(1);
    const int n = aud_render(g_out, 800);
    ASSERT_GE(g_calls, 828);
    ASSERT_LE(g_calls, 829);
    ASSERT_GE(n, 799);
    /* The resampler's filter history holds the samples before the probe, so
     * the ramp is checked well past the start of the buffer. */
    ASSERT_GT(g_out[(n - 1) * 2], g_out[400 * 2]);
    ASSERT_LT(g_out[(n - 1) * 2 + 1], g_out[400 * 2 + 1]);
    aud_stop();
}

/* Frames the sink does not take are made all the same, and dropped. */
UTEST(render, the_machine_makes_its_sound_whether_or_not_the_sink_asks)
{
    aud_setup_probe(counting);
    emu_frames(1);
    aud_render(g_out, 800);
    g_calls = 0;
    emu_frames(10);
    ASSERT_GE(aud_render(g_out, 800), 799);
    ASSERT_EQ(g_calls, 8286);
    aud_stop();
}

UTEST(render, a_held_machine_repeats_its_last_level)
{
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));
    aud_setup_probe(counting);
    g_calls = 0;
    emu_frames(1);
    ASSERT_GE(aud_render(g_out, 800), 799);
    const float last_l = g_out[799 * 2];
    const float last_r = g_out[799 * 2 + 1];

    dbg_set_active(true);
    dbg_request_pause();
    emu_frames(1);
    ASSERT_TRUE(dbg_is_stopped());
    const int made = g_calls;
    ASSERT_EQ(aud_render(g_out, 800), 0);
    emu_frames(1);
    ASSERT_EQ(g_calls, made);
    for (int i = 0; i < 800; i++)
    {
        ASSERT_EQ(g_out[i * 2], last_l);
        ASSERT_EQ(g_out[i * 2 + 1], last_r);
    }

    dbg_continue();
    emu_frames(1);
    ASSERT_GE(aud_render(g_out, 800), 799);
    ASSERT_GT(g_calls, made);
    dbg_set_active(false);
    sys_stop();
    sys_commit();
    aud_stop();
}

UTEST_MAIN_EMU();
