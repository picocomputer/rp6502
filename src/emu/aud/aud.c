/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/aud/aud.h"
#include "emu/sys/mem.h"
#include "emu/sys/vga.h"
#include "aud/aud.h"
#include "aud/bel.h"
#define _USE_MATH_DEFINES /* MSVC: expose M_PI from <math.h> */
#include <hardware/pwm.h>
#include <math.h>
#include <string.h>

int8_t aud_sine_table[256];

/* The active device's sample handler + rate, installed by aud_setup. */
static void (*aud_irq_fn)(void);
static uint32_t aud_irq_rate;

void aud_init(void)
{
    // Phase 0 starts at the trough (-cos), so readers can index the raw phase.
    for (unsigned i = 0; i < 256; i++)
        aud_sine_table[i] = (int8_t)lround(cos(M_PI * 2.0 / 256 * i) * -127);
    aud_stop(); // the standing BEL device + a clean host ring (firmware aud.c)
}

void aud_setup(void (*irq_fn)(void), uint32_t rate)
{
    aud_irq_fn = irq_fn;
    aud_irq_rate = rate;
}

/* ------------------------------------------------------------------ */
/* PWM capture: the seam the audio drivers write each sample through.  */
/* ------------------------------------------------------------------ */

/* Indexed by slice number; only the two audio slices (L/R) are read back. */
static uint16_t g_pwm_level[32];

void pwm_clear_irq(unsigned slice)
{
    (void)slice;
}

void pwm_set_chan_level(unsigned slice, unsigned chan, uint16_t level)
{
    (void)chan;
    g_pwm_level[slice & 31] = level;
}

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
        handler(); /* advances the synth + writes g_pwm_level via the shim */
        int l = (int)g_pwm_level[AUD_L_SLICE] - AUD_PWM_CENTER;
        int r = (int)g_pwm_level[AUD_R_SLICE] - AUD_PWM_CENTER;
        ring_push((float)l / AUD_PWM_CENTER, (float)r / AUD_PWM_CENTER);
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
    memset(g_pwm_level, 0, sizeof g_pwm_level);
    memset(g_viz, 0, sizeof g_viz);
    g_viz_pos = 0;
}
