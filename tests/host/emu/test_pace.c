/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The window app's pace (host/sokol/app/pace.h) under displays of every
 * rate, with callbacks that land late and callbacks that go missing. */

#include "host/sokol/app/pace.h"
#include "utest.h"
#include <stdio.h>

typedef struct
{
    double hz;      // the display
    double late_ms; // each callback as much as this late
    int spike;      // one callback in this many is 3.5 ms later still; 0 for none
    int miss;       // one callback in this many never comes; 0 for none
    double from_s;  // the callbacks come at other_hz between these
    double to_s;
    double other_hz;
} pace_case_t;

typedef struct
{
    double speed; // the machine's, against the wall: 1 is on time
    int none;     // callbacks that ran no frame
    int twos;     // callbacks that ran two or more
    int flips;    // of those, ones next to one of the other kind
    int changes;  // times the display went from followed to not, or back
} pace_got_t;

/* Ten minutes of a display's callbacks, counted from the tenth second. */
static pace_got_t pace_run(const pace_case_t *c)
{
    pace_t p = {0};
    pace_got_t g = {0};
    unsigned seed = 7;
    double grid = 0, before = -1, t0 = 0;
    long frames = 0, k = 0;
    int last = 1;
    bool followed = false;
    while (grid < 600)
    {
        const double period = grid >= c->from_s && grid < c->to_s ? 1 / c->other_hz : 1 / c->hz;
        grid += period;
        seed = seed * 1103515245u + 12345u;
        double t = grid + c->late_ms / 1000 * (double)(seed >> 8) / (double)(1u << 24);
        k++;
        if (c->spike && (seed >> 4) % (unsigned)c->spike == 0)
            t += 0.0035;
        if (c->miss && k % c->miss == 0)
            continue;
        pace_add(&p, before < 0 ? VGA_FRAME_NS : (uint64_t)((t - before) * 1e9), true);
        before = t;
        int n = 0;
        for (; pace_due(&p); n++)
            pace_ran(&p);
        if (grid < 10)
        {
            t0 = t;
            followed = p.follows;
            continue;
        }
        if (p.follows != followed)
            g.changes++, followed = p.follows;
        frames += n;
        if (n == 0)
            g.none++;
        if (n >= 2)
            g.twos++;
        if ((n == 0 && last >= 2) || (n >= 2 && last == 0))
            g.flips++;
        last = n;
    }
    g.speed = (double)frames / ((before - t0) * VGA_HZ);
    printf("%.2f Hz, %.1f ms late, miss %d, spike %d: speed %+.3f%%, %d with none, %d with two, %d flips, "
           "%d changes\n",
           c->hz, c->late_ms, c->miss, c->spike, (g.speed - 1) * 100, g.none, g.twos, g.flips, g.changes);
    return g;
}

/* A display in the range, or at a whole multiple of it, is the machine's
 * frame rate: a frame every callback, or every second, third or fourth, and
 * the machine as far off the wall as the display is off that many times
 * 60 Hz. */
static void pace_follows(int *utest_result, double hz, double most_late_ms)
{
    const double late[] = {0, 0.3, most_late_ms};
    const int each = (int)(hz / VGA_HZ + 0.5);
    for (unsigned i = 0; i < sizeof late / sizeof *late; i++)
    {
        const pace_got_t g = pace_run(&(pace_case_t){.hz = hz, .late_ms = late[i]});
        ASSERT_EQ(0, g.twos);
        ASSERT_EQ(0, g.flips);
        ASSERT_NEAR(hz / each / VGA_HZ, g.speed, 0.0001);
        if (each == 1)
            ASSERT_EQ(0, g.none);
    }
}

UTEST(pace, a_1366x768_panel_runs_a_frame_a_callback) { pace_follows(utest_result, 59.79, 1.5); }
UTEST(pace, a_1280x800_panel_runs_a_frame_a_callback) { pace_follows(utest_result, 59.81, 1.5); }
UTEST(pace, an_ntsc_display_runs_a_frame_a_callback) { pace_follows(utest_result, 59.94, 1.5); }
UTEST(pace, a_60_hz_display_runs_a_frame_a_callback) { pace_follows(utest_result, 60.0, 1.5); }
UTEST(pace, a_1280x1024_display_runs_a_frame_a_callback) { pace_follows(utest_result, 60.02, 1.5); }
UTEST(pace, a_120_hz_display_runs_a_frame_every_second_callback) { pace_follows(utest_result, 120, 1.0); }
UTEST(pace, a_119_88_hz_display_runs_a_frame_every_second_callback) { pace_follows(utest_result, 119.88, 1.0); }
UTEST(pace, a_180_hz_display_runs_a_frame_every_third_callback) { pace_follows(utest_result, 180, 0.6); }
UTEST(pace, a_240_hz_display_runs_a_frame_every_fourth_callback) { pace_follows(utest_result, 239.76, 0.5); }
/* At the ends two seconds of callbacks 1.5 ms late do not always tell the
 * display from one just outside, so these are no later than most are. */
UTEST(pace, the_slow_end_of_the_range_runs_a_frame_a_callback) { pace_follows(utest_result, 59.77, 0.3); }
UTEST(pace, the_fast_end_of_the_range_runs_a_frame_a_callback) { pace_follows(utest_result, 60.05, 0.3); }

/* A callback 3.5 ms late now and then, as a busy desktop makes them, does
 * not put a display out of the range, nor stop one being followed for even
 * a callback, whose sound would change speed with it. */
UTEST(pace, a_callback_late_now_and_then_leaves_a_display_followed)
{
    static const double hz[] = {59.79, 59.81, 59.94, 60.0, 60.02, 119.88, 120, 180, 240};
    for (unsigned i = 0; i < sizeof hz / sizeof *hz; i++)
        for (int spike = 60; spike <= 600; spike *= 10)
        {
            const pace_got_t g = pace_run(&(pace_case_t){.hz = hz[i], .late_ms = 0.3, .spike = spike});
            ASSERT_EQ(0, g.changes);
            ASSERT_EQ(0, g.twos);
            ASSERT_EQ(0, g.flips);
        }
}

/* A display at the edge of the range, with callbacks late enough that two
 * seconds of them cannot place it, is on one side or the other and seldom
 * changes. */
UTEST(pace, a_display_at_the_edge_of_the_range_seldom_changes_sides)
{
    static const double hz[] = {59.75, 59.76, 60.06, 60.09};
    for (unsigned i = 0; i < sizeof hz / sizeof *hz; i++)
    {
        const pace_got_t g = pace_run(&(pace_case_t){.hz = hz[i], .late_ms = 2});
        ASSERT_LE(g.changes, 6);
    }
}

/* Any other display leaves the machine on the wall clock, and its callbacks
 * cross from one frame to two or none once each time the rates slip a
 * frame, not back and forth. */
static void pace_keeps_time(int *utest_result, double hz, double late_ms)
{
    const pace_got_t g = pace_run(&(pace_case_t){.hz = hz, .late_ms = late_ms});
    ASSERT_NEAR(1.0, g.speed, 0.0002);
    ASSERT_EQ(0, g.flips);
    if (hz > VGA_HZ)
        ASSERT_EQ(0, g.twos);
    else
        ASSERT_EQ(0, g.none);
}

UTEST(pace, an_800x600_display_keeps_time) { pace_keeps_time(utest_result, 60.32, 0.3); }
UTEST(pace, a_display_just_slow_of_the_range_keeps_time) { pace_keeps_time(utest_result, 59.6, 0.3); }
UTEST(pace, a_50_hz_display_keeps_time) { pace_keeps_time(utest_result, 50, 0.3); }
UTEST(pace, a_75_hz_display_keeps_time) { pace_keeps_time(utest_result, 75, 0.3); }
UTEST(pace, a_144_hz_display_keeps_time) { pace_keeps_time(utest_result, 144, 0.3); }
UTEST(pace, a_165_hz_display_keeps_time) { pace_keeps_time(utest_result, 165, 0.3); }
UTEST(pace, the_timer_of_a_moved_window_keeps_time) { pace_keeps_time(utest_result, 64, 1.0); }

/* A display that misses a callback now and then is still that display: the
 * callback after runs the frame that was missed, and nothing else changes. */
UTEST(pace, a_display_that_misses_callbacks_is_still_followed)
{
    const pace_got_t g = pace_run(&(pace_case_t){.hz = 59.94, .late_ms = 0.3, .miss = 25});
    ASSERT_EQ(0, g.none);
    ASSERT_EQ(0, g.flips);
    ASSERT_NEAR(59.94 / VGA_HZ, g.speed, 0.0001);
}

/* A window moved for a minute: the machine is on the wall clock while the
 * timer runs the callbacks and back on the display after. */
UTEST(pace, a_window_moved_and_let_go_keeps_time)
{
    const pace_got_t g = pace_run(&(pace_case_t){
        .hz = 60, .late_ms = 0.3, .from_s = 100, .to_s = 160, .other_hz = 64});
    ASSERT_NEAR(1.0, g.speed, 0.0005);
}

UTEST_MAIN();
