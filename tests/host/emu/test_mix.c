/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/api/xreg.h"
#include "core/aud/mix.h"
#include "host/sokol/app/pace.h"
#include "core/aud/opl.h"
#include "core/dap/dbg.h"
#include "core/wdc/bus.h"
#include "core/wdc/cpu.h"
#include "core/wdc/phi2.h"
#include "chips/chips/w65c02.h"
#include "emu_boot.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------------- OPL2 writes in the machine's time ---------------- */

/* Each write the ROM made to RW0: the cycle count just after it, the
 * scanline its batch ran to and the sample it was stamped for. Then each
 * sample that applied a write. */
#define MIX_WRITES 1024
static uint64_t mix_cycle[MIX_WRITES], mix_line[MIX_WRITES], mix_at[MIX_WRITES];
static uint64_t mix_applied_at[MIX_WRITES];
static int mix_writes, mix_applied;

/* The drop count when the run ended, before the stop clears it. */
static uint32_t mix_drops;

/* The stamp a write in the cycle about to run gets. */
static uint64_t mix_next;

/* The OPL2 as aud_mix steps it, noting each sample that applies a write. */
static void mix_probe(int16_t *l, int16_t *r)
{
    if (opl_xaddr_get() == 0xFFFF)
        return;
    const uint16_t waiting = RING_LEN(&opl_queue.ring);
    *l = *r = opl_sample();
    if (RING_LEN(&opl_queue.ring) < waiting)
        mix_applied_at[mix_applied++] = aud_samples();
}

static void mix_cycle_cb(uint64_t pins)
{
    if (!(pins & W65C02_RW) && W65C02_GET_ADDR(pins) == 0xFFE4 && mix_writes < MIX_WRITES)
    {
        mix_cycle[mix_writes] = bus_cycles();
        mix_line[mix_writes] = vga_beam_lines();
        mix_at[mix_writes++] = mix_next;
    }
    mix_next = bus_now(AUD_NATIVE_RATE);
}

/* The ROM run for some frames with every write watched, taking a frame of
 * sound after each frame as the script host does. */
static bool mix_run(const char *rom, int frames, bool muted)
{
    static float out[800 * 2];
    aud_set_enabled(!muted);
    aud_setup_probe(mix_probe);
    xreg0(1, 1, 0xFFFF);
    mix_writes = mix_applied = 0;
    dbg_set_active(true);
    cpu_dbg_cycle_cb = mix_cycle_cb;
    const bool booted = emu_restart(rom);
    for (int i = 0; i < frames; i++)
    {
        emu_frames(1);
        aud_render(out, 800);
    }
    cpu_dbg_cycle_cb = NULL;
    dbg_set_active(false);
    mix_drops = aud_opl_overruns();
    sys_stop();
    sys_commit();
    aud_setup_probe(NULL);
    aud_set_enabled(true);
    return booted;
}

/* A scanline is 2/63000 s and a sample 1/49716 s, so line n starts in sample
 * n * 1381 / 875. A batch runs the cycles that start inside the line before
 * the one it ran to. */
static bool mix_in_line(int i)
{
    const uint64_t first = (mix_line[i] - 1) * 1381 / 875;
    const uint64_t last = mix_line[i] * 1381 / 875;
    return mix_at[i] >= first && mix_at[i] <= last;
}

/* Some start phase puts every write in the sample its cycle falls in at
 * 49716 samples a second against the 6502's clock. In samples times the
 * clock: at * hz <= phase + cycles * 49716 < (at + 1) * hz. */
static bool mix_on_the_clock(void)
{
    const int64_t hz = (int64_t)phi2_get_khz_run() * 1000;
    int64_t lo = INT64_MIN, hi = INT64_MAX;
    for (int i = 0; i < mix_writes; i++)
    {
        const int64_t d = (int64_t)(mix_cycle[i] - mix_cycle[0]) * 49716;
        const int64_t a = (int64_t)(mix_at[i] - mix_at[0]) * hz - d;
        if (a > lo)
            lo = a;
        if (a + hz < hi)
            hi = a + hz;
    }
    return lo < hi;
}

/* The Pico's rule from the samples the writes were stamped for: up to
 * 256 wait in order, and before each sample the oldest applies when the
 * credit is at least 96, which costs 96; then the credit gains 72, up to 192
 * while writes wait and 96 while none do. Register 40 never applies a late
 * half. Returns the drops; at[] gets the samples that applied one. */
static uint32_t mix_pico(uint64_t *at, int *applied)
{
    int waiting = 0, credit = 96, i = 0;
    uint32_t drops = 0;
    *applied = 0;
    for (uint64_t n = mix_at[0]; n < aud_samples(); n++)
    {
        for (; i < mix_writes && mix_at[i] == n; i++)
        {
            if (waiting == OPL_QUEUE_SIZE)
                drops++;
            else
                waiting++;
        }
        if (waiting && credit >= 96)
        {
            at[(*applied)++] = n;
            waiting--;
            credit -= 96;
        }
        credit += 72;
        if (credit > (waiting ? 192 : 96))
            credit = waiting ? 192 : 96;
    }
    return drops;
}

static bool mix_is_pico(void)
{
    static uint64_t at[MIX_WRITES];
    int applied;
    const uint32_t drops = mix_pico(at, &applied);
    return drops == mix_drops && applied == mix_applied &&
           !memcmp(at, mix_applied_at, (size_t)applied * sizeof *at);
}

static void mix_spread(int *utest_result, bool muted)
{
    ASSERT_TRUE(mix_run(AUD_ROM_OPL_SPREAD, 4, muted));
    ASSERT_EQ(mix_writes, 300);
    ASSERT_GT(mix_cycle[299] - mix_cycle[0], (uint64_t)120000);
    for (int i = 0; i < mix_writes; i++)
        ASSERT_TRUE(mix_in_line(i));
    ASSERT_TRUE(mix_on_the_clock());
    ASSERT_EQ(mix_drops, 0u);
    ASSERT_EQ(mix_applied, 300);
    for (int i = 0; i < mix_writes; i++)
        ASSERT_EQ(mix_applied_at[i], mix_at[i]);
    ASSERT_TRUE(mix_is_pico());
}

static void mix_flat(int *utest_result, bool muted)
{
    ASSERT_TRUE(mix_run(AUD_ROM_OPL_FLAT, 3, muted));
    ASSERT_EQ(mix_writes, 768);
    for (int i = 0; i < mix_writes; i++)
        ASSERT_TRUE(mix_in_line(i));
    ASSERT_TRUE(mix_on_the_clock());
    ASSERT_GT(mix_drops, 0u);
    ASSERT_EQ(RING_LEN(&opl_queue.ring), 0);
    ASSERT_TRUE(mix_is_pico());
}

UTEST(mix, writes_spread_over_a_frame_apply_where_the_pico_applies_them)
{
    mix_spread(utest_result, false);
}

UTEST(mix, a_flat_out_burst_drops_what_the_pico_drops)
{
    mix_flat(utest_result, false);
}

UTEST(mix, muted_writes_spread_over_a_frame_still_apply_in_time)
{
    mix_spread(utest_result, true);
}

UTEST(mix, a_muted_flat_out_burst_still_drops_in_time)
{
    mix_flat(utest_result, true);
}

/* ---------------- a streaming sink ---------------- */

/* Native samples in 512 frames at 48 kHz. */
#define MIX_NEED (512.0 * AUD_NATIVE_RATE / 48000)

/* Renders in half a second, the trim's window. */
#define MIX_WINDOW 47

typedef struct
{
    int64_t spread;
    double lo, hi;
} mix_stream_t;

/* The render index each write to register 40 applied at less the stamp it
 * had, which is aud_off, and its least and most once settled. */
static int64_t mix_gap, mix_gap_lo, mix_gap_hi;
static bool mix_gap_seen;

/* Each write's value picks its stamp out of these. */
static uint64_t mix_stamp[256];

static void mix_gap_probe(int16_t *l, int16_t *r)
{
    const uint16_t waiting = RING_LEN(&opl_queue.ring);
    const uint8_t v = (uint8_t)(*RING_AT(&opl_queue.ring, 0) >> 8);
    *l = *r = opl_sample();
    if (RING_LEN(&opl_queue.ring) < waiting)
    {
        const int64_t gap = (int64_t)aud_samples() - (int64_t)mix_stamp[v];
        if (gap < mix_gap_lo)
            mix_gap_lo = gap;
        if (gap > mix_gap_hi)
            mix_gap_hi = gap;
        mix_gap = gap, mix_gap_seen = true;
    }
}

/* An idle machine paced at exactly 60 frames a second writing register 40 at
 * the end of every frame, and a device pulling 512 frames at a time at
 * 48 kHz on a crystal off by `drift`. A render's headroom is what the machine
 * had stamped past the end of what it asked for, in native samples. After
 * the first `settle` seconds: how far the gap from stamp to render wandered,
 * and the least and most of each half second's least headroom. */
static mix_stream_t mix_stream(double drift, int minutes, double settle)
{
    static float out[512 * 2];
    mix_stream_t s = {0, 1e9, -1e9};
    sys_stop();
    sys_commit();
    vga_set_scanout(false);
    aud_set_sink_rate(48000);
    aud_set_streaming(true);
    xreg0(1, 1, 0x4000);
    aud_setup_probe(mix_gap_probe);
    mix_gap_seen = false;
    const double frame = 1.0 / 60, pull = 512 / (48000 * (1 + drift));
    const uint64_t frames = (uint64_t)minutes * 3600;
    uint64_t f = 0, p = 0;
    uint8_t v = 0;
    bool settled = false;
    double low = 1e9;
    int renders = 0;
    while (f < frames)
    {
        if (f * frame <= p * pull)
        {
            emu_frames(1);
            mix_stamp[v] = bus_now(AUD_NATIVE_RATE);
            aud_xram_write(0x4040, v++);
            f++;
            continue;
        }
        const double t = p++ * pull;
        const double lead = (double)(int64_t)(bus_now(AUD_NATIVE_RATE) - aud_samples());
        aud_render(out, 512);
        if (!mix_gap_seen)
            continue;
        const double h = lead + (double)mix_gap - MIX_NEED;
        if (h < low)
            low = h;
        if (++renders < MIX_WINDOW)
            continue;
        if (settled)
        {
            if (low < s.lo)
                s.lo = low;
            if (low > s.hi)
                s.hi = low;
        }
        else if (t >= settle)
            mix_gap_lo = INT64_MAX, mix_gap_hi = INT64_MIN, settled = true;
        low = 1e9, renders = 0;
    }
    s.spread = mix_gap_hi - mix_gap_lo;
    aud_setup_probe(NULL);
    aud_set_streaming(false);
    vga_set_scanout(true);
    printf("drift %+.2f%%: gap wandered %lld, least headroom %.0f to %.0f\n",
           drift * 100, (long long)s.spread, s.lo, s.hi);
    return s;
}

/* No write lands after its sample: a render short of headroom moves the gap
 * on first. With the device on time the gap wanders less than two
 * milliseconds once settled, and the least headroom of each half second
 * stays between a millisecond below none and two above, so writes wait no
 * longer than that past what they have to.
 *
 * A device a quarter of a percent off is twenty times what a crystal is
 * off by, and the loop, which is slow so that the pitch holds still, takes
 * minutes to come round to it. Until then a fast device is short a few
 * samples at every render and plays on, which moves the gap and nothing
 * else, and a slow one has the render hold more. */
static void mix_steady(int *utest_result, double drift)
{
    const mix_stream_t s = mix_stream(drift, 10, drift ? 240 : 20);
    ASSERT_GE(s.lo, -(double)AUD_NATIVE_RATE / 1000);
    if (drift <= 0)
        ASSERT_LT(s.spread, (int64_t)AUD_NATIVE_RATE / 500);
    ASSERT_LE(s.hi, (double)AUD_NATIVE_RATE / 500);
}

UTEST(mix, a_device_a_quarter_percent_fast_streams_on_time)
{
    mix_steady(utest_result, 0.0025);
}

UTEST(mix, a_device_a_quarter_percent_slow_streams_on_time)
{
    mix_steady(utest_result, -0.0025);
}

UTEST(mix, a_device_on_time_streams_on_time)
{
    mix_steady(utest_result, 0);
}

/* ---------------- a streaming sink and the app's pace ---------------- */

typedef struct
{
    int buf;         // frames the device asks for at a time, at 48 kHz
    double display;  // seconds between the display's callbacks
    double jitter;   // each callback as much as this late, in seconds
    double stall_at; // when the callbacks stop, in seconds
    double stall;    // and for how long
    bool both;       // the device stops asking through the stall too
    double deaf;     // the device alone stops asking for this long, at stall_at
    double from, to; // the callbacks come every `other` between these
    double other;
    int busy;        // a program making this many OPL2 writes a frame, spread over it
    double drift;    // the device's crystal off the machine's by this much
    double secs;     // how long it runs; three minutes if 0
} mix_host_t;

typedef struct
{
    double skipped_ms; // sound the machine's thread or a render made and threw away
    double jumped_ms;  // machine time the render passed over without making it
    double late_ms;    // the longest a write waited to sound, in machine time
    uint32_t dropped;  // OPL2 writes the render's queue had no room for
    uint32_t overran;  // and those the machine's count of it had none for
    double bend;       // the most the pitch moved from one half second to the next
    double off_pitch;  // and the furthest it was from true
} mix_hosted_t;

/* What the queue takes at each sample: how far the render's samples are past
 * the machine's for it, which is aud_off, and how long ago the machine made
 * the write. */
static int64_t mix_off, mix_jumped, mix_late;
static bool mix_off_seen;
static void mix_host_probe(int16_t *l, int16_t *r)
{
    const uint16_t waiting = RING_LEN(&opl_queue.ring);
    const uint8_t v = (uint8_t)(*RING_AT(&opl_queue.ring, 0) >> 8);
    *l = *r = opl_sample();
    if (RING_LEN(&opl_queue.ring) < waiting)
    {
        const int64_t off = (int64_t)(aud_samples() - mix_stamp[v]);
        const int64_t late = (int64_t)(bus_now(AUD_NATIVE_RATE) - mix_stamp[v]);
        if (mix_off_seen && off < mix_off)
            mix_jumped += mix_off - off;
        mix_off = off, mix_off_seen = true;
        if (late > mix_late)
            mix_late = late;
    }
}

/* A busy program: a write to register 40 every so many cycles. */
static uint64_t mix_busy_every, mix_busy_next;
static uint8_t mix_busy_v;
static void mix_busy_cb(uint64_t pins)
{
    (void)pins;
    if (bus_cycles() < mix_busy_next)
        return;
    mix_busy_next += mix_busy_every;
    aud_xram_write(0xF040, mix_busy_v++);
}

static void mix_busy_probe(int16_t *l, int16_t *r)
{
    *l = *r = opl_sample();
}

/* The window app as its two threads run it, for three minutes: the display's
 * callbacks run frames by the app's pace (host/sokol/app/pace.h), the
 * device asks on its own clock, and register 40 is written once at the end
 * of each frame, or by a busy program many times through it. Counted from
 * the twentieth second, or the fifth of a short run, past where the render
 * first finds its place. */
static mix_hosted_t mix_hosted(const mix_host_t *o)
{
    static float out[4096 * 2];
    mix_hosted_t s = {0, 0, 0, 0, 0, 0, 0};
    sys_stop();
    sys_commit();
    vga_set_scanout(false);
    aud_set_sink_rate(48000);
    aud_set_streaming(true);
    if (o->busy)
    {
        /* The ROM maps the OPL2 at F000, makes its own writes in its first
         * frame and then spins, which gives the cycles to write from. */
        emu_restart(AUD_ROM_OPL_SPREAD);
        emu_frames(4);
        aud_setup_probe(mix_busy_probe);
        mix_busy_every = 8000000 / VGA_HZ / (unsigned)o->busy;
        mix_busy_next = bus_cycles();
        dbg_set_active(true);
        cpu_dbg_cycle_cb = mix_busy_cb;
    }
    else
    {
        xreg0(1, 1, 0x4000);
        aud_setup_probe(mix_host_probe);
    }
    mix_off_seen = false;
    const double secs = o->secs ? o->secs : 180, pull = o->buf / (48000.0 * (1 + o->drift));
    const double from = secs < 60 ? 5 : 20;
    const int64_t need = (int64_t)((double)o->buf * AUD_NATIVE_RATE / 48000 * 1.006) + 8;
    pace_t pace = {0};
    double t = 0, entered = -1, grid = 0, hold = -1;
    uint64_t p = 0;
    uint8_t v = 0;
    bool stalled = false, counting = false;
    unsigned seed = 1;
    int64_t skipped = 0, span_made = 0, span_asked = 0;
    double pitch_was = 0;
    bool pitch_seen = false;
    while (t < secs)
    {
        const double at = p * pull;
        if (!counting && at >= from && t >= from)
        {
            counting = true;
            mix_jumped = mix_late = skipped = 0;
            (void)opl_take_drops();
            (void)aud_opl_overruns();
        }
        if (at < t)
        {
            p++;
            if (o->both && at < hold)
                continue;
            if (o->deaf > 0 && at >= o->stall_at && at < o->stall_at + o->deaf)
                continue;
            const uint64_t before = aud_samples();
            aud_render(out, o->buf);
            const int64_t made = (int64_t)(aud_samples() - before);
            if (made > need)
                skipped += made - need;
            else if (counting)
            {
                /* The pitch is the machine's samples played for the
                 * device's, taken over half a second. */
                span_made += made, span_asked += o->buf;
                if (span_asked >= 24000)
                {
                    const double pitch = (double)span_made * 48000 / ((double)span_asked * AUD_NATIVE_RATE) - 1;
                    if (pitch_seen && fabs(pitch - pitch_was) > s.bend)
                        s.bend = fabs(pitch - pitch_was);
                    if (fabs(pitch) > s.off_pitch)
                        s.off_pitch = fabs(pitch);
                    pitch_was = pitch, pitch_seen = true;
                    span_made = span_asked = 0;
                }
            }
            continue;
        }
        pace_add(&pace, entered < 0 ? VGA_FRAME_NS : (uint64_t)((t - entered) * 1e9), true);
        aud_set_speed(pace_speed(&pace));
        entered = t;
        while (pace_due(&pace))
        {
            const uint64_t before = aud_samples();
            emu_frames(1);
            skipped += (int64_t)(aud_samples() - before);
            if (!o->busy)
            {
                mix_stamp[v] = bus_now(AUD_NATIVE_RATE);
                aud_xram_write(0x4040, v++);
            }
            pace_ran(&pace);
        }
        grid += grid >= o->from && grid < o->to ? o->other : o->display;
        if (!stalled && o->stall > 0 && grid >= o->stall_at)
            grid += o->stall, stalled = true, hold = grid;
        seed = seed * 1103515245u + 12345u;
        t = grid + o->jitter * (double)(seed >> 8) / (double)(1u << 24);
    }
    s.dropped = opl_take_drops();
    s.overran = aud_opl_overruns();
    s.skipped_ms = (double)skipped * 1000 / AUD_NATIVE_RATE;
    s.jumped_ms = (double)mix_jumped * 1000 / AUD_NATIVE_RATE;
    s.late_ms = (double)mix_late * 1000 / AUD_NATIVE_RATE;
    cpu_dbg_cycle_cb = NULL;
    dbg_set_active(false);
    sys_stop();
    sys_commit();
    aud_setup_probe(NULL);
    aud_set_speed(1);
    aud_set_streaming(false);
    vga_set_scanout(true);
    printf("buf %d, %.2f Hz, %.1f ms late, stall %.0f ms%s, deaf %.0f ms, busy %d: skipped %.1f ms, "
           "jumped %.1f ms, late %.1f ms, writes dropped %u, overran %u, pitch off %.3f%% bent %.3f%%\n",
           o->buf, 1 / o->display, o->jitter * 1000, o->stall * 1000, o->both ? " of both" : "",
           o->deaf * 1000, o->busy, s.skipped_ms, s.jumped_ms, s.late_ms, (unsigned)s.dropped,
           (unsigned)s.overran, s.off_pitch * 100, s.bend * 100);
    return s;
}

/* A busy program is run a cycle at a time, which is slow, so it has one
 * size and what it is for happens early. */
static const int mix_sizes[] = {480, 512, 1024};
#define MIX_EACH_SIZE(o) \
    for (unsigned i = 0; i < ((o).busy ? 1 : sizeof mix_sizes / sizeof *mix_sizes) && ((o).buf = mix_sizes[i]); i++)

/* What a host may cost the sound over three minutes: sound made and thrown
 * away, machine time the render passed over, and how long a write waits to
 * sound, counted in the frames the machine ran meanwhile. The render's queue
 * always drops the writes the machine counted as dropped and no others. */
static void mix_host_costs(int *utest_result, mix_host_t o, double skipped_ms, double jumped_ms,
                           double late_ms)
{
    MIX_EACH_SIZE(o)
    {
        const mix_hosted_t s = mix_hosted(&o);
        ASSERT_LE(s.skipped_ms, skipped_ms);
        ASSERT_LE(s.jumped_ms, jumped_ms);
        ASSERT_LE(s.late_ms, late_ms);
        ASSERT_EQ(s.overran, s.dropped);
        /* The pitch moves less than a twentieth of a percent, under a cent,
         * from one half second to the next, twice that where frames come as
         * much as 10 ms late, and is not looked at where the render had to
         * start over. */
        if (!o.busy && !o.deaf && !o.stall && !o.other)
            ASSERT_LE(s.bend, o.jitter > 0.005 ? 0.001 : 0.0005);
    }
}

/* A display that is the machine's frame rate costs nothing: a write sounds
 * within two frames, and the render passes over no more than the few
 * milliseconds the trim settles by. */
UTEST(mix, a_60_hz_display_keeps_every_sample)
{
    mix_host_costs(utest_result, (mix_host_t){.display = 1.0 / 60}, 0, 10, 34);
}

UTEST(mix, an_ntsc_display_keeps_every_sample)
{
    mix_host_costs(utest_result, (mix_host_t){.display = 1.001 / 60}, 0, 10, 34);
}

UTEST(mix, a_1366x768_panel_keeps_every_sample)
{
    mix_host_costs(utest_result, (mix_host_t){.display = 1 / 59.79}, 0, 10, 34);
}

UTEST(mix, a_120_hz_display_keeps_every_sample)
{
    mix_host_costs(utest_result, (mix_host_t){.display = 1.0 / 120}, 0, 10, 34);
}

/* A display the machine does not follow brings its frames a little earlier
 * each time and then one a whole frame late, every three seconds at
 * 800x600's 60.32 Hz and every ten at 60.10. The render holds what the late
 * one needs and plays evenly through it. */
UTEST(mix, an_800x600_display_keeps_every_sample)
{
    mix_host_costs(utest_result, (mix_host_t){.display = 1 / 60.32, .jitter = 0.0003}, 0, 20, 51);
}

UTEST(mix, a_60_10_hz_display_keeps_every_sample)
{
    mix_host_costs(utest_result, (mix_host_t){.display = 1 / 60.10, .jitter = 0.0003}, 0, 20, 51);
}

UTEST(mix, a_59_6_hz_display_keeps_every_sample)
{
    mix_host_costs(utest_result, (mix_host_t){.display = 1 / 59.6, .jitter = 0.0003}, 0, 20, 51);
}

/* The two crystals a machine on the wall clock and its device run from are
 * off by hundredths of a percent, which the trim takes up. */
UTEST(mix, an_800x600_display_and_a_fast_device_keep_every_sample)
{
    mix_host_costs(utest_result,
                   (mix_host_t){.display = 1 / 60.32, .jitter = 0.0003, .drift = 0.0003}, 0, 30, 67);
}

UTEST(mix, an_800x600_display_and_a_slow_device_keep_every_sample)
{
    mix_host_costs(utest_result,
                   (mix_host_t){.display = 1 / 60.32, .jitter = 0.0003, .drift = -0.0003}, 0, 30, 67);
}

/* WASAPI asks every 10 ms, which a 50 Hz display's 20 ms is a whole number
 * of: the least headroom does not climb and fall there but walks with the
 * two crystals, a request's worth and round again, which is the trim's to
 * take up and nothing to pass over. */
UTEST(mix, a_50_hz_display_and_a_slow_device_keep_every_sample)
{
    mix_host_costs(utest_result,
                   (mix_host_t){.display = 1.0 / 50, .jitter = 0.0003, .drift = -0.0002}, 0, 20, 67);
}

UTEST(mix, a_144_hz_display_keeps_every_sample)
{
    mix_host_costs(utest_result, (mix_host_t){.display = 1.0 / 144, .jitter = 0.0003}, 0, 20, 51);
}

/* Callbacks that land unevenly have the render hold more, in steps it gives
 * back when they even out. */
UTEST(mix, callbacks_4_ms_uneven_keep_every_sample)
{
    mix_host_costs(utest_result, (mix_host_t){.display = 1.0 / 60, .jitter = 0.004}, 0, 40, 51);
}

UTEST(mix, callbacks_10_ms_uneven_keep_every_sample)
{
    /* Callbacks this uneven also leave the display's rate known only to a
     * tenth of a percent, which the render holds more to cover. */
    mix_host_costs(utest_result, (mix_host_t){.display = 1.0 / 60, .jitter = 0.010}, 0, 40, 84);
}

/* A host that stops the machine and not the device: the sound runs on over
 * the stall, and the frames the machine makes up after it are passed over.
 * The trim that took a stall for lateness to cover held more and more and
 * threw 68 ms away every 12 seconds from then on. */
UTEST(mix, a_machine_stalled_for_300_ms_costs_the_stall)
{
    mix_host_costs(utest_result, (mix_host_t){.display = 1.0 / 60, .stall_at = 30, .stall = 0.3},
                   0, 50, 67);
}

UTEST(mix, a_machine_stalled_for_2_s_costs_the_stall)
{
    mix_host_costs(utest_result,
                   (mix_host_t){.display = 1.0 / 60, .jitter = 0.002, .stall_at = 30, .stall = 2},
                   0, 50, 67);
}

/* A host that stops both, as a browser does its page. */
UTEST(mix, a_host_stalled_for_500_ms_costs_the_stall)
{
    mix_host_costs(utest_result,
                   (mix_host_t){.display = 1.0 / 60, .stall_at = 30, .stall = 0.5, .both = true},
                   0, 60, 84);
}

/* Windows runs the callbacks from a 64 Hz timer while a window is moved. */
UTEST(mix, a_window_moved_for_a_minute_keeps_every_sample)
{
    mix_host_costs(utest_result,
                   (mix_host_t){.display = 1.0 / 60, .jitter = 0.003, .from = 30, .to = 90, .other = 0.015625},
                   0, 40, 51);
}

/* A 1366x768 panel's 59.79 Hz is the machine's rate until a window is moved
 * and after it, and the wall clock between: the machine's speed changes by
 * a third of a percent each way, and the pitch follows it at the rate the
 * trim moves, the two together a twentieth of a percent a half second. */
UTEST(mix, a_window_moved_on_a_1366x768_panel_bends_no_pitch)
{
    mix_host_t o = {.display = 1 / 59.79, .jitter = 0.001, .from = 30, .to = 40, .other = 0.015625};
    MIX_EACH_SIZE(o)
    {
        const mix_hosted_t s = mix_hosted(&o);
        ASSERT_EQ(0, s.skipped_ms);
        ASSERT_LE(s.jumped_ms, 20);
        ASSERT_LE(s.bend, 0.0006);
        ASSERT_LE(s.late_ms, 67);
    }
}

/* A device that stops asking has its sound made and thrown away by the
 * machine's thread, and no more than it missed. The render may be as much
 * as AUD_STALL behind when it does, eight frames and the one being made. */
UTEST(mix, a_device_deaf_for_500_ms_costs_what_it_missed)
{
    mix_host_costs(utest_result, (mix_host_t){.display = 1.0 / 60, .stall_at = 30, .deaf = 0.5},
                   560, 40, 151);
}

/* A program writing the OPL2 150 times a frame loses none of its writes to
 * the host: not when the render starts over after the device was deaf,
 * where moving aud_off back over the frames made meanwhile would land them
 * all on one sample, and not when the host stalls or runs unevenly. */
UTEST(mix, a_busy_program_keeps_its_writes_when_the_device_goes_deaf)
{
    mix_host_costs(utest_result,
                   (mix_host_t){.display = 1.0 / 60, .stall_at = 8, .deaf = 0.5, .busy = 150, .secs = 15},
                   560, 0, 0);
}

UTEST(mix, a_busy_program_keeps_its_writes_when_the_host_stalls)
{
    mix_host_costs(utest_result,
                   (mix_host_t){.display = 1.0 / 60, .stall_at = 8, .stall = 0.5, .both = true, .busy = 150, .secs = 15},
                   0, 0, 0);
}

UTEST(mix, a_busy_program_keeps_its_writes_when_callbacks_are_uneven)
{
    mix_host_costs(utest_result, (mix_host_t){.display = 1.0 / 60, .jitter = 0.004, .busy = 150, .secs = 15},
                   0, 0, 0);
}

UTEST_MAIN_EMU();