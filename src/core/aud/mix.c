/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/aud/mix.h"
#include "core/aud/bel.h"
#include "core/aud/rsmp.h"
#include "core/aud/opl.h"
#include <stdatomic.h>
#include "core/aud/psg.h"
#include "core/aud/sine.h"
#include "core/dap/dbg.h"
#include <string.h>

/* What the sink runs at until it says otherwise. libretro declares 48000 to
 * its frontend and never calls aud_set_sink_rate; a window host asks its
 * device and reports back what it was given. */
#define AUD_SINK_RATE 48000
static uint32_t g_sink_rate = AUD_SINK_RATE;

static aud_dev_t aud_dev;
static void (*aud_probe)(int16_t *left, int16_t *right);

/* One resampler per channel, carried across renders so the phase is
 * continuous. */
static rsmp_t g_rs_l, g_rs_r;

/* What the last input yielded past the end of a buffer, for the start of the
 * next. Only a sink faster than the machine leaves samples over. */
static int32_t g_pend_l[8], g_pend_r[8];
static int g_pend_n, g_pend_i;

static float g_last_l, g_last_r;

#define AUD_VIZ_SAMPLES 4096
static float g_viz[AUD_VIZ_SAMPLES];
static int g_viz_pos;

/* A session setting rather than machine state, so a reset leaves it
 * alone. */
static bool g_enabled = true;

static _Atomic unsigned g_park_gen;
static _Atomic unsigned g_park_ack;
static _Atomic bool g_park;

void aud_park_request(void)
{
    atomic_fetch_add(&g_park_gen, 1);
    atomic_store(&g_park, true);
}

bool aud_parked(void)
{
    return atomic_load(&g_park) &&
           atomic_load(&g_park_ack) == atomic_load(&g_park_gen);
}

void aud_park_release(void)
{
    atomic_store(&g_park, false);
}

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
    aud_dev = aud_dev_none;
    aud_probe = NULL;
}

void aud_stop(void)
{
    aud_dev = aud_dev_none;
}

void aud_setup(aud_dev_t dev)
{
    aud_dev = dev;
    aud_probe = NULL;
}

aud_dev_t aud_device(void) { return aud_dev; }

void aud_setup_probe(void (*sample)(int16_t *left, int16_t *right))
{
    aud_dev = aud_dev_none;
    aud_probe = sample;
}

void aud_set_sink_rate(uint32_t rate)
{
    if (rate)
        g_sink_rate = rate;
}

static inline float to_f(int32_t v)
{
    /* A windowed sinc overshoots full scale on transients, and the host's
     * converter is the end of the path, so the overshoot stops here. */
    if (v > 32767)
        v = 32767;
    if (v < -32768)
        v = -32768;
    return (float)v / 32768.0f;
}

/* One frame at AUD_NATIVE_RATE, clamped to sixteen bits as wiring.sv clamps
 * the fabric's own sum. */
static void mix(int32_t *left, int32_t *right)
{
    int16_t l = 0, r = 0;
    if (aud_probe)
        aud_probe(&l, &r);
    else
        switch (aud_dev)
        {
        case aud_dev_psg: psg_sample(&l, &r); break;
        case aud_dev_opl: opl_stereo(&l, &r); break;
        case aud_dev_none: break;
        }
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
    /* The acknowledgement is stored ahead of the tests below, because a
     * muted or debugger-held machine renders nothing and a park that waited
     * on those paths would run to its bound. */
    if (atomic_load(&g_park))
    {
        atomic_store(&g_park_ack, atomic_load(&g_park_gen));
        for (int i = 0; i < samples; i++)
        {
            dst[i * 2] = g_last_l;
            dst[i * 2 + 1] = g_last_r;
        }
        return 0;
    }
    if (!g_enabled)
    {
        memset(dst, 0, (size_t)samples * 2 * sizeof *dst);
        return 0;
    }
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

static void aud_put_rsmp(sst_cursor_t *c, const rsmp_t *r)
{
    for (int i = 0; i < RSMP_TAPS; i++)
        sst_put_i32(c, r->hist[i]);
    sst_put_u64(c, r->phase);
    sst_put_bool(c, r->primed);
}

static void aud_get_rsmp(sst_cursor_t *c, rsmp_t *r)
{
    for (int i = 0; i < RSMP_TAPS; i++)
        r->hist[i] = sst_get_i32(c);
    r->phase = sst_get_u64(c);
    r->primed = sst_get_bool(c);
}

/* The held level goes out as the sixteen bits it came in as. It is only ever
 * a sample the mixer already clamped, so the float carries nothing the
 * integer does not, and a float in a savestate is a byte two builds could
 * disagree about. */
static int16_t aud_to_i16(float v)
{
    int32_t s = (int32_t)(v * 32768.0f);
    if (s > AUD_SAMPLE_MAX)
        s = AUD_SAMPLE_MAX;
    if (s < AUD_SAMPLE_MIN)
        s = AUD_SAMPLE_MIN;
    return (int16_t)s;
}

void aud_sst_save(sst_cursor_t *c, unsigned flags)
{
    sst_put_u8(c, (uint8_t)aud_dev);
    aud_put_rsmp(c, &g_rs_l);
    aud_put_rsmp(c, &g_rs_r);
    for (int i = 0; i < 8; i++)
        sst_put_i32(c, g_pend_l[i]);
    for (int i = 0; i < 8; i++)
        sst_put_i32(c, g_pend_r[i]);
    sst_put_u8(c, (uint8_t)g_pend_n);
    sst_put_u8(c, (uint8_t)g_pend_i);
    sst_put_u16(c, (uint16_t)aud_to_i16(g_last_l));
    sst_put_u16(c, (uint16_t)aud_to_i16(g_last_r));
    bel_sst_save(c, flags);
}

bool aud_sst_load(sst_cursor_t *c, unsigned flags)
{
    uint8_t dev = sst_get_u8(c);
    rsmp_t l, r;
    aud_get_rsmp(c, &l);
    aud_get_rsmp(c, &r);
    int32_t pl[8], pr[8];
    for (int i = 0; i < 8; i++)
        pl[i] = sst_get_i32(c);
    for (int i = 0; i < 8; i++)
        pr[i] = sst_get_i32(c);
    uint8_t n = sst_get_u8(c), at = sst_get_u8(c);
    int16_t last_l = (int16_t)sst_get_u16(c);
    int16_t last_r = (int16_t)sst_get_u16(c);
    if (!sst_ok(c) || dev > aud_dev_opl || n > 8 || at > n)
        return false;

    aud_dev = (aud_dev_t)dev;
    aud_probe = NULL;
    g_rs_l = l;
    g_rs_r = r;
    memcpy(g_pend_l, pl, sizeof g_pend_l);
    memcpy(g_pend_r, pr, sizeof g_pend_r);
    g_pend_n = n;
    g_pend_i = at;
    g_last_l = (float)last_l / 32768.0f;
    g_last_r = (float)last_r / 32768.0f;
    return bel_sst_load(c, flags);
}
