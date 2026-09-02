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
#include "core/vga/vga_emu.h"
#include "host/host.h"
#include <stdatomic.h>
#include <string.h>

/* The active device's sample handler + rate, installed by aud_setup. */
static void (*aud_irq_fn)(void);
static uint32_t aud_irq_rate;

/* What the machine generates at. The default is what we ask the host for;
 * aud_set_native_rate replaces it with what the host actually gave, so the
 * voices whose rate is ours to choose are generated at the device's rate
 * and nothing has to convert for them. */
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

/* When the next sample is due on the machine's clock: microseconds and a
 * remainder in 1/rate of one, so a second of samples is exactly a second. */
static uint64_t g_due_us;
static uint32_t g_due_rem;

void aud_setup(void (*irq_fn)(void), uint32_t rate)
{
    aud_irq_rate = rate;
    aud_irq_fn = irq_fn;
    g_due_rem = 0;
}

/* The last stereo level the active handler wrote, signed with silence at
 * zero. Nothing here knows the RP2350's PWM depth any more. */
static int16_t g_out_l, g_out_r;

void aud_out(int16_t left, int16_t right)
{
    g_out_l = left;
    g_out_r = right;
}

void aud_clear_irq(void) {}

/* Rolling mono downmix of everything made, for waveform display; the
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

/* What the machine has made and the device has not yet taken. The machine
 * writes the head and the device reads at the tail, each on its own
 * thread; the counters run free and the size is a power of two, so the
 * lead is head minus tail. Sized for AUD_LEAD_FRAMES at 192 kHz. */
#define AUD_RING_FRAMES 8192
static float g_ring[AUD_RING_FRAMES][2];
static atomic_uint g_head, g_tail;

static inline unsigned frame_samples(void) { return g_native_rate / VGA_HZ; }

static void push(float l, float r)
{
    g_viz[g_viz_pos] = (l + r) * 0.5f;
    g_viz_pos = (g_viz_pos + 1) % AUD_VIZ_SAMPLES;
    const unsigned head = atomic_load_explicit(&g_head, memory_order_relaxed);
    const unsigned tail = atomic_load_explicit(&g_tail, memory_order_acquire);
    if (head - tail >= AUD_LEAD_FRAMES * frame_samples())
        return;
    g_ring[head % AUD_RING_FRAMES][0] = l;
    g_ring[head % AUD_RING_FRAMES][1] = r;
    atomic_store_explicit(&g_head, head + 1, memory_order_release);
}

/* One resampler per channel, carried across samples so the phase is
 * continuous. Only the OPL2 ever reaches these: everything else is
 * generated at aud_native_rate(), which is the device's own rate. The
 * history belongs to the handler that made it. */
static rsmp_t g_rs_l, g_rs_r;
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

static void make(void)
{
    aud_irq_fn();
    /* The usual case, and not merely an optimisation: a resampler run at
     * unity still rounds, and a voice generated at the device's own rate has
     * nothing to gain from being filtered. */
    if (aud_irq_rate == g_native_rate)
    {
        push(g_out_l / 32768.0f, g_out_r / 32768.0f);
        return;
    }
    if (aud_irq_fn != g_rs_fn)
    {
        rsmp_reset(&g_rs_l);
        rsmp_reset(&g_rs_r);
        g_rs_fn = aud_irq_fn;
    }
    const uint64_t step = rsmp_step(aud_irq_rate, g_native_rate);
    int32_t l[8], r[8];
    const int n = rsmp_push(&g_rs_l, g_out_l, step, l, 8);
    rsmp_push(&g_rs_r, g_out_r, step, r, 8);
    for (int i = 0; i < n; i++)
        push(to_f(l[i]), to_f(r[i]));
}

void aud_task(void)
{
    const uint64_t now = host_clock_us();
    if (!g_enabled)
    {
        g_due_us = now;
        g_due_rem = 0;
        return;
    }
    const uint32_t rate = aud_irq_rate;
    while (g_due_us <= now)
    {
        make();
        g_due_us += 1000000u / rate;
        g_due_rem += 1000000u % rate;
        if (g_due_rem >= rate)
        {
            g_due_rem -= rate;
            g_due_us++;
        }
    }
}

int aud_render(float *dst, int samples)
{
    if (!g_enabled)
    {
        memset(dst, 0, (size_t)samples * 2 * sizeof *dst);
        return 0;
    }
    static float last_l, last_r;
    static bool served;
    unsigned tail = atomic_load_explicit(&g_tail, memory_order_relaxed);
    const unsigned head = atomic_load_explicit(&g_head, memory_order_acquire);
    unsigned avail = head - tail;
    if (!served && avail < frame_samples() + (unsigned)samples)
        avail = 0;
    const int n = avail < (unsigned)samples ? (int)avail : samples;
    for (int i = 0; i < n; i++, tail++)
    {
        last_l = dst[i * 2 + 0] = g_ring[tail % AUD_RING_FRAMES][0];
        last_r = dst[i * 2 + 1] = g_ring[tail % AUD_RING_FRAMES][1];
    }
    atomic_store_explicit(&g_tail, tail, memory_order_release);
    served = n == samples;
    for (int i = n; i < samples; i++)
    {
        dst[i * 2 + 0] = last_l;
        dst[i * 2 + 1] = last_r;
    }
    return n;
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
}
