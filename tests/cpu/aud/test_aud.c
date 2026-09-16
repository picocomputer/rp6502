/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Both audio engines end to end, on this machine: a program asks for the
 * device with xreg, pours its registers into the XRAM page through RW0,
 * and the machine makes a noise. The programs are the ones test_aud.cpp
 * boots on the fabric, so what sounds there and what sounds here cannot
 * drift apart. Nothing here checks a waveform; what these check is every
 * link between a 6502 store and a sample the machine made -- and that a
 * real program's opening burst, more writes in a few scanlines than the
 * queue holds, all reaches the chip before the sink asks.
 *
 * The measurement is the mixer's own tap of what it rendered: a sink frame
 * pulled after each machine frame, so a frame's number is what the machine
 * had made by the end of it. Two numbers come off that tap. The peak says
 * a voice is at scale, which catches silence; the gated crossings say the
 * level is moving and how fast, which catches a voice stuck at one level
 * and puts a pitch on the one that isn't.
 */

#include "core/aud/mix.h"
#include "core/sys/sys.h"
#include "emu_boot.h"

static int g_pos;
static float g_peak;

/* The sink rate the frequency is measured against. Pinned rather than
 * assumed, since a host is free to ask for another one. */
#define SINK_RATE 48000

/* A level has to pass this to count as a side of zero, so the noise either
 * side of a crossing is not a cycle of its own. Well under one voice at
 * scale, well over a quiet room. */
#define GATE (512.0f / 32768.0f)

static int g_cross;  /* full crossings: two to the cycle */
static int g_span;   /* samples they were counted over */
static int g_side;   /* which side of zero the level was last seen on */

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

/* Start counting here, so an attack or a first frame of setup is not part
 * of the pitch. */
static void measure_from_here(void)
{
    g_cross = 0;
    g_span = 0;
    g_side = 0;
}

/* What the crossings say the voice is doing, in Hz. */
static float measured_hz(void)
{
    if (g_span <= 0)
        return 0.0f;
    return (g_cross / 2.0f) * SINK_RATE / (float)g_span;
}

/* Frames until the engine is heard, or -1. Two is the budget; the load
 * and the program's own setup eat most of the first. */
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

/* Let a sound run its course. A frame of the machine makes no sound on its
 * own; the sink has to keep asking. */
static void play_out(int frames)
{
    while (frames-- > 0)
        run_frame();
}

/* One PSG channel at full volume, centred, is 16127 of 32767; a peak above
 * 4096 can only be a voice at scale. */
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

/* A peak alone cannot tell a voice from a level stuck away from zero, so
 * this counts the crossings and turns them into the pitch the program asked
 * for: aud_rom_gen.py writes 440 Hz, which the engine divides by three. A
 * tenth either way is room for the gate and the frame the count starts on,
 * and nowhere near the next note. */
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

/* The same claim for the other engine, without the pitch. aud_rom_gen.py
 * keys block 4 with f-number 0x198, a fundamental near 310 Hz, but an FM
 * voice with feedback is not a sine: its harmonics cross zero several times
 * a cycle, so counted crossings measure the timbre, not the note. What is
 * worth asserting is that the level keeps moving, which is what a stuck
 * engine would fail. */
UTEST(aud, opl_oscillates)
{
    ASSERT_TRUE(load_rom(AUD_ROM_OPL));
    ASSERT_NE(frames_to_sound(8), -1);
    measure_from_here();
    play_out(12);
    ASSERT_GT(g_peak, LOUD);
    ASSERT_GT(g_cross, 80);
}

/* The same note with the block written before the pointer: the whole
 * structure has to arrive, and the gate written among it has to not
 * sound. The note starts on the gate written afterwards. */
UTEST(aud, a_psg_block_programmed_before_its_pointer)
{
    ASSERT_TRUE(load_rom(AUD_ROM_PSG_PRE));
    run_frame();
    /* The program holds its own gate off for about five frames. */
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

/* A real program clears every register before it plays: 255 writes in a
 * few scanlines, then the note. The queue holds 255, so the note only
 * sounds if the machine drains as the program writes. */
UTEST(aud, opl_sounds_after_a_clearing_burst)
{
    ASSERT_TRUE(load_rom(AUD_ROM_OPL_INIT));
    const int at = frames_to_sound(8);
    ASSERT_NE(at, -1);
    ASSERT_LT(at, 3);
    ASSERT_GT(g_peak, LOUD);
}

/* The bell sounds with no program holding an engine at all -- the
 * console's own case. */
UTEST(aud, the_bell_rings_with_no_program)
{
    ASSERT_TRUE(load_rom(AUD_ROM_BEL));
    const int at = frames_to_sound(4);
    ASSERT_NE(at, -1);
    ASSERT_GT(g_peak, BELL);
    play_out(60); /* a bell rings through a program change; let it end */
}

/* Nothing gates the mix: an OPL program holds an engine and the bell
 * sounds over it. */
UTEST(aud, the_bell_rings_over_the_opl)
{
    ASSERT_TRUE(load_rom(AUD_ROM_OPL_BEL));
    const int at = frames_to_sound(4);
    ASSERT_NE(at, -1);
    ASSERT_GT(g_peak, BELL);
    play_out(60);
}

/* Exiting a program parks the engine: the stop hands the standing bell
 * back, and with nothing rung the machine makes exactly zero. */
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
    run_frame(); /* the release already in flight */
    run_frame();
    ASSERT_EQ(g_peak, 0.0f);
}

UTEST_MAIN_EMU();
