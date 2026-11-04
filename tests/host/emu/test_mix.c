/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/aud/mix.h"
#include "core/aud/opl.h"
#include "core/dap/dbg.h"
#include "core/wdc/bus.h"
#include "core/wdc/cpu.h"
#include "core/wdc/phi2.h"
#include "emu_boot.h"
#include <stdio.h>
#include <string.h>

/* ---------------- OPL2 writes in the machine's time ---------------- */

/* Each write the engine took: the cycle count just after it, the scanline its
 * batch ran to and the sample it was handed over before. Then each sample
 * that applied a write. */
#define MIX_WRITES 1024
static uint64_t mix_cycle[MIX_WRITES], mix_line[MIX_WRITES], mix_at[MIX_WRITES];
static uint64_t mix_applied_at[MIX_WRITES];
static int mix_writes, mix_applied;

/* The OPL2 as mix() steps it, noting each sample that applies a write. */
static void mix_probe(int16_t *l, int16_t *r)
{
    if (opl_xaddr_get() == 0xFFFF)
        return;
    const uint16_t waiting = opl_queue.count;
    *l = *r = opl_sample();
    if (opl_queue.count < waiting)
        mix_applied_at[mix_applied++] = aud_samples();
}

/* Writes queued, applied and dropped only ever grow by the writes taken. */
static void mix_cycle_cb(uint64_t pins)
{
    (void)pins;
    const int taken = opl_queue.count + (int)opl_queue.drops + mix_applied;
    for (; mix_writes < taken; mix_writes++)
    {
        mix_cycle[mix_writes] = bus_cycles();
        mix_line[mix_writes] = vga_beam_lines();
        mix_at[mix_writes] = aud_samples();
    }
}

/* The ROM run for some frames with every write watched, taking a frame of
 * sound after each frame as the script host does. */
static bool mix_run(const char *rom, int frames, bool muted)
{
    static float out[800 * 2];
    aud_set_enabled(!muted);
    aud_setup_probe(mix_probe);
    opl_xreg(0xFFFF);
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

/* The Pico's rule from the samples the writes were handed over before: up to
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
    return drops == opl_queue.drops && applied == mix_applied &&
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
    ASSERT_EQ(opl_queue.drops, 0u);
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
    ASSERT_GT(opl_queue.drops, 0u);
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

/* The level a sink pulling 512 frames at 48 kHz is held at: its buffer and a
 * frame. */
#define MIX_TARGET (512 + 800)

typedef struct
{
    long underruns;
    int lo, hi;
    double first_minute, last_minute;
} mix_stream_t;

/* An idle machine paced at exactly 60 frames a second, and a device pulling
 * 512 frames at a time at 48 kHz on a crystal off by `drift`. After the first
 * minute: callbacks short of 512, the buffer's least and most before a
 * callback, and its mean over the second minute and the last. */
static mix_stream_t mix_stream(double drift, int minutes)
{
    static float out[512 * 2];
    mix_stream_t s = {0, MIX_TARGET * 4, 0, 0, 0};
    sys_stop();
    sys_commit();
    aud_setup_probe(NULL);
    vga_set_scanout(false);
    aud_set_sink_rate(48000);
    aud_set_sink_buffer(512);
    const double frame = 1.0 / 60, pull = 512 / (48000 * (1 + drift));
    const uint64_t frames = (uint64_t)minutes * 3600;
    double sum[2] = {0, 0};
    long count[2] = {0, 0};
    uint64_t f = 0, p = 0;
    while (f < frames)
    {
        if (f * frame <= p * pull)
        {
            emu_frames(1);
            f++;
            continue;
        }
        const double t = p++ * pull;
        const int have = aud_ready();
        const int n = aud_render(out, 512);
        if (t < 60)
            continue;
        if (n < 512)
            s.underruns++;
        if (have < s.lo)
            s.lo = have;
        if (have > s.hi)
            s.hi = have;
        const int minute = (int)(t / 60);
        if (minute == 1 || minute == minutes - 1)
            sum[minute != 1] += have, count[minute != 1]++;
    }
    s.first_minute = sum[0] / count[0];
    s.last_minute = sum[1] / count[1];
    aud_set_sink_buffer(0);
    vga_set_scanout(true);
    printf("drift %+.1f%%: %ld underruns, buffer %d to %d, mean %.0f then %.0f\n",
           drift * 100, s.underruns, s.lo, s.hi, s.first_minute, s.last_minute);
    return s;
}

/* The buffer never runs dry or past twice its target once settled, and its
 * mean holds still from the second minute to the last. */
static void mix_steady(int *utest_result, double drift)
{
    const mix_stream_t s = mix_stream(drift, 10);
    ASSERT_EQ(s.underruns, 0);
    ASSERT_GE(s.lo, 512);
    ASSERT_LE(s.hi, 2 * MIX_TARGET);
    ASSERT_LT(s.last_minute - s.first_minute, 16.0);
    ASSERT_GT(s.last_minute - s.first_minute, -16.0);
}

UTEST(mix, a_device_half_a_percent_fast_keeps_a_steady_buffer)
{
    mix_steady(utest_result, 0.005);
}

UTEST(mix, a_device_half_a_percent_slow_keeps_a_steady_buffer)
{
    mix_steady(utest_result, -0.005);
}

UTEST(mix, a_device_on_time_keeps_a_steady_buffer)
{
    mix_steady(utest_result, 0);
}

UTEST_MAIN_EMU();
