/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "core/aud/aud_mix.h"
#include "core/aud/rsmp.h"
#include "core/aud/bel.h"
#include "core/aud/psg.h"
#include "core/dap/dbg.h"
#include <string.h>

/* The active device's sample handler + rate, installed by aud_setup. */
static void (*aud_irq_fn)(void);
static uint32_t aud_irq_rate;

/* What the machine generates at. The default is what we ask the host for;
 * aud_set_native_rate replaces it with what the host actually gave, so the
 * voices whose rate is ours to choose are generated at the device's rate
 * and aud_render has nothing to convert for them. */
#define AUD_NATIVE_RATE 48000
static uint32_t g_native_rate = AUD_NATIVE_RATE;

uint32_t aud_native_rate(void) { return g_native_rate; }

void aud_init(void)
{
    aud_sine_init();
    psg_setup(aud_native_rate());
    aud_stop(); /* the standing BEL device, as the firmware's aud_init */
}

void aud_set_native_rate(uint32_t rate)
{
    if (!rate || rate == g_native_rate)
        return;
    g_native_rate = rate;
    /* The PSG's envelope tables and phase divisor come out of the rate, and
     * the standing bell is registered with it, so both have to be rebuilt. */
    psg_setup(rate);
    aud_stop();
}

void aud_setup(void (*irq_fn)(void), uint32_t rate)
{
    aud_irq_rate = rate;
    aud_irq_fn = irq_fn;
}

/* The last stereo level the active handler wrote, signed with silence at
 * zero; aud_render reads it back each sample and scales it to the float the
 * host wants. Nothing here knows the RP2350's PWM depth any more. */
static int16_t g_out_l, g_out_r;

void aud_out(int16_t left, int16_t right)
{
    g_out_l = left;
    g_out_r = right;
}

void aud_clear_irq(void) {}

/* Rolling mono downmix of everything rendered, for waveform display; the
 * reader plots the buffer directly against the write position. */
#define AUD_VIZ_SAMPLES 4096
static float g_viz[AUD_VIZ_SAMPLES];
static int g_viz_pos;

/* --mute: when off, the synth never runs (no per-sample CPU work) and the
 * app opens no OS audio device. A session setting, not machine state, so resets
 * leave it alone. */
static bool g_enabled = true;

void aud_set_enabled(bool on) { g_enabled = on; }
bool aud_enabled(void) { return g_enabled; }

int aud_rate(void)
{
    if (!g_enabled)
        return 0;
    return aud_irq_fn ? (int)aud_irq_rate : 0;
}

/* One resampler per channel, carried across calls so the phase is
 * continuous, and what the last call's final push yielded past its buffer.
 * Only the OPL2 ever reaches these: everything else is generated at
 * aud_native_rate(), which is the device's own rate. The history belongs to
 * the handler that made it. */
static rsmp_t g_rs_l, g_rs_r;
static int32_t g_carry_l[8], g_carry_r[8];
static int g_carry_n, g_carry_at;
static void (*g_rs_fn)(void);

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

static inline void put(float *dst, int i, float l, float r)
{
    dst[i * 2 + 0] = l;
    dst[i * 2 + 1] = r;
    g_viz[g_viz_pos] = (l + r) * 0.5f;
    g_viz_pos = (g_viz_pos + 1) % AUD_VIZ_SAMPLES;
}

/* Under the debugger the handler does not run and the level stands: a
 * stopped program is stopped for someone to read it, and a note sustaining
 * under the cursor is not information, while a drop to zero is a click at
 * both ends. A sys_stop is not this -- audio plays through it, which is how
 * the bell rings between programs. */
void aud_render(float *dst, int samples)
{
    void (*handler)(void) = aud_irq_fn;
    const uint32_t in_rate = aud_irq_rate;
    const uint32_t out_rate = g_native_rate;
    if (!g_enabled || !handler)
    {
        memset(dst, 0, (size_t)samples * 2 * sizeof *dst);
        return;
    }
    const bool held = dbg_is_stopped();
    /* The usual case, and not merely an optimisation: a resampler run at
     * unity still rounds, and a voice generated at the device's own rate has
     * nothing to gain from being filtered. */
    if (in_rate == out_rate)
    {
        for (int i = 0; i < samples; i++)
        {
            if (!held)
                handler();
            put(dst, i, g_out_l / 32768.0f, g_out_r / 32768.0f);
        }
        return;
    }
    if (handler != g_rs_fn)
    {
        rsmp_reset(&g_rs_l);
        rsmp_reset(&g_rs_r);
        g_carry_n = g_carry_at = 0;
        g_rs_fn = handler;
    }
    const uint64_t step = rsmp_step(in_rate, out_rate);
    int i = 0;
    while (i < samples)
    {
        for (; g_carry_at < g_carry_n && i < samples; g_carry_at++, i++)
            put(dst, i, to_f(g_carry_l[g_carry_at]), to_f(g_carry_r[g_carry_at]));
        if (i == samples)
            break;
        if (!held)
            handler();
        g_carry_n = rsmp_push(&g_rs_l, g_out_l, step, g_carry_l, 8);
        rsmp_push(&g_rs_r, g_out_r, step, g_carry_r, 8);
        g_carry_at = 0;
    }
}

const float *aud_viz_buffer(int *num_samples)
{
    *num_samples = AUD_VIZ_SAMPLES;
    return g_viz;
}

int aud_viz_pos(void) { return g_viz_pos; }

void aud_stop(void)
{
    bel_setup();
    memset(g_viz, 0, sizeof g_viz);
    g_viz_pos = 0;
}
