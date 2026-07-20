/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Integration test for the RIA audio path. furelise.rp6502 (the ezpsg tracker
 * playing Beethoven's Fur Elise) programs the PSG via xreg device 0 channel 1,
 * writes note configs and gate bits to XRAM, and paces itself on vsync. This
 * exercises the whole chain: the xreg audio dispatch, the XRAM write-notify
 * queue that carries gate edges to the sample handler, the vendored psg.c DSP,
 * and the aud.c per-frame pump feeding the native-rate ring.
 */

#include "emu/aud/aud.h"
#include "emu/sys/mem.h"
#include "emu/sys/cpu.h"
#include "emu/sys/vga.h"
#include "emu_boot.h"
#include <math.h>

/* Run one frame and drain its audio into running peak/energy accumulators. */
static void pump(int n, float *peak, double *energy, long *frames)
{
    static float buf[4096 * 2];
    for (int i = 0; i < n; i++)
    {
        sys_run_frame();
        int got;
        while ((got = aud_read(buf, 4096)) > 0)
        {
            for (int s = 0; s < got * 2; s++)
            {
                float a = fabsf(buf[s]);
                if (a > *peak)
                    *peak = a;
                *energy += (double)buf[s] * buf[s];
                if (s & 1)
                    (*frames)++;
            }
        }
    }
}

/* Before the program touches the PSG the device is silent (rate 0, no
 * samples); once it plays, samples appear at the PSG's fixed 24 kHz. */
UTEST(furelise, plays_psg_audio)
{
    ASSERT_TRUE(emu_restart(FURELISE_ROM));
    /* The standing BEL device runs at boot (firmware: aud_init installs it),
     * silent until ezpsg_init swaps in the PSG — also 24 kHz. */
    ASSERT_EQ(aud_rate(), 24000);

    float peak = 0.0f;
    double energy = 0.0;
    long frames = 0;

    /* A few frames in, the ROM has run ezpsg_init -> the PSG is live. */
    pump(8, &peak, &energy, &frames);
    ASSERT_EQ(aud_rate(), 24000);

    /* Play ~3 s of the song; the first notes ramp up well within that. */
    pump(180, &peak, &energy, &frames);
    ASSERT_FALSE(cpu_halted()); /* still playing */

    ASSERT_GT(frames, (long)(24000 * 2)); /* ~24 kHz of stereo frames generated */
    ASSERT_GT(peak, 0.01f);               /* the song is audibly playing */
    ASSERT_GT(energy, 1.0);               /* sustained signal, not a lone blip */
}

/* A program stop tears the PSG down (back to the silent standing BEL) and
 * drains the ring. */
UTEST(furelise, reset_silences)
{
    ASSERT_TRUE(emu_restart(FURELISE_ROM));

    float peak = 0.0f;
    double energy = 0.0;
    long frames = 0;
    pump(60, &peak, &energy, &frames);
    ASSERT_EQ(aud_rate(), 24000);
    ASSERT_GT(peak, 0.0f);

    main_stop(); /* aud_stop falls back to the standing BEL + drains the ring */
    ASSERT_EQ(aud_rate(), 24000); /* BEL device present, but silent */

    static float buf[64];
    ASSERT_EQ(aud_read(buf, 64), 0); /* ring drained */
}

/* furelise prints its title "Für Elise" — the 'ü' is a CP437 high-half glyph
 * (byte 0x81). It must actually render, not blank. Regression guard for the font
 * high-half loading: font_init (run once at main_init) must
 * load the high half, not leave 0x80-0xFF blank ("F r Elise"). */
UTEST(furelise, umlaut_renders)
{
    static uint32_t fb[VGA_MAX_WIDTH * VGA_MAX_HEIGHT];
    ASSERT_TRUE(emu_restart(FURELISE_ROM));
    vga_set_framebuffer(fb);
    for (int i = 0; i < 8; i++)
        sys_run_frame(); /* prints the title; the lazy-clear render needs frames */

    /* Count lit pixels in the 8x16 cell at (col,row0) of the 80x30 console.
     * "Für Elise": F=col0, ü=col1, r=col2. */
    int lit[3] = {0, 0, 0};
    for (int col = 0; col < 3; col++)
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 8; x++)
                if ((fb[y * VGA_MAX_WIDTH + col * 8 + x] & 0x00FFFFFFu) != 0)
                    lit[col]++;

    ASSERT_GT(lit[0], 0); /* 'F' drew (sanity: text is rendering) */
    ASSERT_GT(lit[1], 0); /* 'ü' drew — the CP437 high-half glyph is present */
    ASSERT_GT(lit[2], 0); /* 'r' drew */
}

UTEST_MAIN_EMU()
