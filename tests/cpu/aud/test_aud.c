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
 * had made by the end of it.
 */

#include "core/aud/mix.h"
#include "core/wdc/resb.h"
#include "emu_boot.h"

static int g_pos;
static float g_peak;

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
    }
    g_pos = pos;
}

static bool load_rom(const char *rom)
{
    if (!emu_restart(rom))
        return false;
    g_pos = aud_viz_pos();
    return true;
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
        if (!resb_running())
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
