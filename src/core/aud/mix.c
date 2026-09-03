/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The soft machine's mixer. The sink asks for a buffer and this fills it,
 * pulling the registered device and the bell one sample at a time at
 * AUD_NATIVE_RATE and resampling to whatever the sink runs at. Nothing here
 * keeps time: the sink's clock is the only one.
 */

#include "core/aud/mix.h"
#include "core/aud/bel.h"
#include "core/aud/rsmp.h"
#include "core/aud/sine.h"
#include "core/dap/dbg.h"
#include <string.h>

/* What the sink runs at until it says otherwise. libretro declares 48000 to
 * its frontend and never calls aud_set_sink_rate; a window host asks its
 * device and reports back what it was given. */
#define AUD_SINK_RATE 48000
static uint32_t g_sink_rate = AUD_SINK_RATE;

static void (*aud_dev)(int16_t *left, int16_t *right);

/* One resampler per channel, carried across renders so the phase is
 * continuous. Every voice goes through it: the machine's rate is never the
 * sink's. */
static rsmp_t g_rs_l, g_rs_r;

/* What the last input yielded past the end of a buffer, for the start of the
 * next. Empty at unity and below; only a sink faster than the machine leaves
 * any, and never more than rsmp_push can hand over at once. */
static int32_t g_pend_l[8], g_pend_r[8];
static int g_pend_n, g_pend_i;

/* The level as it stands, for a held machine to repeat. */
static float g_last_l, g_last_r;

/* Rolling mono downmix of everything rendered, for waveform display; the
 * reader plots the buffer directly against the write position. */
#define AUD_VIZ_SAMPLES 4096
static float g_viz[AUD_VIZ_SAMPLES];
static int g_viz_pos;

/* --mute: when off, the synth never runs (no per-sample CPU work) and the
 * app opens no OS audio device. A session setting, not machine state, so
 * resets leave it alone. */
static bool g_enabled = true;

void aud_set_enabled(bool on) { g_enabled = on; }
bool aud_enabled(void) { return g_enabled; }

void aud_init(void)
{
    sine_init();
    bel_init();
    rsmp_reset(&g_rs_l);
    rsmp_reset(&g_rs_r);
    g_pend_n = g_pend_i = 0;
    g_last_l = g_last_r = 0;
    aud_dev = NULL;
}

void aud_stop(void)
{
    aud_dev = NULL;
}

void aud_setup(void (*sample)(int16_t *left, int16_t *right))
{
    aud_dev = sample;
}

void aud_set_sink_rate(uint32_t rate)
{
    if (rate)
        g_sink_rate = rate;
}

static inline float to_f(int32_t v)
{
    /* The filter overshoots on transients, which is a sinc doing its job.
     * The host's converter is the sink, so this is where it stops. */
    if (v > 32767)
        v = 32767;
    if (v < -32768)
        v = -32768;
    return (float)v / 32768.0f;
}

/* One frame at AUD_NATIVE_RATE: the device if one is registered, the bell
 * regardless, summed and clamped the way wiring.sv sums the fabric's. */
static void mix(int32_t *left, int32_t *right)
{
    int16_t l = 0, r = 0;
    if (aud_dev)
        aud_dev(&l, &r);
    const int32_t bel = bel_sample();
    int32_t sl = l + bel;
    int32_t sr = r + bel;
    if (sl < AUD_SAMPLE_MIN)
        sl = AUD_SAMPLE_MIN;
    if (sl > AUD_SAMPLE_MAX)
        sl = AUD_SAMPLE_MAX;
    if (sr < AUD_SAMPLE_MIN)
        sr = AUD_SAMPLE_MIN;
    if (sr > AUD_SAMPLE_MAX)
        sr = AUD_SAMPLE_MAX;
    *left = sl;
    *right = sr;
}

int aud_render(float *dst, int samples)
{
    if (!g_enabled)
    {
        memset(dst, 0, (size_t)samples * 2 * sizeof *dst);
        return 0;
    }
    /* A debugger holding the 6502 holds the whole machine, and a held
     * machine makes nothing: every frame is the last one it made. Silence
     * would be a click. */
    if (dbg_is_stopped())
    {
        for (int i = 0; i < samples; i++)
        {
            dst[i * 2] = g_last_l;
            dst[i * 2 + 1] = g_last_r;
        }
        return 0;
    }
    const uint64_t step = rsmp_step(AUD_NATIVE_RATE, g_sink_rate);
    for (int i = 0; i < samples; i++)
    {
        while (g_pend_i == g_pend_n)
        {
            int32_t l, r;
            mix(&l, &r);
            g_pend_n = rsmp_push(&g_rs_l, l, step, g_pend_l, 8);
            rsmp_push(&g_rs_r, r, step, g_pend_r, 8);
            g_pend_i = 0;
        }
        g_last_l = dst[i * 2] = to_f(g_pend_l[g_pend_i]);
        g_last_r = dst[i * 2 + 1] = to_f(g_pend_r[g_pend_i]);
        g_pend_i++;
        g_viz[g_viz_pos] = (g_last_l + g_last_r) * 0.5f;
        g_viz_pos = (g_viz_pos + 1) % AUD_VIZ_SAMPLES;
    }
    return samples;
}

const float *aud_viz_buffer(int *num_samples)
{
    *num_samples = AUD_VIZ_SAMPLES;
    return g_viz;
}

int aud_viz_pos(void) { return g_viz_pos; }
