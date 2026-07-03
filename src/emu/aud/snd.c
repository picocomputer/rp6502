/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/aud/aud.h"
#include "emu/aud/snd.h"
#include "emu/sys/mem.h"
#include "emu/sys/sys.h"
#include "aud/aud.h"
#include <hardware/pwm.h>
#include <string.h>

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
#define SND_RING_FRAMES 4096
static float g_ring[SND_RING_FRAMES * 2];
static unsigned g_head, g_tail; /* frame indices, mod SND_RING_FRAMES */

/* Rolling mono downmix of everything pushed to the ring, for waveform display;
 * the reader plots the buffer directly against the write position. */
#define SND_VIZ_SAMPLES 4096
static float g_viz[SND_VIZ_SAMPLES];
static int g_viz_pos;

/* Fractional sample carry so a rate that isn't a multiple of 60 (OPL's 49716)
 * stays pitch-accurate: each frame is owed rate/60 samples on average. */
static uint32_t g_sample_acc;

static void ring_push(float l, float r)
{
    unsigned next = (g_head + 1) % SND_RING_FRAMES;
    if (next == g_tail) /* full: drop the oldest frame */
        g_tail = (g_tail + 1) % SND_RING_FRAMES;
    g_ring[g_head * 2 + 0] = l;
    g_ring[g_head * 2 + 1] = r;
    g_head = next;
    g_viz[g_viz_pos] = (l + r) * 0.5f;
    g_viz_pos = (g_viz_pos + 1) % SND_VIZ_SAMPLES;
}

/* --mute: when off, the synth never runs (no per-sample CPU work) and the
 * app opens no OS audio device. A session setting, not machine state, so resets
 * leave it alone. */
static bool snd_enabled = true;

void emu_set_audio_enabled(bool on) { snd_enabled = on; }
bool emu_audio_enabled(void) { return snd_enabled; }

void snd_task(void)
{
    if (!snd_enabled)
        return;

    /* The active device handler: PSG, OPL, or the standing BEL (silent until
     * rung), always installed like the firmware. */
    void (*handler)(void) = aud_host_irq();
    if (!handler)
        return;
    uint32_t rate = aud_host_rate();

    g_sample_acc += rate;
    unsigned n = g_sample_acc / EMU_VGA_HZ;
    g_sample_acc -= n * EMU_VGA_HZ;

    for (unsigned i = 0; i < n; i++)
    {
        handler(); /* advances the synth + writes g_pwm_level via the shim */
        int l = (int)g_pwm_level[AUD_L_SLICE] - AUD_PWM_CENTER;
        int r = (int)g_pwm_level[AUD_R_SLICE] - AUD_PWM_CENTER;
        ring_push((float)l / AUD_PWM_CENTER, (float)r / AUD_PWM_CENTER);
    }
}

int emu_audio_rate(void)
{
    if (!snd_enabled)
        return 0;
    return aud_host_irq() ? (int)aud_host_rate() : 0;
}

int emu_audio_read(float *dst, int max_frames)
{
    int got = 0;
    while (got < max_frames && g_tail != g_head)
    {
        dst[got * 2 + 0] = g_ring[g_tail * 2 + 0];
        dst[got * 2 + 1] = g_ring[g_tail * 2 + 1];
        g_tail = (g_tail + 1) % SND_RING_FRAMES;
        got++;
    }
    return got;
}

const float *emu_audio_viz_buffer(int *num_samples)
{
    *num_samples = SND_VIZ_SAMPLES;
    return g_viz;
}

int emu_audio_viz_pos(void) { return g_viz_pos; }

void snd_reset(void)
{
    aud_stop(); /* fall back to the standing BEL device (firmware aud_stop) */
    g_head = g_tail = 0;
    g_sample_acc = 0;
    xram_queue_head = xram_queue_tail = 0;
    xram_queue_page = 0;
    memset(g_pwm_level, 0, sizeof g_pwm_level);
    memset(g_viz, 0, sizeof g_viz);
    g_viz_pos = 0;
}
