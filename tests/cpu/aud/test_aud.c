/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/aud/mix.h"
#include "core/sys/sys.h"
#include "emu_boot.h"

static int g_pos;
static float g_peak;

#define SINK_RATE 48000

#define GATE (512.0f / 32768.0f)

static int g_cross;
static int g_span;
static int g_side;

static void run_frame(void)
{
    static float out[800 * 2];
    int len;
    const float *viz = aud_viz_buffer(&len);
    emu_frames(1);
    aud_render(out, 800);
    const int pos = aud_viz_pos();
    g_peak = 0;
    for (int i = g_pos; i != pos; i = (i + 1) % len)
    {
        const float v = viz[i] < 0 ? -viz[i] : viz[i];
        if (v > g_peak)
            g_peak = v;
        g_span++;
        if (viz[i] > GATE)
        {
            if (g_side < 0)
                g_cross++;
            g_side = 1;
        }
        else if (viz[i] < -GATE)
        {
            if (g_side > 0)
                g_cross++;
            g_side = -1;
        }
    }
    g_pos = pos;
}

static bool load_rom(const char *rom)
{
    if (!emu_restart(rom))
        return false;
    aud_set_sink_rate(SINK_RATE);
    g_pos = aud_viz_pos();
    return true;
}

static void measure_from_here(void)
{
    g_cross = 0;
    g_span = 0;
    g_side = 0;
}

static float measured_hz(void)
{
    if (g_span <= 0)
        return 0.0f;
    return (g_cross / 2.0f) * SINK_RATE / (float)g_span;
}

static int frames_to_sound(int limit)
{
    for (int i = 0; i < limit; i++)
    {
        run_frame();
        if (g_peak > 0)
            return i;
    }
    return -1;
}

static void play_out(int frames)
{
    while (frames-- > 0)
        run_frame();
}

/* One PSG channel at full volume with centred pan peaks at about
 * 32767 * 63 / 128 = 16127, so a working PSG voice clears 4096 by a wide
 * margin. */
#define LOUD (4096.0f / 32768.0f)
#define BELL (32.0f / 32768.0f)

UTEST(aud, psg_makes_a_noise)
{
    ASSERT_TRUE(load_rom(AUD_ROM_PSG));
    const int at = frames_to_sound(8);
    ASSERT_NE(at, -1);
    ASSERT_LT(at, 3);
    ASSERT_GT(g_peak, LOUD);
}

/* aud_rom_gen.py writes a freq of 1320, which the PSG divides by three, so
 * the note is 440 Hz and the bounds are a tenth either side of it. */
UTEST(aud, psg_oscillates)
{
    ASSERT_TRUE(load_rom(AUD_ROM_PSG));
    ASSERT_NE(frames_to_sound(8), -1);
    measure_from_here();
    play_out(12);
    ASSERT_GT(g_peak, LOUD);
    ASSERT_GT(g_cross, 100);
    const float hz = measured_hz();
    ASSERT_GT(hz, 396.0f);
    ASSERT_LT(hz, 484.0f);
}

UTEST(aud, opl_oscillates)
{
    ASSERT_TRUE(load_rom(AUD_ROM_OPL));
    ASSERT_NE(frames_to_sound(8), -1);
    measure_from_here();
    play_out(12);
    ASSERT_GT(g_peak, LOUD);
    ASSERT_GT(g_cross, 80);
}

/* The gate in the block is written before xreg selects the PSG, so it
 * does not start the note. A note starts only on a gate write the RW engine
 * reported, and it reports nothing until psg_xreg names the page. The note
 * starts on the gate written afterwards. */
UTEST(aud, a_psg_block_programmed_before_its_pointer)
{
    ASSERT_TRUE(load_rom(AUD_ROM_PSG_PRE));
    run_frame();
    /* The program writes the second gate about five frames after xreg, at
     * the default 8 MHz. */
    for (int i = 0; i < 3; i++)
    {
        run_frame();
        ASSERT_EQ(g_peak, 0.0f);
    }
    const int at = frames_to_sound(16);
    ASSERT_NE(at, -1);
    ASSERT_GT(g_peak, LOUD);
}

UTEST(aud, opl_makes_a_noise)
{
    ASSERT_TRUE(load_rom(AUD_ROM_OPL));
    const int at = frames_to_sound(8);
    ASSERT_NE(at, -1);
    ASSERT_LT(at, 3);
    ASSERT_GT(g_peak, LOUD);
}

UTEST(aud, opl_sounds_after_a_clearing_burst)
{
    ASSERT_TRUE(load_rom(AUD_ROM_OPL_INIT));
    const int at = frames_to_sound(8);
    ASSERT_NE(at, -1);
    ASSERT_LT(at, 3);
    ASSERT_GT(g_peak, LOUD);
}

UTEST(aud, the_bell_rings_with_no_program)
{
    ASSERT_TRUE(load_rom(AUD_ROM_BEL));
    const int at = frames_to_sound(4);
    ASSERT_NE(at, -1);
    ASSERT_GT(g_peak, BELL);
    /* emu_restart does not stop the 800 ms bell, so it is played out
     * before the next test. */
    play_out(60);
}

UTEST(aud, the_bell_rings_over_the_opl)
{
    ASSERT_TRUE(load_rom(AUD_ROM_OPL_BEL));
    const int at = frames_to_sound(4);
    ASSERT_NE(at, -1);
    ASSERT_GT(g_peak, BELL);
    play_out(60);
}

UTEST(aud, a_program_exit_goes_quiet)
{
    ASSERT_TRUE(load_rom(AUD_ROM_OPL_EXIT));
    ASSERT_NE(frames_to_sound(8), -1);
    int stopped = -1;
    for (int i = 0; i < 20; i++)
    {
        run_frame();
        if (!sys_running())
        {
            stopped = i;
            break;
        }
    }
    ASSERT_NE(stopped, -1);
    run_frame();
    run_frame();
    ASSERT_EQ(g_peak, 0.0f);
}

UTEST_MAIN_EMU();
