/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Host audio output: drain the emulator's native-rate stereo ring, linear-
 * resample it to the sokol-audio device rate, and push. The RIA devices run at a
 * fixed rate (PSG 24 kHz, OPL ~49.7 kHz) that rarely matches the host device, so
 * we rate-convert here; the resampler state carries across frames so the
 * interpolation is continuous. Split out of app_sokol.c (the window file).
 */

#include "emu/app/audio_out.h"

#ifdef EMU_WITH_AUDIO

#include "emu/aud/aud.h"
#include "sokol_audio.h"

/* Linear resampler state carried across frames so interpolation is continuous:
 * the read position between the previous and current native input sample, and
 * that previous sample. */
static struct
{
    double frac;
    float prev_l, prev_r;
    bool primed;
} g_rs;

void audio_out_pump(void)
{
    if (!saudio_isvalid())
        return;
    int in_rate = emu_audio_rate();
    int out_rate = saudio_sample_rate();
    if (in_rate <= 0 || out_rate <= 0)
        return;
    const double step = (double)in_rate / out_rate; /* input frames per output frame */

    static float in[4096 * 2];
    static float out[4096 * 2];
    int navail;
    while ((navail = emu_audio_read(in, 4096)) > 0)
    {
        int oc = 0;
        for (int i = 0; i < navail; i++)
        {
            float cl = in[i * 2 + 0], cr = in[i * 2 + 1];
            if (!g_rs.primed)
            {
                g_rs.prev_l = cl;
                g_rs.prev_r = cr;
                g_rs.primed = true;
            }
            /* Emit every output sample that falls between prev and cur. */
            while (g_rs.frac < 1.0)
            {
                out[oc * 2 + 0] = g_rs.prev_l + (cl - g_rs.prev_l) * (float)g_rs.frac;
                out[oc * 2 + 1] = g_rs.prev_r + (cr - g_rs.prev_r) * (float)g_rs.frac;
                if (++oc == 4096)
                {
                    saudio_push(out, oc);
                    oc = 0;
                }
                g_rs.frac += step;
            }
            g_rs.frac -= 1.0;
            g_rs.prev_l = cl;
            g_rs.prev_r = cr;
        }
        if (oc > 0)
            saudio_push(out, oc);
    }
}

#else

void audio_out_pump(void) {}

#endif /* EMU_WITH_AUDIO */
