/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/api/xreg.h"
#include "core/aud/mix.h"
#include "core/aud/opl.h"
#include "core/dap/dbg.h"
#include "core/wdc/bus.h"
#include "core/wdc/cpu.h"
#include "core/wdc/phi2.h"
#include "chips/chips/w65c02.h"
#include "emu_boot.h"
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
    const uint16_t waiting = opl_queue.count;
    *l = *r = opl_sample();
    if (opl_queue.count < waiting)
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
    ASSERT_EQ(opl_queue.count, 0);
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
    const uint16_t waiting = opl_queue.count;
    const uint8_t v = (uint8_t)(opl_queue.entry[opl_queue.head] >> 8);
    *l = *r = opl_sample();
    if (opl_queue.count < waiting)
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
 * the first 20 seconds: how far the gap from stamp to render wandered, and
 * the least and most of each half second's least headroom. */
static mix_stream_t mix_stream(double drift, int minutes)
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
        else if (t >= 20)
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
 * on first. Once settled the gap wanders less than two milliseconds, and the
 * least headroom of each half second stays between a millisecond below none
 * and two above, so writes wait no longer than that past what they have
 * to. */
static void mix_steady(int *utest_result, double drift)
{
    const mix_stream_t s = mix_stream(drift, 10);
    ASSERT_LT(s.spread, (int64_t)AUD_NATIVE_RATE / 500);
    ASSERT_GE(s.lo, -(double)AUD_NATIVE_RATE / 1000);
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

UTEST_MAIN_EMU();
