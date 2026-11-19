/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/aud/mix.h"
#include "core/aud/bel.h"
#include "core/aud/rsmp.h"
#include "core/aud/opl.h"
#include "core/aud/pcm.h"
#include "core/aud/psg.h"
#include "core/dap/dbg.h"
#include "core/sys/ring.h"
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

/* ---------------- what the machine hands over ---------------- */

/* Each change to an engine, stamped with the native sample it applies
 * before. The machine's thread writes them and the render reads them. A 6502 at 8 MHz storing to RW0 flat out makes one every
 * four cycles, 33,300 a frame, and the ring holds that and the sink's lead
 * behind it. */
#define AUD_EV_PSG 0x01 /* a write to the PSG's 64 bytes */
#define AUD_EV_OPL 0x02 /* a write to a register in the OPL2's 256 */
/* A write is one event with either kind or both, its addr the byte's offset
 * in the PSG's block in the high byte and in the OPL2's in the low. */
#define AUD_EV_PSG_MAP 0x08 /* psg_map(addr) */
#define AUD_EV_OPL_MAP 0x10 /* opl_map(addr) */
/* The PCM: device: opened at rate addr, 16-bit if val's bit 0 and stereo
 * if its bit 1; bytes written, addr's low then high byte then val, as many
 * as the kind says; the buffer emptied; and closed. */
#define AUD_EV_PCM_OPEN 0x20
#define AUD_EV_PCM_DATA1 0x21
#define AUD_EV_PCM_DATA2 0x22
#define AUD_EV_PCM_DATA3 0x23
#define AUD_EV_PCM_CLEAR 0x24
#define AUD_EV_PCM_CLOSE 0x25
#define AUD_EV_BEL 0x30 /* byte addr of the bell's structure */

typedef struct
{
    uint32_t at;
    uint16_t addr;
    uint8_t val;
    uint8_t kind;
} aud_ev_t;

static RING(aud_ev_t, 65536) aud_ev;

/* Where the machine has the engines. The render reaches the same place
 * once it makes the sample each map is stamped for. */
static uint16_t aud_psg_at = 0xFFFF, aud_opl_at = 0xFFFF;

/* The OPL2's queue as the machine sees it. A write is kept or dropped here,
 * by the rule opl.h gives and in the machine's own time, so the count is
 * right the moment the 6502 asks for it and nothing the sink does can change
 * it. The render's queue is handed the kept writes on the same samples, so
 * it takes and applies them on the same samples and never fills. */
static struct
{
    uint64_t at; /* the next sample to step */
    RING(uint8_t, OPL_QUEUE_SIZE) ring;
    bool late;
    uint32_t credit;
    uint32_t drops;
} aud_oq;

static void aud_oq_reset(uint64_t at)
{
    aud_oq.at = at;
    RING_RESET(&aud_oq.ring);
    aud_oq.late = false;
    aud_oq.credit = 96;
    aud_oq.drops = 0;
}

/* Every sample before `to`, as opl_commit steps them. */
static void aud_oq_step(uint64_t to)
{
    while (aud_oq.at < to)
    {
        if (!RING_LEN(&aud_oq.ring) && !aud_oq.late && aud_oq.credit == 96)
        {
            aud_oq.at = to;
            return;
        }
        const bool held = aud_oq.late;
        aud_oq.late = false;
        uint8_t reg;
        if (!held && aud_oq.credit >= 96 && RING_GET(&aud_oq.ring, &reg))
        {
            aud_oq.late = opl_is_late(reg);
            aud_oq.credit -= 96;
        }
        aud_oq.credit += 72;
        const uint32_t cap = RING_LEN(&aud_oq.ring) ? 192 : 96;
        if (aud_oq.credit > cap)
            aud_oq.credit = cap;
        aud_oq.at++;
    }
}

/* The PCM: device's buffer as the machine sees it, by the rule pcm_sample
 * takes frames with, so what a write takes, what SEEK_CUR says and the
 * underruns counted are the machine's own and nothing the sink does can
 * change them. The render's
 * buffer is handed the same bytes on the same samples and takes them on the
 * same samples. */
static struct
{
    bool on;
    pcm_fmt_t fmt;
    uint8_t frame;
    uint64_t step;
    uint64_t at; /* the next sample to step */
    uint64_t phase;
    uint16_t level;
    uint32_t underruns;
} aud_pm;

/* Every sample before `to`. A take with no whole frame to take is silence,
 * and nothing is written between two steps, so the buffer runs dry at most
 * once in one. */
static void aud_pm_step(uint64_t to)
{
    if (aud_pm.on && to > aud_pm.at)
    {
        uint64_t takes = pcm_takes(&aud_pm.phase, aud_pm.step, to - aud_pm.at);
        const uint64_t frames = aud_pm.level / aud_pm.frame;
        if (frames && takes >= frames && aud_pm.underruns < 0x7FFFFFFF)
            aud_pm.underruns++;
        if (takes > frames)
            takes = frames;
        aud_pm.level = (uint16_t)(aud_pm.level - takes * aud_pm.frame);
    }
    aud_pm.at = to;
}

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

/* The PCM: device's sound as the render has it. It steps by the machine's
 * samples, aud_made - aud_off, rather than the render's, so when aud_off
 * moves it holds or catches up and its buffer stays the machine's. due is
 * the machine sample its next step makes, and l and r what the last made. */
static pcm_t aud_pcm;
static bool aud_pcm_play;
static uint32_t aud_pcm_due;
static int32_t aud_pcm_l, aud_pcm_r;

/* Frames a native sample made past the end of a streaming render, the first
 * of the next. */
static int16_t aud_carry[8][2];
static int aud_carry_n;

static float aud_last_l, aud_last_r;

static float aud_viz[AUD_VIZ_SAMPLES];
static int aud_viz_at;

/* A session setting rather than machine state, so a reset leaves it
 * alone. */
static bool aud_on = true;

static _Atomic bool aud_streaming;

/* The streaming trim works on headroom: at each render, how many native
 * samples past the end of what it asks for the machine has stamped. A render
 * that finds less than none moves aud_off on by the shortfall, so a change
 * never lands after its sample, and the sound is that much further behind
 * the machine from then on.
 *
 * The resampler's step is first the machine's speed, which the host knows
 * (aud_set_speed): a machine that takes its frames from a 59.94 Hz display
 * makes a thousandth less sound a second than one on the wall clock. What
 * is left for the trim is the two crystals, the host's and the device's,
 * hundredths of a percent, and whatever the host's figure is off by.
 *
 * It takes the least headroom of each half second and steers by the least
 * of the last sixteen seconds of those. Frames do not come evenly from a
 * display the machine does not follow: at 800x600's 60.32 Hz each comes a
 * little earlier than the one before for three seconds, and then one comes a
 * whole frame late. The least of sixteen seconds is the bottom of that,
 * which stays where it is. A shortfall moves aud_off, and the leasts kept
 * with it, so they stay what they would have been where the render now is,
 * and it steers the loop once, in its own half second. A sink that runs
 * fast is short a little at every render, and plays on: that keeps it in
 * step by itself, a sample at a time, while the loop comes round to it.
 *
 * A PI loop on that, critically damped with a natural frequency of a
 * twentieth of a radian a second, stretches the resampler's step by at most
 * 0.5%, and the step moves no more than AUD_BEND in a half second. It is
 * that slow so that the pitch does not move: a loop ten times as fast bent
 * a steady tone by a third of a percent from one half second to the next,
 * which is heard. The loop is at its 0.5% with a least of AUD_JUMP, 50 ms,
 * and a least that far past one of the sink's requests, sixteen seconds
 * running, is past what it can remove and is given back by moving aud_off
 * back. That waits for
 * the OPL2's queue to be empty and goes only up to the next stamped change,
 * so the queue never sees two writes closer than the machine made them and
 * keeps or drops each as the Pico would. */
#define AUD_TRIM_W 0.05
#define AUD_TRIM_MAX 0.005
#define AUD_JUMP ((int64_t)(AUD_TRIM_MAX * AUD_NATIVE_RATE) * 10)
#define AUD_BEND 0.00025
#define AUD_LOWS 32
static _Atomic int32_t aud_speed; // the machine's, off the wall clock's, in parts of 1e9
static bool aud_anchored;
static int64_t aud_low, aud_short, aud_lows[AUD_LOWS];
static uint32_t aud_window;
static double aud_trim_i, aud_trim, aud_hint;

/* A machine that stopped is not a frame come late: a window being moved,
 * or the host busy for a while, takes time the machine never makes up. So
 * the shortfall since the machine last stepped is counted, and once it
 * passes two frames the trim, which has nothing to learn from it, waits for
 * the machine and starts over. */
#define AUD_GONE ((int64_t)AUD_NATIVE_RATE * 2 / VGA_HZ)
static uint64_t aud_seen;
static int64_t aud_dry;

/* A streaming sink this far behind has stopped asking, or the machine is
 * running faster than sound plays, and the machine's thread brings the
 * render along: eight frames, which is past what a sink's request, the
 * frame it waits on, what late frames have it hold and a burst after a stall
 * come to. The render then starts over where the machine is. */
#define AUD_STALL ((int64_t)AUD_NATIVE_RATE * 8 / VGA_HZ)

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

static void aud_apply(const aud_ev_t *e);

/* A stop or a reset empties what was stamped, but the bell is not the
 * program's and outlives both, so its bytes are taken now. */
static void aud_keep_bel(void)
{
    const uint32_t n = RING_LEN(&aud_ev);
    for (uint32_t i = 0; i < n; i++)
        if (RING_AT(&aud_ev, i)->kind == AUD_EV_BEL)
            aud_apply(RING_AT(&aud_ev, i));
    RING_SKIP(&aud_ev, n);
}

void aud_init(void)
{
    aud_lock();
    bel_init();
    rsmp_reset(&aud_rs_l);
    rsmp_reset(&aud_rs_r);
    aud_keep_bel();
    aud_psg_at = psg_xaddr_get();
    aud_opl_at = opl_xaddr_get();
    aud_made = bus_now(AUD_NATIVE_RATE);
    atomic_store(&aud_now, aud_made);
    aud_oq_reset(aud_made);
    aud_pm.on = false;
    aud_pm.underruns = 0;
    aud_pcm_play = false;
    aud_off = 0;
    aud_anchored = false;
    aud_carry_n = 0;
    aud_last_l = aud_last_r = 0;
    aud_probe = NULL;
    aud_unlock();
}

/* A program's stop parks the engines it mapped. PCM: is a descriptor, which
 * an exec hands on, so its sound plays on into the next program. */
void aud_stop(void)
{
    aud_lock();
    aud_keep_bel();
    psg_park();
    opl_park();
    aud_psg_at = aud_opl_at = 0xFFFF;
    aud_oq_reset(bus_now(AUD_NATIVE_RATE));
    aud_unlock();
}

/* The end of a chain takes its PCM: sound with it, so the close std_reset
 * makes next has nothing to wait for. */
void aud_end(void)
{
    aud_lock();
    aud_pm_step(bus_now(AUD_NATIVE_RATE));
    aud_pm.level = 0;
    aud_pm.underruns = 0;
    pcm_clear(&aud_pcm);
    aud_unlock();
}

void aud_setup_probe(void (*sample)(int16_t *left, int16_t *right))
{
    aud_probe = sample;
}

uint32_t aud_opl_overruns(void)
{
    const uint32_t drops = aud_oq.drops;
    aud_oq.drops = 0;
    return drops;
}

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
    case AUD_EV_PCM_OPEN:
    {
        const pcm_fmt_t fmt = {e->addr, (uint8_t)(e->val & 1 ? 16 : 8),
                               (uint8_t)(e->val & 2 ? 2 : 1)};
        pcm_reset(&aud_pcm, &fmt);
        aud_pcm_play = true;
        aud_pcm_due = e->at;
        aud_pcm_l = aud_pcm_r = 0;
        return;
    }
    case AUD_EV_PCM_DATA1:
    case AUD_EV_PCM_DATA2:
    case AUD_EV_PCM_DATA3:
    {
        const uint8_t b[3] = {(uint8_t)e->addr, (uint8_t)(e->addr >> 8), e->val};
        pcm_put(&aud_pcm, b, (uint32_t)(e->kind - AUD_EV_PCM_DATA1 + 1));
        return;
    }
    case AUD_EV_PCM_CLEAR:
        pcm_clear(&aud_pcm);
        return;
    case AUD_EV_PCM_CLOSE:
        aud_pcm_play = false;
        return;
    case AUD_EV_BEL:
        psg_put(PSG_REGS_SIZE + e->addr, e->val);
        return;
    }
    if (e->kind & AUD_EV_PSG)
        psg_put(e->addr >> 8, e->val);
    if (e->kind & AUD_EV_OPL)
        opl_push((uint8_t)e->addr, e->val);
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

/* aud_mix writes one frame at AUD_NATIVE_RATE: every mapped engine summed,
 * the bell inside the PSG's, then clamped to sixteen bits. */
static void aud_mix(int32_t *left, int32_t *right)
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
        if (psg_xaddr_get() != 0xFFFF || !psg_bel_idle())
        {
            psg_sample(&l, &r);
            sl += l, sr += r;
        }
        if (opl_xaddr_get() != 0xFFFF)
        {
            const int32_t o = opl_sample();
            sl += o, sr += o;
        }
        if (aud_pcm_play)
        {
            const uint32_t due = (uint32_t)(aud_made - (uint64_t)aud_off);
            for (; (int32_t)(due - aud_pcm_due) >= 0; aud_pcm_due++)
                pcm_sample(&aud_pcm, &aud_pcm_l, &aud_pcm_r);
            sl += aud_pcm_l, sr += aud_pcm_r;
        }
    }
    *left = aud_clip(sl);
    *right = aud_clip(sr);
}

/* Every change stamped for the next native sample. */
static void aud_due(void)
{
    const uint32_t n = RING_LEN(&aud_ev);
    const uint32_t due = (uint32_t)(aud_made - (uint64_t)aud_off);
    uint32_t i = 0;
    for (; i < n && (int32_t)(due - RING_AT(&aud_ev, i)->at) >= 0; i++)
        aud_apply(RING_AT(&aud_ev, i));
    RING_SKIP(&aud_ev, i);
}

static void aud_next(int32_t *left, int32_t *right)
{
    aud_due();
    aud_mix(left, right);
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
static void aud_overflow(void)
{
    aud_lock();
    int32_t l, r;
    while (!RING_AVAIL(&aud_ev))
        aud_next(&l, &r);
    aud_unlock();
}

static void aud_push_at(uint64_t at, uint8_t kind, uint16_t addr, uint8_t val)
{
    if (!RING_AVAIL(&aud_ev))
        aud_overflow();
    RING_PUT(&aud_ev, ((aud_ev_t){(uint32_t)at, addr, val, kind}));
}

static void aud_push(uint8_t kind, uint16_t addr, uint8_t val)
{
    aud_push_at(bus_now(AUD_NATIVE_RATE), kind, addr, val);
}

void aud_xram_write(uint16_t addr, uint8_t val)
{
    uint8_t kind = 0;
    const uint16_t psg = (uint16_t)(addr - aud_psg_at);
    const uint16_t opl = (uint16_t)(addr - aud_opl_at);
    if (aud_psg_at != 0xFFFF && psg < PSG_REGS_SIZE)
        kind |= AUD_EV_PSG;
    const uint8_t reg = (uint8_t)opl;
    const bool to_opl = aud_opl_at != 0xFFFF && opl < OPL_REGS_SIZE && opl_is_reg(reg);
    if (!kind && !to_opl)
        return;
    const uint64_t at = bus_now(AUD_NATIVE_RATE);
    if (to_opl)
    {
        aud_oq_step(at);
        if (RING_PUT(&aud_oq.ring, reg))
            kind |= AUD_EV_OPL;
        else if (aud_oq.drops < 0x7FFFFFFF)
            aud_oq.drops++;
    }
    if (kind)
        aud_push_at(at, kind, (uint16_t)(psg << 8 | reg), val);
}

void aud_task(void) { bel_task(); }

void aud_bel_put(unsigned i, uint8_t val)
{
    aud_push(AUD_EV_BEL, (uint16_t)i, val);
}

void aud_psg_map(uint16_t word)
{
    aud_psg_at = word;
    aud_push(AUD_EV_PSG_MAP, word, 0);
}

void aud_opl_map(uint16_t word)
{
    const uint64_t at = bus_now(AUD_NATIVE_RATE);
    aud_opl_at = word;
    aud_oq_reset(at);
    aud_push_at(at, AUD_EV_OPL_MAP, word, 0);
}

bool aud_pcm_open(const pcm_fmt_t *fmt)
{
    if (aud_pm.on)
        return false;
    const uint64_t at = bus_now(AUD_NATIVE_RATE);
    aud_pm.on = true;
    aud_pm.fmt = *fmt;
    aud_pm.frame = (uint8_t)(fmt->bits / 8 * fmt->channels);
    aud_pm.step = pcm_step(fmt->rate);
    aud_pm.at = at;
    aud_pm.phase = (uint64_t)1 << 32;
    aud_pm.level = 0;
    aud_pm.underruns = 0;
    aud_push_at(at, AUD_EV_PCM_OPEN, (uint16_t)fmt->rate,
                (uint8_t)((fmt->bits == 16 ? 1 : 0) | (fmt->channels == 2 ? 2 : 0)));
    return true;
}

void aud_pcm_close(void)
{
    aud_pm.on = false;
    aud_push(AUD_EV_PCM_CLOSE, 0, 0);
}

uint32_t aud_pcm_write(const uint8_t *buf, uint32_t n)
{
    const uint64_t at = bus_now(AUD_NATIVE_RATE);
    aud_pm_step(at);
    const uint32_t room = PCM_BUF_SIZE - aud_pm.level;
    if (n > room)
        n = room;
    aud_pm.level = (uint16_t)(aud_pm.level + n);
    for (uint32_t i = 0; i < n; i += 3)
    {
        const uint32_t left = n - i;
        const uint8_t b1 = left > 1 ? buf[i + 1] : 0;
        const uint8_t b2 = left > 2 ? buf[i + 2] : 0;
        aud_push_at(at, (uint8_t)(AUD_EV_PCM_DATA1 + (left > 2 ? 2 : left - 1)),
                    (uint16_t)(buf[i] | b1 << 8), b2);
    }
    return n;
}

uint16_t aud_pcm_level(void)
{
    aud_pm_step(bus_now(AUD_NATIVE_RATE));
    return aud_pm.level;
}

void aud_pcm_clear(void)
{
    aud_pm_step(bus_now(AUD_NATIVE_RATE));
    aud_pm.level = 0;
    aud_push(AUD_EV_PCM_CLEAR, 0, 0);
}

uint32_t aud_pcm_underruns(void)
{
    aud_pm_step(bus_now(AUD_NATIVE_RATE));
    const uint32_t underruns = aud_pm.underruns;
    aud_pm.underruns = 0;
    return underruns;
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

void aud_set_speed(double speed)
{
    atomic_store(&aud_speed, (int32_t)((speed - 1) * 1e9));
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
        {
            aud_skip(to);
            aud_anchored = false;
        }
        aud_unlock();
    }
}

uint64_t aud_samples(void) { return aud_made; }

/* The end of a window of the streaming trim. need is the sink's request. */
static void aud_settle(uint32_t rate, int64_t need)
{
    const double dt = (double)aud_window / rate;
    const int64_t shortfall = aud_short;
    memmove(&aud_lows[1], &aud_lows[0], (AUD_LOWS - 1) * sizeof *aud_lows);
    aud_lows[0] = aud_low;
    aud_low = INT64_MAX;
    aud_short = 0;
    aud_window = 0;
    int64_t low = INT64_MAX;
    for (int i = 0; i < AUD_LOWS; i++)
        if (aud_lows[i] < low)
            low = aud_lows[i];
    if (aud_lows[AUD_LOWS - 1] != INT64_MAX && low > need + AUD_JUMP && !RING_LEN(&opl_queue.ring))
    {
        int64_t by = low;
        if (RING_LEN(&aud_ev))
        {
            /* The queue's credit is back to idle two samples after it
             * empties. */
            const uint32_t due = (uint32_t)(aud_made - (uint64_t)aud_off);
            const int64_t room = (int32_t)(RING_AT(&aud_ev, 0)->at - due) - 2;
            if (room < by)
                by = room;
        }
        if (by > 0)
        {
            aud_off -= by;
            for (int i = 0; i < AUD_LOWS; i++)
                if (aud_lows[i] != INT64_MAX)
                    aud_lows[i] -= by;
            low -= by;
        }
    }
    const int64_t on = shortfall ? -shortfall : low;
    const double e = (double)(on > AUD_JUMP ? AUD_JUMP : on < -AUD_JUMP ? -AUD_JUMP : on);
    const double r = AUD_NATIVE_RATE;
    aud_trim_i += AUD_TRIM_W * AUD_TRIM_W / r * e * dt;
    if (aud_trim_i > AUD_TRIM_MAX)
        aud_trim_i = AUD_TRIM_MAX;
    if (aud_trim_i < -AUD_TRIM_MAX)
        aud_trim_i = -AUD_TRIM_MAX;
    double to = 2 * AUD_TRIM_W / r * e + aud_trim_i;
    if (to > AUD_TRIM_MAX)
        to = AUD_TRIM_MAX;
    if (to < -AUD_TRIM_MAX)
        to = -AUD_TRIM_MAX;
    aud_trim += to - aud_trim > AUD_BEND ? AUD_BEND : to - aud_trim < -AUD_BEND ? -AUD_BEND : to - aud_trim;
}

// The trim starts over: no windows behind it.
static void aud_forget(void)
{
    aud_low = INT64_MAX;
    aud_short = 0;
    for (int i = 0; i < AUD_LOWS; i++)
        aud_lows[i] = INT64_MAX;
    aud_window = 0;
}

/* A streaming render: everything asked for, made now. */
static int aud_stream(float *dst, int samples)
{
    const uint32_t rate = atomic_load(&aud_sink_rate);
    /* The machine's speed is followed no faster than the trim moves, so a
     * machine that leaves its display for the wall clock, as it does while
     * a window is moved, bends the pitch no more than the trim does. The
     * headroom takes what that is behind by. A render that starts over
     * starts at the speed told. */
    const double speed = atomic_load(&aud_speed) / 1e9;
    const double bend = AUD_BEND * samples / (rate / 2.0);
    if (!aud_anchored)
        aud_hint = speed;
    else
        aud_hint += speed - aud_hint > bend ? bend : speed - aud_hint < -bend ? -bend : speed - aud_hint;
    uint64_t step = aud_ratio();
    step += (uint64_t)(int64_t)((double)step * (aud_trim + aud_hint));
    const int64_t need = (int64_t)(((uint64_t)samples * step) >> 32);
    const uint64_t now = atomic_load(&aud_now);
    const int64_t h = (int64_t)now + aud_off - (int64_t)aud_made - need;
    if (!aud_anchored)
    {
        /* A render that starts, or starts over, starts with none. What the
         * machine has stamped past that is left unplayed, the render moving
         * on to it, since moving aud_off back instead would put the changes
         * stamped there closer together than the machine made them. */
        if (h > 0)
            aud_skip(aud_made + (uint64_t)h);
        else
            aud_off -= h;
        aud_anchored = true;
        aud_forget();
        aud_seen = now;
        aud_dry = 0;
    }
    else
    {
        if (now != aud_seen)
        {
            if (aud_dry > AUD_GONE)
                aud_forget();
            aud_seen = now;
            aud_dry = 0;
        }
        if (aud_dry > AUD_GONE)
            aud_forget();
        else
        {
            if (h < 0)
            {
                aud_short -= h;
                for (int i = 0; i < AUD_LOWS; i++)
                    if (aud_lows[i] != INT64_MAX)
                        aud_lows[i] -= h;
                if (aud_low != INT64_MAX)
                    aud_low -= h;
            }
            if ((h < 0 ? 0 : h) < aud_low)
                aud_low = h < 0 ? 0 : h;
        }
        if (h < 0)
            aud_off -= h, aud_dry -= h;
    }
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
    if (aud_dry <= AUD_GONE)
        aud_window += (uint32_t)samples;
    if (aud_window >= rate / 2)
        aud_settle(rate, need);
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

/* Under the lock, because the render writes it on the audio device's
 * thread. */
int aud_viz_take(float *dst)
{
    aud_lock();
    memcpy(dst, aud_viz, sizeof aud_viz);
    const int at = aud_viz_at;
    aud_unlock();
    return at;
}

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

/* The PCM: device's sound, from the render, which was saved with the sound
 * taken. Its history goes oldest first and its bytes from the oldest, so the
 * load puts both back at the start of their rings. An open the render has
 * yet to take is saved as just opened. */
static void aud_pcm_save(sst_cursor_t *c)
{
    pcm_t *p = &aud_pcm;
    const bool play = aud_pm.on && aud_pcm_play;
    sst_put_bool(c, aud_pm.on);
    sst_put_u32(c, aud_pm.on ? aud_pm.fmt.rate : 0);
    sst_put_u8(c, aud_pm.on ? aud_pm.fmt.bits : 0);
    sst_put_u8(c, aud_pm.on ? aud_pm.fmt.channels : 0);
    sst_put_u64(c, play ? p->phase : (uint64_t)1 << 32);
    for (int ch = 0; ch < 2; ch++)
        for (int i = 0; i < RSMP_TAPS; i++)
            sst_put_i32(c, play ? p->hist[ch][p->at + i] : 0);
    const uint16_t level = play ? pcm_level(p) : 0;
    sst_put_u16(c, level);
    for (unsigned i = 0; i < PCM_BUF_SIZE; i++)
        sst_put_u8(c, i < level ? *RING_AT(&p->buf, i) : 0);
}

/* After the STD row, whose reopen has opened the device again in the
 * format it was saved in. */
static bool aud_pcm_load(sst_cursor_t *c, uint64_t made)
{
    pcm_fmt_t fmt;
    const bool on = sst_get_bool(c);
    fmt.rate = sst_get_u32(c);
    fmt.bits = sst_get_u8(c);
    fmt.channels = sst_get_u8(c);
    const uint64_t phase = sst_get_u64(c);
    int32_t hist[2][RSMP_TAPS];
    bool ok = true;
    for (int ch = 0; ch < 2; ch++)
        for (int i = 0; i < RSMP_TAPS; i++)
        {
            hist[ch][i] = sst_get_i32(c);
            if (hist[ch][i] < AUD_SAMPLE_MIN || hist[ch][i] > AUD_SAMPLE_MAX)
                ok = false;
        }
    const uint16_t level = sst_get_u16(c);
    if (!sst_ok(c) || !ok || on != aud_pm.on || level > PCM_BUF_SIZE)
        return false;
    if (on && (fmt.rate != aud_pm.fmt.rate || fmt.bits != aud_pm.fmt.bits ||
               fmt.channels != aud_pm.fmt.channels ||
               phase > ((uint64_t)1 << 32) + aud_pm.step))
        return false;
    uint8_t bytes[PCM_BUF_SIZE];
    for (unsigned i = 0; i < PCM_BUF_SIZE; i++)
        bytes[i] = sst_get_u8(c);
    if (!sst_ok(c))
        return false;
    aud_pcm_play = on;
    if (!on)
        return true;
    pcm_reset(&aud_pcm, &fmt);
    pcm_put(&aud_pcm, bytes, level);
    aud_pcm.phase = phase;
    for (int ch = 0; ch < 2; ch++)
        for (int i = 0; i < RSMP_TAPS; i++)
            aud_pcm.hist[ch][i] = aud_pcm.hist[ch][i + RSMP_TAPS] = hist[ch][i];
    aud_pcm_due = (uint32_t)made;
    aud_pcm_l = aud_pcm_r = 0;
    aud_pm.at = made;
    aud_pm.phase = phase;
    aud_pm.level = level;
    return true;
}

/* What the machine stamped since the sound was taken is for the next
 * sample, so a save takes it before the engines' rows rather than leave it
 * on the ring a load empties. A PCM: write made between frames is such a
 * change. */
void aud_sst_settle(void)
{
    aud_lock();
    aud_due();
    aud_unlock();
}

void aud_sst_save(sst_cursor_t *c, unsigned flags)
{
    aud_lock();
    /* The load starts the PCM: device's model where the render is, so the
     * underruns it saves are the ones up to there. */
    const uint64_t due = aud_made - (uint64_t)aud_off;
    if (due > aud_pm.at)
        aud_pm_step(due);
    sst_put_u8(c, (uint8_t)((psg_xaddr_get() != 0xFFFF ? 1 : 0) |
                            (opl_xaddr_get() != 0xFFFF ? 2 : 0)));
    aud_put_rsmp(c, &aud_rs_l);
    aud_put_rsmp(c, &aud_rs_r);
    sst_put_u64(c, aud_made);
    sst_put_u32(c, aud_oq.drops);
    sst_put_u32(c, aud_pm.underruns);
    bel_sst_save(c, flags);
    aud_pcm_save(c);
    aud_unlock();
}

bool aud_sst_load(sst_cursor_t *c, unsigned flags)
{
    uint8_t engines = sst_get_u8(c);
    rsmp_t l, r;
    aud_get_rsmp(c, &l);
    aud_get_rsmp(c, &r);
    uint64_t made = sst_get_u64(c);
    uint32_t drops = sst_get_u32(c);
    uint32_t underruns = sst_get_u32(c);
    if (!sst_ok(c) || engines > 3 || drops > 0x7FFFFFFF || underruns > 0x7FFFFFFF)
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
    RING_SKIP(&aud_ev, RING_LEN(&aud_ev));
    aud_psg_at = psg_xaddr_get();
    aud_opl_at = opl_xaddr_get();
    /* The machine's queue is the render's, which was saved with the sound
     * taken. */
    aud_oq_reset(made);
    aud_oq.drops = drops;
    aud_pm.underruns = underruns;
    for (uint32_t i = 0; i < RING_LEN(&opl_queue.ring); i++)
        RING_PUT(&aud_oq.ring, (uint8_t)*RING_AT(&opl_queue.ring, i));
    opl_pace_get(&aud_oq.credit, &aud_oq.late);
    aud_probe = NULL;
    aud_rs_l = l;
    aud_rs_r = r;
    aud_made = made;
    atomic_store(&aud_now, made);
    aud_off = 0;
    aud_anchored = false;
    aud_carry_n = 0;
    const bool ok = bel_sst_load(c, flags) && aud_pcm_load(c, made);
    aud_unlock();
    return ok;
}
