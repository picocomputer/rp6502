/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "core/aud/aud_mix.h"
#include "core/mem/mem.h"
#include "core/vga/vga_emu.h"
#include "core/aud/rsmp.h"
#include "core/aud/bel.h"
#include "core/aud/psg.h"
#define _USE_MATH_DEFINES /* MSVC: expose M_PI from <math.h> */
#include <math.h>
#include <string.h>

/* The active device's sample handler + rate, installed by aud_setup. */
static void (*aud_irq_fn)(void);
static uint32_t aud_irq_rate;

/* What the machine generates at. The default is what we ask the host for;
 * aud_set_native_rate replaces it with what the host actually gave, so the
 * voices whose rate is ours to choose are generated at the device's rate
 * and aud_pump has nothing to do for them. */
#define AUD_NATIVE_RATE 48000
static uint32_t g_native_rate = AUD_NATIVE_RATE;

uint32_t aud_native_rate(void) { return g_native_rate; }

void aud_init(void)
{
    aud_sine_init();
    psg_setup(aud_native_rate());
    aud_stop(); // the standing BEL device + a clean host ring (firmware aud.c)
}

void aud_set_native_rate(uint32_t rate)
{
    if (!rate || rate == g_native_rate)
        return;
    g_native_rate = rate;
    /* The PSG's envelope tables and phase divisor come out of the rate, and
     * the standing bell is registered with it, so both have to be rebuilt.
     * aud_stop does the bell and drains the ring. */
    psg_setup(rate);
    aud_stop();
}

void aud_setup(void (*irq_fn)(void), uint32_t rate)
{
    aud_irq_fn = irq_fn;
    aud_irq_rate = rate;
}

/* ------------------------------------------------------------------ */
/* Stereo output capture: the seam the audio drivers write through.    */
/* ------------------------------------------------------------------ */

/* The last stereo level the active handler wrote, signed with silence at
 * zero; aud_task reads it back each sample and scales it to the float the
 * host wants. Nothing here knows the RP2350's PWM depth any more. */
static int16_t g_out_l, g_out_r;

void aud_out(int16_t left, int16_t right)
{
    g_out_l = left;
    g_out_r = right;
}

void aud_clear_irq(void) {}

/* ------------------------------------------------------------------ */
/* Native-rate stereo ring                                             */
/* ------------------------------------------------------------------ */

/* ~170 ms at 24 kHz — far more than one frame, so the app draining once per
 * frame never sees it fill. A consumer that falls behind drops the oldest. */
#define AUD_RING_FRAMES 4096
static float g_ring[AUD_RING_FRAMES * 2];
static unsigned g_head, g_tail; /* frame indices, mod AUD_RING_FRAMES */

/* Rolling mono downmix of everything pushed to the ring, for waveform display;
 * the reader plots the buffer directly against the write position. */
#define AUD_VIZ_SAMPLES 4096
static float g_viz[AUD_VIZ_SAMPLES];
static int g_viz_pos;

/* Fractional sample carry so a rate that isn't a multiple of 60 (OPL's 49716)
 * stays pitch-accurate: each frame is owed rate/60 samples on average. */
static uint32_t g_sample_acc;

static void ring_push(float l, float r)
{
    unsigned next = (g_head + 1) % AUD_RING_FRAMES;
    if (next == g_tail) /* full: drop the oldest frame */
        g_tail = (g_tail + 1) % AUD_RING_FRAMES;
    g_ring[g_head * 2 + 0] = l;
    g_ring[g_head * 2 + 1] = r;
    g_head = next;
    g_viz[g_viz_pos] = (l + r) * 0.5f;
    g_viz_pos = (g_viz_pos + 1) % AUD_VIZ_SAMPLES;
}

/* --mute: when off, the synth never runs (no per-sample CPU work) and the
 * app opens no OS audio device. A session setting, not machine state, so resets
 * leave it alone. */
static bool g_enabled = true;

void aud_set_enabled(bool on) { g_enabled = on; }
bool aud_enabled(void) { return g_enabled; }

void aud_task(void)
{
    if (!g_enabled)
        return;

    /* The active device handler: PSG, OPL, or the standing BEL (silent until
     * rung), always installed like the firmware. */
    void (*handler)(void) = aud_irq_fn;
    if (!handler)
        return;
    uint32_t rate = aud_irq_rate;

    g_sample_acc += rate;
    unsigned n = g_sample_acc / VGA_HZ;
    g_sample_acc -= n * VGA_HZ;

    for (unsigned i = 0; i < n; i++)
    {
        handler(); /* advances the synth + writes g_out_l/g_out_r via aud_out */
        ring_push(g_out_l / 32768.0f, g_out_r / 32768.0f);
    }
}

int aud_rate(void)
{
    if (!g_enabled)
        return 0;
    return aud_irq_fn ? (int)aud_irq_rate : 0;
}

int aud_read(float *dst, int max_frames)
{
    int got = 0;
    while (got < max_frames && g_tail != g_head)
    {
        dst[got * 2 + 0] = g_ring[g_tail * 2 + 0];
        dst[got * 2 + 1] = g_ring[g_tail * 2 + 1];
        g_tail = (g_tail + 1) % AUD_RING_FRAMES;
        got++;
    }
    return got;
}

/* One resampler per channel, carried across frames so the phase is
 * continuous. Only the OPL2 ever reaches these: everything else is generated
 * at aud_native_rate(), which is the device's own rate. */
static rsmp_t g_rs_l, g_rs_r;

/* The ring is float only because that is what the host wants; every value in
 * it is an exact int16 over 32768, so this round trip loses nothing. */
static inline int32_t to_i(float f) { return (int32_t)lrintf(f * 32768.0f); }

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

/* saudio_push returns how many frames it took. A full device FIFO means the
 * machine is ahead of the converter, and the remainder is dropped rather than
 * waited on — blocking here would trade a click for a stall, and the ring
 * upstream already drops its oldest for the same reason. */
static void push_all(const float *f, int n,
                     int (*push)(const float *frames, int num_frames))
{
    int done = 0;
    while (done < n)
    {
        const int got = push(f + done * 2, n - done);
        if (got <= 0)
            break;
        done += got;
    }
}

void aud_pump(int out_rate, int (*push)(const float *frames, int num_frames))
{
    const int in_rate = aud_rate();
    if (in_rate <= 0 || out_rate <= 0)
        return;

    static float in[4096 * 2];
    static float out[4096 * 2];
    int navail;

    /* The usual case, and not merely an optimisation: a resampler run at
     * unity still rounds, and a voice generated at the device's own rate has
     * nothing to gain from being filtered. */
    if (in_rate == out_rate)
    {
        while ((navail = aud_read(in, 4096)) > 0)
            push_all(in, navail, push);
        return;
    }

    const uint64_t step = rsmp_step((uint32_t)in_rate, (uint32_t)out_rate);
    while ((navail = aud_read(in, 4096)) > 0)
    {
        int oc = 0;
        for (int i = 0; i < navail; i++)
        {
            int32_t bl[8], br[8];
            const int n = rsmp_push(&g_rs_l, to_i(in[i * 2 + 0]), step, bl, 8);
            rsmp_push(&g_rs_r, to_i(in[i * 2 + 1]), step, br, 8);
            for (int k = 0; k < n; k++)
            {
                out[oc * 2 + 0] = to_f(bl[k]);
                out[oc * 2 + 1] = to_f(br[k]);
                if (++oc == 4096)
                {
                    push_all(out, oc, push);
                    oc = 0;
                }
            }
        }
        if (oc > 0)
            push_all(out, oc, push);
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
    bel_setup(); /* fall back to the standing BEL device (firmware aud_stop) */
    /* Drain the emu's host PCM output ring so a stopped program's stale samples
     * don't bleed into the next. The BEL device keeps its state — a rung bell
     * rings through (CLAUDE.md); only this host-side ring is cleared. */
    g_head = g_tail = 0;
    g_sample_acc = 0;
    xram_queue_head = xram_queue_tail = 0;
    xram_queue_page = 0;
    g_out_l = g_out_r = 0;
    memset(g_viz, 0, sizeof g_viz);
    g_viz_pos = 0;
    /* The resampler's phase and history outlived a program stop and put a
     * discontinuity at the start of the next one. */
    rsmp_reset(&g_rs_l);
    rsmp_reset(&g_rs_r);
}
