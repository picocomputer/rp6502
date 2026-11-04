/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/aud/mix.h"
#include "core/aud/bel.h"
#include "core/aud/rsmp.h"
#include "core/aud/opl.h"
#include "core/aud/psg.h"
#include "core/aud/sine.h"
#include "core/dap/dbg.h"
#include "core/sys/sys.h"
#include "core/vga/vga_emu.h"
#include "core/wdc/bus.h"
#include <stdatomic.h>
#include <string.h>

/* What the sink runs at until it says otherwise. libretro declares 48000 to
 * its frontend and never calls aud_set_sink_rate; a window host asks its
 * device and reports back what it was given. */
#define AUD_SINK_RATE 48000
static _Atomic uint32_t aud_sink_rate = AUD_SINK_RATE;

static void (*aud_probe)(int16_t *left, int16_t *right);

/* The index of the next native sample, counted from scanline 0. */
static uint64_t aud_made;

/* One resampler per channel, carried across runs so the phase is
 * continuous. */
static rsmp_t aud_rs_l, aud_rs_r;

/* Frames at the sink's rate, written at aud_w by the machine's thread and
 * read at aud_r by the sink's. */
#define AUD_BUF_FRAMES 16384
static int16_t aud_buf[AUD_BUF_FRAMES][2];
static _Atomic uint32_t aud_w, aud_r;

/* A streaming sink's device buffer, 0 for a sink that pulls once a frame. */
static _Atomic int aud_sink_frames;

/* Set while a streaming sink waits for its buffer to fill to the target. */
static _Atomic bool aud_refill = true;

/* The streaming trim: the buffer's level smoothed, the integral term, and the
 * fraction the resampler's step is stretched by. The machine's thread owns
 * all three. */
static double aud_fill_avg, aud_trim_i, aud_trim;

/* The trim is a PI loop on the buffer's level, critically damped with a
 * natural frequency of a quarter radian a second, on a level smoothed over
 * half a second, and held within 1%. */
#define AUD_TRIM_W 0.25
#define AUD_TRIM_TAU 0.5
#define AUD_TRIM_MAX 0.01

static float aud_last_l, aud_last_r;

#define AUD_VIZ_SAMPLES 4096
static float aud_viz[AUD_VIZ_SAMPLES];
static int aud_viz_at;

/* A session setting rather than machine state, so a reset leaves it
 * alone. */
static bool aud_on = true;

void aud_set_enabled(bool on) { aud_on = on; }
bool aud_enabled(void) { return aud_on; }

void aud_init(void)
{
    sine_init();
    bel_init();
    rsmp_reset(&aud_rs_l);
    rsmp_reset(&aud_rs_r);
    aud_made = bus_now(AUD_NATIVE_RATE);
    aud_last_l = aud_last_r = 0;
    aud_probe = NULL;
}

void aud_stop(void)
{
    aud_engine_lock();
    psg_park();
    opl_park();
    aud_engine_unlock();
}

/* For a host that unloads the library rather than ending the process. The
 * engines are parked first, so no later step reaches one being reset. */
void aud_shutdown(void)
{
    aud_probe = NULL;
    aud_stop();
    opl_shutdown();
}

void aud_setup_probe(void (*sample)(int16_t *left, int16_t *right))
{
    aud_probe = sample;
}

void aud_engine_lock(void)
{
    aud_step();
}

void aud_engine_unlock(void)
{
}

/* The pointers move only on this thread, in psg_xreg and opl_xreg, so most
 * writes, which miss both pages, cost two compares. */
void aud_xram_write(uint16_t addr, uint8_t val)
{
    uint16_t page = addr & 0xFF00;
    uint16_t psg = psg_xaddr_get();
    bool to_psg = psg != 0xFFFF && page == (psg & 0xFF00);
    bool to_opl = page == opl_xaddr_get();
    if (!to_psg && !to_opl)
        return;
    aud_engine_lock();
    if (to_psg)
        psg_xram_write((uint8_t)addr, val);
    if (to_opl)
        opl_xram_write((uint8_t)addr, val);
    aud_engine_unlock();
}

void aud_set_sink_rate(uint32_t rate)
{
    if (rate)
        atomic_store(&aud_sink_rate, rate);
}

/* The level a streaming sink is held at. */
static uint32_t aud_target(int frames)
{
    return (uint32_t)frames + atomic_load(&aud_sink_rate) / VGA_HZ;
}

void aud_set_sink_buffer(int frames)
{
    atomic_store(&aud_sink_frames, frames);
    atomic_store(&aud_refill, true);
    aud_fill_avg = frames ? aud_target(frames) : 0;
    aud_trim_i = aud_trim = 0;
}

/* Sixteen bits: the mix's sum, as wiring.sv clamps it in the fabric, and the
 * resampler's overshoot of full scale on transients. */
static inline int16_t aud_clip(int32_t v)
{
    if (v > AUD_SAMPLE_MAX)
        v = AUD_SAMPLE_MAX;
    if (v < AUD_SAMPLE_MIN)
        v = AUD_SAMPLE_MIN;
    return (int16_t)v;
}

/* mix() writes one frame at AUD_NATIVE_RATE: every mapped engine and the
 * bell summed, then clamped to sixteen bits. */
static void mix(int32_t *left, int32_t *right)
{
    int32_t sl = 0, sr = 0;
    int16_t l, r;
    if (aud_probe)
    {
        l = r = 0;
        aud_probe(&l, &r);
        sl += l, sr += r;
    }
    else
    {
        if (psg_xaddr_get() != 0xFFFF)
        {
            psg_sample(&l, &r);
            sl += l, sr += r;
        }
        if (opl_xaddr_get() != 0xFFFF)
        {
            const int32_t o = opl_sample();
            sl += o, sr += o;
        }
    }
    const int32_t bel = bel_sample();
    sl += bel;
    sr += bel;
    *left = aud_clip(sl);
    *right = aud_clip(sr);
}

/* One update of the streaming trim, after `made` frames left the buffer
 * `fill` deep. The level counted is the middle of the run, which is the
 * buffer's average across it. A refill and a buffer past three times the
 * target are the sink's to settle, so they hold the loop where it is. */
static void aud_trim_update(uint32_t fill, uint32_t made, int frames)
{
    const double target = aud_target(frames);
    if (atomic_load(&aud_refill) || fill > 3 * target)
    {
        aud_fill_avg = target;
        return;
    }
    const double rate = atomic_load(&aud_sink_rate);
    double k = made / (AUD_TRIM_TAU * rate);
    if (k > 1)
        k = 1;
    aud_fill_avg += (fill - made / 2.0 - aud_fill_avg) * k;
    const double e = aud_fill_avg - target;
    aud_trim_i += AUD_TRIM_W * AUD_TRIM_W / rate * e * made / rate;
    if (aud_trim_i > AUD_TRIM_MAX)
        aud_trim_i = AUD_TRIM_MAX;
    if (aud_trim_i < -AUD_TRIM_MAX)
        aud_trim_i = -AUD_TRIM_MAX;
    aud_trim = 2 * AUD_TRIM_W / rate * e + aud_trim_i;
    if (aud_trim > AUD_TRIM_MAX)
        aud_trim = AUD_TRIM_MAX;
    if (aud_trim < -AUD_TRIM_MAX)
        aud_trim = -AUD_TRIM_MAX;
}

void aud_step(void)
{
    const uint64_t to = bus_now(AUD_NATIVE_RATE);
    if (aud_made >= to)
        return;
    const int frames = atomic_load(&aud_sink_frames);
    uint64_t step = rsmp_step(AUD_NATIVE_RATE, atomic_load(&aud_sink_rate));
    if (frames)
        step += (uint64_t)(int64_t)((double)step * aud_trim);
    uint32_t w = atomic_load_explicit(&aud_w, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&aud_r, memory_order_acquire);
    const uint32_t from = w;
    for (; aud_made < to; aud_made++)
    {
        int32_t l, rr;
        mix(&l, &rr);
        if (!aud_on)
            continue;
        int32_t out_l[8], out_r[8];
        const int n = rsmp_push(&aud_rs_l, l, step, out_l, 8);
        rsmp_push(&aud_rs_r, rr, step, out_r, 8);
        for (int i = 0; i < n; i++)
        {
            /* A streaming sink's thread owns aud_r, so a full buffer drops
             * the newest; a sink on this thread loses the oldest. */
            if (w - r == AUD_BUF_FRAMES)
            {
                if (frames)
                    continue;
                r++;
            }
            aud_buf[w % AUD_BUF_FRAMES][0] = aud_clip(out_l[i]);
            aud_buf[w % AUD_BUF_FRAMES][1] = aud_clip(out_r[i]);
            w++;
        }
    }
    if (!frames)
        atomic_store_explicit(&aud_r, r, memory_order_relaxed);
    atomic_store_explicit(&aud_w, w, memory_order_release);
    if (frames && w != from)
        aud_trim_update(w - r, w - from, frames);
}

uint64_t aud_samples(void) { return aud_made; }

int aud_ready(void)
{
    return (int)(atomic_load(&aud_w) - atomic_load(&aud_r));
}

static void aud_hold(float *dst, int from, int samples)
{
    for (int i = from; i < samples; i++)
    {
        dst[i * 2] = aud_last_l;
        dst[i * 2 + 1] = aud_last_r;
    }
}

int aud_render(float *dst, int samples)
{
    if (!aud_on)
    {
        memset(dst, 0, (size_t)samples * 2 * sizeof *dst);
        return 0;
    }
    if (dbg_is_stopped() && sys_running())
    {
        aud_hold(dst, 0, samples);
        return 0;
    }
    const uint32_t w = atomic_load_explicit(&aud_w, memory_order_acquire);
    uint32_t r = atomic_load_explicit(&aud_r, memory_order_relaxed);
    uint32_t have = w - r;
    const int frames = atomic_load(&aud_sink_frames);
    if (frames)
    {
        /* The target is the level averaged over time, which is half a pull
         * less than the level found at a pull. */
        const uint32_t target = aud_target(frames);
        const uint32_t start = target + (uint32_t)samples / 2;
        const bool refill = atomic_load(&aud_refill);
        if (refill && have < start)
        {
            aud_hold(dst, 0, samples);
            return 0;
        }
        if (refill || have > 3 * target)
            r = w - start, have = start;
        if (refill)
            atomic_store(&aud_refill, false);
    }
    else if (have > (uint32_t)samples)
        r = w - (uint32_t)samples, have = (uint32_t)samples;
    const int n = have < (uint32_t)samples ? (int)have : samples;
    for (int i = 0; i < n; i++, r++)
    {
        aud_last_l = dst[i * 2] = aud_buf[r % AUD_BUF_FRAMES][0] / 32768.0f;
        aud_last_r = dst[i * 2 + 1] = aud_buf[r % AUD_BUF_FRAMES][1] / 32768.0f;
        aud_viz[aud_viz_at] = (aud_last_l + aud_last_r) * 0.5f;
        aud_viz_at = (aud_viz_at + 1) % AUD_VIZ_SAMPLES;
    }
    atomic_store_explicit(&aud_r, r, memory_order_release);
    aud_hold(dst, n, samples);
    if (frames && n < samples)
        atomic_store(&aud_refill, true);
    return n;
}

const float *aud_viz_buffer(int *num_samples)
{
    *num_samples = AUD_VIZ_SAMPLES;
    return aud_viz;
}

int aud_viz_pos(void) { return aud_viz_at; }

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

void aud_sst_save(sst_cursor_t *c, unsigned flags)
{
    sst_put_u8(c, (uint8_t)((psg_xaddr_get() != 0xFFFF ? 1 : 0) |
                            (opl_xaddr_get() != 0xFFFF ? 2 : 0)));
    aud_put_rsmp(c, &aud_rs_l);
    aud_put_rsmp(c, &aud_rs_r);
    sst_put_u64(c, aud_made);
    bel_sst_save(c, flags);
}

bool aud_sst_load(sst_cursor_t *c, unsigned flags)
{
    uint8_t engines = sst_get_u8(c);
    rsmp_t l, r;
    aud_get_rsmp(c, &l);
    aud_get_rsmp(c, &r);
    uint64_t made = sst_get_u64(c);
    if (!sst_ok(c) || engines > 3)
        return false;
    /* This row loads after the engines' own, and an engine it names has to
     * have come back mapped. */
    if (((engines & 1) && psg_xaddr_get() == 0xFFFF) ||
        ((engines & 2) && opl_xaddr_get() == 0xFFFF))
        return false;
    if (!(engines & 1))
        psg_park();
    if (!(engines & 2))
        opl_park();

    aud_probe = NULL;
    aud_rs_l = l;
    aud_rs_r = r;
    aud_made = made;
    return bel_sst_load(c, flags);
}
