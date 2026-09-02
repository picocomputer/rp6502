/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * aud_render: the device's buffer, filled from the registers as they stand.
 * A handler that writes its own call count is a level the test can read
 * back, so what reaches the buffer says exactly which calls were made.
 */

#include "core/aud/aud_mix.h"
#include "core/dap/dbg.h"
#include "emu_boot.h"

#include <string.h>

static int g_calls;

static void counting(void)
{
    g_calls++;
    aud_out((int16_t)g_calls, (int16_t)-g_calls);
}

/* The same handler under another name: the resampler starts over when the
 * handler changes, which is how one run is kept from the next. */
static int g_again;

static void counting_again(void)
{
    g_again++;
    counting();
}

static float g_out[800 * 2];

UTEST(render, a_sample_at_the_device_rate_is_one_handler_call)
{
    aud_setup(counting, aud_native_rate());
    g_calls = 0;
    aud_render(g_out, 800);
    ASSERT_EQ(g_calls, 800);
    for (int i = 0; i < 800; i++)
    {
        ASSERT_EQ(g_out[i * 2], (float)(i + 1) / 32768.0f);
        ASSERT_EQ(g_out[i * 2 + 1], (float)-(i + 1) / 32768.0f);
    }
    aud_stop();
}

/* What one push yields past the end of a buffer opens the next one: the
 * stream is the same whether the device asks for it in one piece or two. */
UTEST(render, a_split_render_is_the_same_stream)
{
    ASSERT_NE(aud_native_rate(), 49716u); /* the OPL2's rate, so this resamples */
    static float whole[800 * 2], parts[800 * 2];
    const int splits[] = {1, 100, 401, 799};
    for (size_t s = 0; s < sizeof splits / sizeof *splits; s++)
    {
        const int a = splits[s], b = 800 - a;
        aud_setup(counting, 49716);
        g_calls = 0;
        aud_render(whole, 800);
        const int calls_whole = g_calls;

        aud_setup(counting_again, 49716);
        g_calls = 0;
        aud_render(parts, a);
        aud_render(parts + a * 2, b);
        ASSERT_EQ(g_calls, calls_whole);
        ASSERT_EQ(memcmp(whole, parts, sizeof whole), 0);
    }
    aud_stop();
}

/* Under the debugger the handler does not run and the level stands. */
UTEST(render, a_held_machine_repeats_its_last_level)
{
    aud_setup(counting, aud_native_rate());
    g_calls = 0;
    aud_render(g_out, 800);
    ASSERT_EQ(g_calls, 800);

    dbg_note_stop(0);
    ASSERT_TRUE(dbg_is_stopped());
    aud_render(g_out, 800);
    ASSERT_EQ(g_calls, 800);
    for (int i = 0; i < 800; i++)
    {
        ASSERT_EQ(g_out[i * 2], 800.0f / 32768.0f);
        ASSERT_EQ(g_out[i * 2 + 1], -800.0f / 32768.0f);
    }

    dbg_continue();
    aud_render(g_out, 800);
    ASSERT_EQ(g_calls, 1600);
    aud_stop();
}

UTEST_MAIN_EMU();
