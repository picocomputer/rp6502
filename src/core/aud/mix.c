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
#include "core/sys/xram.h"
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

/* ---------------- what the machine hands over ---------------- */

/* Each change to an engine, stamped with the native sample it applies
 * before. The machine's thread writes them at aud_ev_w and the render reads
 * them at aud_ev_r. A 6502 at 8 MHz storing to RW0 flat out makes one every
 * four cycles, 33,300 a frame, and the ring holds that and the sink's lead
 * behind it. */
#define AUD_EV_PSG 0x01     /* a write to the PSG's block */
#define AUD_EV_OPL 0x02     /* a write to a register on the OPL2's page */
#define AUD_EV_PSG_REG 0x04 /* byte addr of the PSG's block as XRAM held it */
#define AUD_EV_PSG_MAP 0x08 /* psg_map(addr) */
#define AUD_EV_OPL_MAP 0x10 /* opl_map(addr) */

typedef struct
{
    uint32_t at;
    uint16_t addr;
    uint8_t val;
    uint8_t kind;
} aud_ev_t;

#define AUD_EV_SIZE 65536
static aud_ev_t aud_ev[AUD_EV_SIZE];
static _Atomic uint32_t aud_ev_w, aud_ev_r;

/* Where the machine has the engines. The render reaches the same place
 * once it makes the sample each map is stamped for. */
static uint16_t aud_psg_at = 0xFFFF, aud_opl_at = 0xFFFF;

/* The native sample the machine had reached at the end of its last frame. */
static _Atomic uint64_t aud_now;

/* ---------------- the render ---------------- */

/* Held by the render and by the machine's thread whenever it reaches into
 * the render's state. */
static atomic_flag aud_busy = ATOMIC_FLAG_INIT;

/* The index of the next native sample. */
static uint64_t aud_made;

/* A change stamped at applies before sample at + aud_off. A sink that pulls
 * once a frame keeps it at 0. */
static int64_t aud_off;

/* One resampler per channel, carried across runs so the phase is
 * continuous. */
static rsmp_t aud_rs_l, aud_rs_r;

/* The PSG's channel block as the render has it. */
static uint8_t aud_psg_regs[PSG_REGS_SIZE];

/* Frames a native sample made past the end of a streaming render, the first
 * of the next. */
static int16_t aud_carry[8][2];
static int aud_carry_n;

static float aud_last_l, aud_last_r;

#define AUD_VIZ_SAMPLES 4096
static float aud_viz[AUD_VIZ_SAMPLES];
static int aud_viz_at;

/* A session setting rather than machine state, so a reset leaves it
 * alone. */
static bool aud_on = true;

static _Atomic bool aud_streaming;

/* The streaming trim works on headroom: at each render, how many native
 * samples past the end of what it asks for the machine has stamped. Each
 * half second the least of it is taken. Below zero, changes for samples the
 * render made had not been stamped yet and landed late; above, they waited
 * longer than they had to. A PI loop on it, critically damped with a natural
 * frequency of a quarter radian a second, stretches the resampler's step by
 * at most 0.5%, the deviation Arntzen's dynamic rate control for emulators
 * found no listener could detect. A least beyond what that trim takes a
 * second to remove, two windows running, moves aud_off instead: the start,
 * or a machine held up, where a gap or a skip in the sound would be heard
 * and changes landing a little early or late are not. */
#define AUD_TRIM_W 0.5
#define AUD_TRIM_MAX 0.005
#define AUD_JUMP ((int64_t)(AUD_TRIM_MAX * AUD_NATIVE_RATE))
static bool aud_anchored;
static int64_t aud_low, aud_low_was;
static uint32_t aud_window;
static double aud_trim_i, aud_trim;

/* A streaming sink this far behind has stopped asking, and the machine's
 * thread brings the render along: four frames. */
#define AUD_STALL ((int64_t)AUD_NATIVE_RATE * 4 / VGA_HZ)

static void aud_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&aud_busy, memory_order_acquire))
        ;
}

static void aud_unlock(void)
{
    atomic_flag_clear_explicit(&aud_busy, memory_order_release);
}

void aud_set_enabled(bool on) { aud_on = on; }
bool aud_enabled(void) { return aud_on; }

void aud_init(void)
{
    aud_lock();
    sine_init();
    bel_init();
    rsmp_reset(&aud_rs_l);
    rsmp_reset(&aud_rs_r);
    atomic_store(&aud_ev_r, atomic_load(&aud_ev_w));
    aud_psg_at = psg_xaddr_get();
    aud_opl_at = opl_xaddr_get();
    aud_made = bus_now(AUD_NATIVE_RATE);
    atomic_store(&aud_now, aud_made);
    aud_off = 0;
    aud_anchored = false;
    aud_carry_n = 0;
    aud_last_l = aud_last_r = 0;
    aud_probe = NULL;
    aud_unlock();
}

void aud_stop(void)
{
    aud_lock();
    atomic_store(&aud_ev_r, atomic_load(&aud_ev_w));
    psg_park();
    opl_park();
    aud_psg_at = aud_opl_at = 0xFFFF;
    aud_unlock();
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

void aud_engine_lock(void) { aud_lock(); }
void aud_engine_unlock(void) { aud_unlock(); }

static void aud_apply(const aud_ev_t *e)
{
    switch (e->kind)
    {
    case AUD_EV_PSG_MAP:
        psg_map(e->addr);
        return;
    case AUD_EV_OPL_MAP:
        opl_map(e->addr);
        return;
    case AUD_EV_PSG_REG:
        aud_psg_regs[e->addr] = e->val;
        return;
    }
    if (e->kind & AUD_EV_PSG)
    {
        aud_psg_regs[(uint16_t)(e->addr - psg_xaddr_get())] = e->val;
        psg_xram_write((uint8_t)e->addr, e->val);
    }
    if (e->kind & AUD_EV_OPL)
        opl_xram_write((uint8_t)e->addr, e->val);
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
            psg_sample(aud_psg_regs, &l, &r);
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

/* Every change stamped for the next native sample. */
static void aud_due(void)
{
    const uint32_t w = atomic_load_explicit(&aud_ev_w, memory_order_acquire);
    uint32_t r = atomic_load_explicit(&aud_ev_r, memory_order_relaxed);
    const uint32_t due = (uint32_t)(aud_made - (uint64_t)aud_off);
    for (; r != w && (int32_t)(due - aud_ev[r % AUD_EV_SIZE].at) >= 0; r++)
        aud_apply(&aud_ev[r % AUD_EV_SIZE]);
    atomic_store_explicit(&aud_ev_r, r, memory_order_release);
}

static void aud_next(int32_t *left, int32_t *right)
{
    aud_due();
    mix(left, right);
    aud_made++;
}

/* Native samples made up to sample `to` and not resampled, for a sink that
 * is not taking them, and the changes stamped for the one after. */
static void aud_skip(uint64_t to)
{
    int32_t l, r;
    while (aud_made < to)
        aud_next(&l, &r);
    aud_due();
}

static void aud_put(float *dst, int i, int32_t l, int32_t r)
{
    aud_last_l = dst[i * 2] = aud_clip(l) / 32768.0f;
    aud_last_r = dst[i * 2 + 1] = aud_clip(r) / 32768.0f;
    aud_viz[aud_viz_at] = (aud_last_l + aud_last_r) * 0.5f;
    aud_viz_at = (aud_viz_at + 1) % AUD_VIZ_SAMPLES;
}

static void aud_hold(float *dst, int from, int samples)
{
    for (int i = from; i < samples; i++)
    {
        dst[i * 2] = aud_last_l;
        dst[i * 2 + 1] = aud_last_r;
    }
}

static uint64_t aud_ratio(void)
{
    return rsmp_step(AUD_NATIVE_RATE, atomic_load(&aud_sink_rate));
}

/* A ring still full holds a sink that stopped asking, or a machine run
 * faster than its sink, and the render is brought along until it has
 * room. */
static void aud_overflow(uint32_t w)
{
    aud_lock();
    int32_t l, r;
    while (w - atomic_load_explicit(&aud_ev_r, memory_order_acquire) == AUD_EV_SIZE)
        aud_next(&l, &r);
    aud_unlock();
}

static void aud_push(uint8_t kind, uint16_t addr, uint8_t val)
{
    const uint32_t w = atomic_load_explicit(&aud_ev_w, memory_order_relaxed);
    if (w - atomic_load_explicit(&aud_ev_r, memory_order_acquire) == AUD_EV_SIZE)
        aud_overflow(w);
    aud_ev[w % AUD_EV_SIZE] = (aud_ev_t){(uint32_t)bus_now(AUD_NATIVE_RATE), addr, val, kind};
    atomic_store_explicit(&aud_ev_w, w + 1, memory_order_release);
}

void aud_xram_write(uint16_t addr, uint8_t val)
{
    uint8_t kind = 0;
    if (aud_psg_at != 0xFFFF && (uint16_t)(addr - aud_psg_at) < PSG_REGS_SIZE)
        kind |= AUD_EV_PSG;
    if ((addr & 0xFF00) == aud_opl_at && opl_is_reg((uint8_t)addr))
        kind |= AUD_EV_OPL;
    if (kind)
        aud_push(kind, addr, val);
}

/* The render reads the PSG's block from its own copy, so a map sends the
 * block as XRAM has it with the map's stamp. */
static void aud_psg_send(void)
{
    for (uint16_t i = 0; i < PSG_REGS_SIZE; i++)
        aud_push(AUD_EV_PSG_REG, i, xram[aud_psg_at + i]);
}

void aud_psg_map(uint16_t word)
{
    aud_psg_at = word;
    aud_push(AUD_EV_PSG_MAP, word, 0);
    if (word != 0xFFFF)
        aud_psg_send();
}

/* opl_xreg has cleared the page by now, the PSG's block too if it is
 * there. */
void aud_opl_map(uint16_t word)
{
    aud_opl_at = word;
    aud_push(AUD_EV_OPL_MAP, word, 0);
    if (word != 0xFFFF && aud_psg_at != 0xFFFF && (aud_psg_at & 0xFF00) == word)
        aud_psg_send();
}

void aud_set_sink_rate(uint32_t rate)
{
    if (rate)
        atomic_store(&aud_sink_rate, rate);
}

void aud_set_streaming(bool on)
{
    aud_lock();
    atomic_store(&aud_streaming, on);
    aud_anchored = false;
    aud_trim_i = aud_trim = 0;
    aud_carry_n = 0;
    aud_unlock();
}

void aud_step(void)
{
    const uint64_t now = bus_now(AUD_NATIVE_RATE);
    const uint64_t was = atomic_exchange(&aud_now, now);
    if (!aud_on)
    {
        aud_lock();
        aud_skip(now + (uint64_t)aud_off);
        aud_unlock();
    }
    else if (!atomic_load(&aud_streaming))
    {
        if (dbg_is_stopped() && sys_running())
            return;
        aud_lock();
        aud_skip(was);
        aud_unlock();
    }
    else if (!atomic_flag_test_and_set_explicit(&aud_busy, memory_order_acquire))
    {
        const uint64_t to = now + (uint64_t)aud_off;
        if ((int64_t)(to - aud_made) > AUD_STALL)
            aud_skip(to);
        aud_unlock();
    }
}

uint64_t aud_samples(void) { return aud_made; }

/* The end of a window of the streaming trim. */
static void aud_settle(uint32_t rate)
{
    const int64_t low = aud_low, was = aud_low_was;
    const double dt = (double)aud_window / rate;
    aud_low = INT64_MAX;
    aud_window = 0;
    if ((low > AUD_JUMP && was > AUD_JUMP) || (low < -AUD_JUMP && was < -AUD_JUMP))
    {
        const int64_t by = low > 0 ? (low < was ? low : was) : (low > was ? low : was);
        aud_off -= by;
        aud_low_was = low - by;
        return;
    }
    aud_low_was = low;
    const double e = (double)(low > AUD_JUMP ? AUD_JUMP : low < -AUD_JUMP ? -AUD_JUMP : low);
    const double r = AUD_NATIVE_RATE;
    aud_trim_i += AUD_TRIM_W * AUD_TRIM_W / r * e * dt;
    if (aud_trim_i > AUD_TRIM_MAX)
        aud_trim_i = AUD_TRIM_MAX;
    if (aud_trim_i < -AUD_TRIM_MAX)
        aud_trim_i = -AUD_TRIM_MAX;
    aud_trim = 2 * AUD_TRIM_W / r * e + aud_trim_i;
    if (aud_trim > AUD_TRIM_MAX)
        aud_trim = AUD_TRIM_MAX;
    if (aud_trim < -AUD_TRIM_MAX)
        aud_trim = -AUD_TRIM_MAX;
}

/* A streaming render: everything asked for, made now. The first sets
 * aud_off so that what it asks for has just been stamped. */
static int aud_stream(float *dst, int samples)
{
    const uint32_t rate = atomic_load(&aud_sink_rate);
    uint64_t step = aud_ratio();
    step += (uint64_t)(int64_t)((double)step * aud_trim);
    const int64_t need = (int64_t)(((uint64_t)samples * step) >> 32);
    int64_t h = (int64_t)atomic_load(&aud_now) + aud_off - (int64_t)aud_made - need;
    if (!aud_anchored)
    {
        aud_off -= h;
        h = 0;
        aud_anchored = true;
        aud_low = INT64_MAX;
        aud_low_was = 0;
        aud_window = 0;
    }
    if (h < aud_low)
        aud_low = h;
    int at = 0, kept = 0;
    for (int i = 0; i < aud_carry_n; i++)
        if (at < samples)
            aud_put(dst, at++, aud_carry[i][0], aud_carry[i][1]);
        else
            aud_carry[kept][0] = aud_carry[i][0], aud_carry[kept++][1] = aud_carry[i][1];
    aud_carry_n = kept;
    while (at < samples)
    {
        int32_t l, r, out_l[8], out_r[8];
        aud_next(&l, &r);
        const int n = rsmp_push(&aud_rs_l, l, step, out_l, 8);
        rsmp_push(&aud_rs_r, r, step, out_r, 8);
        for (int i = 0; i < n; i++)
            if (at < samples)
                aud_put(dst, at++, out_l[i], out_r[i]);
            else
            {
                aud_carry[aud_carry_n][0] = aud_clip(out_l[i]);
                aud_carry[aud_carry_n++][1] = aud_clip(out_r[i]);
            }
    }
    aud_window += (uint32_t)samples;
    if (aud_window >= rate / 2)
        aud_settle(rate);
    return samples;
}

static void aud_reverse(float *dst, int from, int to)
{
    for (to--; from < to; from++, to--)
        for (int c = 0; c < 2; c++)
        {
            const float t = dst[from * 2 + c];
            dst[from * 2 + c] = dst[to * 2 + c];
            dst[to * 2 + c] = t;
        }
}

/* A render for a sink on the machine's thread: the samples up to the
 * machine's present, the newest `samples` of them kept. */
static int aud_take(float *dst, int samples)
{
    const uint64_t to = bus_now(AUD_NATIVE_RATE);
    const uint64_t step = aud_ratio();
    int total = 0;
    while (aud_made < to)
    {
        int32_t l, r, out_l[8], out_r[8];
        aud_next(&l, &r);
        const int n = rsmp_push(&aud_rs_l, l, step, out_l, 8);
        rsmp_push(&aud_rs_r, r, step, out_r, 8);
        for (int i = 0; i < n; i++, total++)
            aud_put(dst, total % samples, out_l[i], out_r[i]);
    }
    aud_due();
    if (total > samples)
    {
        const int oldest = total % samples;
        aud_reverse(dst, 0, oldest);
        aud_reverse(dst, oldest, samples);
        aud_reverse(dst, 0, samples);
    }
    const int made = total < samples ? total : samples;
    aud_hold(dst, made, samples);
    return made;
}

int aud_render(float *dst, int samples)
{
    aud_lock();
    int made = 0;
    if (!aud_on)
        memset(dst, 0, (size_t)samples * 2 * sizeof *dst);
    else if (dbg_is_stopped() && sys_running())
        aud_hold(dst, 0, samples);
    else if (atomic_load(&aud_streaming))
        made = aud_stream(dst, samples);
    else
        made = aud_take(dst, samples);
    aud_unlock();
    return made;
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
    aud_lock();
    if (!(engines & 1))
        psg_park();
    if (!(engines & 2))
        opl_park();
    /* What was stamped belongs to the time this load left. */
    atomic_store(&aud_ev_r, atomic_load(&aud_ev_w));
    aud_psg_at = psg_xaddr_get();
    aud_opl_at = opl_xaddr_get();
    if (aud_psg_at != 0xFFFF)
        memcpy(aud_psg_regs, (const void *)&xram[aud_psg_at], PSG_REGS_SIZE);
    aud_probe = NULL;
    aud_rs_l = l;
    aud_rs_r = r;
    aud_made = made;
    atomic_store(&aud_now, made);
    aud_off = 0;
    aud_anchored = false;
    aud_carry_n = 0;
    const bool ok = bel_sst_load(c, flags);
    aud_unlock();
    return ok;
}
