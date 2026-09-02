/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The machine makes its samples on its own clock as it runs, and the
 * device takes them from what it has made. A handler that writes its own
 * call count is a level the test can read back, so what reaches the
 * device says exactly which samples were made, and in what order.
 */

#include "core/aud/aud_mix.h"
#include "core/dap/dbg.h"
#include "emu_boot.h"

static int g_calls;

static void counting(void)
{
    g_calls++;
    aud_out((int16_t)g_calls, (int16_t)-g_calls);
}

static float g_out[800 * 2];

static float level(int call) { return (float)call / 32768.0f; }

UTEST(render, a_frame_at_the_device_rate_is_a_frame_of_handler_calls)
{
    emu_audio_settle();
    aud_setup(counting, aud_native_rate());
    g_calls = 0;
    emu_frames(2);
    ASSERT_GE(g_calls, 1599); /* a frame is not a whole number of samples */
    ASSERT_LE(g_calls, 1601);
    ASSERT_EQ(aud_render(g_out, 800), 800);
    for (int i = 0; i < 800; i++)
    {
        ASSERT_EQ(g_out[i * 2], level(i + 1));
        ASSERT_EQ(g_out[i * 2 + 1], -level(i + 1));
    }
    aud_stop();
}

/* The OPL2's rate is not the device's: its handler runs at its own rate
 * and what it makes is resampled, so a frame of it is a frame of the
 * device's samples. */
UTEST(render, a_frame_at_the_opl_rate_is_a_frame_of_device_samples)
{
    ASSERT_NE(aud_native_rate(), 49716u);
    emu_audio_settle();
    aud_setup(counting, 49716);
    g_calls = 0;
    emu_frames(2);
    ASSERT_GE(g_calls, 1656);
    ASSERT_LE(g_calls, 1658);
    ASSERT_EQ(aud_render(g_out, 800), 800);
    ASSERT_GT(g_out[799 * 2], g_out[0]); /* a ramp in is a ramp out */
    aud_stop();
}

/* A machine the device has not kept up with -- a stall, a debugger, a test
 * -- keeps AUD_LEAD_FRAMES of what it made and drops the rest, so a burst
 * of frames is not owed as latency. The oldest are the ones kept. */
UTEST(render, the_machine_leads_the_device_by_at_most_the_lead)
{
    emu_audio_settle();
    aud_setup(counting, aud_native_rate());
    g_calls = 0;
    emu_frames(10);
    ASSERT_GE(g_calls, 7999);
    for (int f = 0; f < AUD_LEAD_FRAMES; f++)
    {
        ASSERT_EQ(aud_render(g_out, 800), 800);
        for (int i = 0; i < 800; i++)
            ASSERT_EQ(g_out[i * 2], level(f * 800 + i + 1));
    }
    ASSERT_EQ(aud_render(g_out, 800), 0);
    for (int i = 0; i < 800; i++)
        ASSERT_EQ(g_out[i * 2], level(AUD_LEAD_FRAMES * 800));
    aud_stop();
}

/* Under the debugger the machine makes nothing, and once the device has
 * taken what it had, every sample is the last one -- never silence, which
 * is a click. Let go, and the count picks up where it stopped. */
UTEST(render, a_held_machine_repeats_its_last_level)
{
    emu_audio_settle();
    aud_setup(counting, aud_native_rate());
    g_calls = 0;
    emu_frames(2);
    ASSERT_EQ(aud_render(g_out, 800), 800);

    dbg_note_stop(0);
    ASSERT_TRUE(dbg_is_stopped());
    const int made = g_calls;
    emu_frames(5);
    ASSERT_EQ(g_calls, made);
    int n, taken = 0;
    while ((n = aud_render(g_out, 800)) > 0)
        taken += n;
    ASSERT_EQ(taken, made - 800);
    for (int i = 0; i < 800; i++)
    {
        ASSERT_EQ(g_out[i * 2], level(made));
        ASSERT_EQ(g_out[i * 2 + 1], -level(made));
    }

    dbg_continue();
    emu_frames(AUD_LEAD_FRAMES);
    ASSERT_EQ(aud_render(g_out, 800), 800);
    ASSERT_EQ(g_out[0], level(made + 1));
    aud_stop();
}

UTEST_MAIN_EMU();
