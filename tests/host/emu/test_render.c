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

/* One sink frame at 48000 Hz spans 49716/48000 machine samples, so 800 sink
 * frames take 828 or 829 calls, depending on the resampler's phase. */
UTEST(render, a_sink_frame_is_a_frame_of_calls_at_the_machine_rate)
{
    aud_setup_probe(counting);
    g_calls = 0;
    ASSERT_EQ(aud_render(g_out, 800), 800);
    ASSERT_GE(g_calls, 828);
    ASSERT_LE(g_calls, 829);
    /* When another case runs first, as it can under utest's --random-order,
     * the resampler's filter history still holds that case's samples, so the
     * ramp is checked well past the start of the buffer. */
    ASSERT_GT(g_out[799 * 2], g_out[400 * 2]);
    ASSERT_LT(g_out[799 * 2 + 1], g_out[400 * 2 + 1]);
    aud_stop();
}

UTEST(render, nothing_is_made_until_the_sink_asks)
{
    aud_setup_probe(counting);
    g_calls = 0;
    emu_frames(10);
    ASSERT_EQ(g_calls, 0);
    ASSERT_EQ(aud_render(g_out, 800), 800);
    ASSERT_GE(g_calls, 828);
    aud_stop();
}

UTEST(render, a_held_machine_repeats_its_last_level)
{
    ASSERT_TRUE(emu_restart(TEST_FIXTURE));
    aud_setup_probe(counting);
    g_calls = 0;
    ASSERT_EQ(aud_render(g_out, 800), 800);
    const float last_l = g_out[799 * 2];
    const float last_r = g_out[799 * 2 + 1];

    dbg_set_active(true);
    dbg_request_pause();
    emu_frames(1);
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
    dbg_set_active(false);
    sys_stop();
    sys_commit();
    aud_stop();
}

UTEST_MAIN_EMU();
