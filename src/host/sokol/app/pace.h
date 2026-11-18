/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _HOST_SOKOL_APP_PACE_H_
#define _HOST_SOKOL_APP_PACE_H_

/* How many machine frames a display's callbacks run.
 *
 * The debt is the wall time owed to the machine, repaid a frame at a time.
 * A frame is due half a callback before a whole one is owed, which is midway
 * between what two callbacks in a row find, so callbacks run the frames
 * they should however unevenly they land: what one came late by, the next
 * comes early by.
 *
 * A display from PACE_SLOW_HZ to PACE_FAST_HZ is the machine's frame rate:
 * each callback runs a frame, and the 6502, which has the same cycles in
 * every frame, runs that fraction of a percent off the wall. The range
 * takes in the rates the display timings call 60 Hz, each with room for a
 * crystal at the edge of what it may be: 59.79 Hz, 1366x768; 59.81 Hz,
 * 1280x800; 59.94 Hz, NTSC's and most televisions'; 60.00 Hz; 60.02 Hz,
 * 1280x1024. Past the slow end a 6502 program would run more than four
 * parts in a thousand slow. A display at two, three or four times a rate in the
 * range is the same with a frame every second, third or fourth callback:
 * 120, 180 and 240 Hz.
 *
 * Any other display leaves the machine on the wall clock, and its callbacks
 * run a frame more or a frame fewer where the two rates slip past each
 * other: a 50 or 144 Hz display, 800x600's 60.32 Hz, and the 64 Hz timer
 * Windows runs the callbacks from while a window is moved.
 *
 * Two measures of the display decide which it is. A fine one, the time
 * 128 frames' worth of callbacks took, has to be in the range, and the
 * display changes sides only when two of those in a row agree: one late
 * callback makes one long and the next short. Until one has been taken the
 * machine is on the wall clock. A quick one, smoothed over half a second,
 * has to come within a hundredth of a frame, and then stay within four; it
 * is what sees a window start to move, which a late callback or two are
 * not. A callback that took the time of two is two, so a display that
 * missed one is not taken for a slower display.
 *
 * The sound is told how fast the machine then runs (pace_speed), so that a
 * display's rate is not left for its trim to find. */

#include "core/vga/vga_emu.h"
#include <stdbool.h>
#include <stdint.h>

#define PACE_SLOW_HZ 59.76
#define PACE_FAST_HZ 60.06
#define PACE_SLOW_NS ((int64_t)(1e9 / PACE_SLOW_HZ))
#define PACE_FAST_NS ((int64_t)(1e9 / PACE_FAST_HZ))

#define PACE_FRAME_NS ((int64_t)VGA_FRAME_NS)
// A callback further off its display's period than this is not that display.
#define PACE_QUICK_NS (PACE_FRAME_NS / 100)
// A display in the range stays in it until this far outside.
#define PACE_STAY_NS (PACE_FRAME_NS / 2000)
// The frames the fine measure is the mean of.
#define PACE_SPAN 128
// A period is never taken for less than this, a millionth of a second.
#define PACE_LEAST_NS 1000
// The most of a frame one callback's lateness is taken to be.
#define PACE_LATE_NS (PACE_FRAME_NS / 50)
/* A frame that is due stays due this far short of it, and a second one is
 * not due until this far past it, so callbacks that sit on the edge between
 * one frame and two, or one and none, cross it once and not back. */
#define PACE_EDGE_NS (PACE_FRAME_NS / 16)

typedef struct
{
    int64_t debt_ns;
    int64_t period_ns; // the display's, smoothed; 0 before the first callback
    int64_t carry_ns;  // of the last callbacks, what was not a whole period
    int64_t span_ns;   // the fine measure: this long
    uint16_t periods;  // for this many periods
    uint8_t folds;     // callbacks in a row that took two periods or more
    uint8_t shorts;    // callbacks in a row well short of a period
    int8_t out;        // the last fine measure: slow of the range, in it, or fast
    bool measured;     // a fine measure has been taken
    bool quick;        // the quick measure has the callbacks a frame apart
    bool near;         // the fine measure found the display in the range
    bool follows;      // the display is the machine's frame rate
    bool ran;          // a frame has run in this callback
    int64_t frame_ns;  // a frame of the display followed, smoothed
} pace_t;

static inline int64_t pace_round(int64_t ns, int64_t of)
{
    return (ns + (ns < 0 ? -of : of) / 2) / of;
}

/* A callback dt_ns after the one before, paced by the display or not. A
 * stall of more than three frames is dropped, not replayed as a burst. */
static inline void pace_add(pace_t *p, uint64_t dt_ns, bool paced)
{
    const bool stalled = dt_ns > 3 * VGA_FRAME_NS;
    const int64_t dt = stalled ? 3 * PACE_FRAME_NS : (int64_t)dt_ns;
    p->debt_ns += dt;
    p->ran = false;
    if (!p->period_ns)
        p->period_ns = dt > PACE_LEAST_NS ? dt : PACE_LEAST_NS;
    /* The periods this callback was. The carry leaks, so what the smoothed
     * period is off by does not add up to a period miscounted. */
    p->carry_ns += dt;
    const int64_t n = pace_round(p->carry_ns, p->period_ns);
    p->carry_ns -= n * p->period_ns;
    p->carry_ns -= p->carry_ns / 8;
    p->folds = n > 1 ? p->folds + 1 : 0;
    p->shorts = dt * 3 < p->period_ns * 2 ? p->shorts + 1 : 0;
    if (stalled || p->folds == 8 || p->shorts == 8)
    {
        /* Not a display missing callbacks, or one late and the next early:
         * one that stopped, or changed. */
        if (!stalled)
            p->period_ns = dt > PACE_LEAST_NS ? dt : PACE_LEAST_NS;
        p->carry_ns = p->span_ns = 0;
        p->periods = p->folds = p->shorts = 0;
        p->out = 0;
    }
    else
    {
        if (n > 0)
            p->period_ns += (dt / n - p->period_ns) / 32;
        p->span_ns += dt;
        p->periods = (uint16_t)(p->periods + n);
    }
    /* The callbacks to a frame, were this a display the machine follows. */
    int64_t each = pace_round(PACE_FRAME_NS, p->period_ns);
    each = each < 1 ? 1 : each > 4 ? 4 : each;
    if (p->periods >= PACE_SPAN * each)
    {
        const int64_t mean = p->span_ns * each / p->periods;
        const int64_t stay = p->near ? PACE_STAY_NS : 0;
        const int8_t out = mean > PACE_SLOW_NS + stay ? -1 : mean < PACE_FAST_NS - stay ? 1 : 0;
        const bool was = p->near;
        if (out == p->out || !p->measured)
            p->near = !out;
        /* Smoothed while the display stays in the range; a display that
         * has just come into it is what this measure found. */
        if (!out)
            p->frame_ns = was && p->near ? p->frame_ns + (mean - p->frame_ns) / 8 : mean;
        p->out = out;
        p->measured = true;
        p->span_ns = 0;
        p->periods = 0;
    }
    const int64_t apart = p->period_ns * each - PACE_FRAME_NS;
    if (apart < PACE_QUICK_NS && apart > -PACE_QUICK_NS)
        p->quick = true;
    else if (apart > 4 * PACE_QUICK_NS || apart < -4 * PACE_QUICK_NS)
        p->quick = false;
    p->follows = paced && p->near && p->quick;
    if (p->follows)
    {
        /* The display's frames are the machine's: what the debt is off a
         * whole number of callbacks by is the callback's lateness and the
         * display's rate, and is forgotten. */
        const int64_t step = PACE_FRAME_NS / each;
        const int64_t off = p->debt_ns - pace_round(p->debt_ns, step) * step;
        p->debt_ns -= off > PACE_LATE_NS ? PACE_LATE_NS : off < -PACE_LATE_NS ? -PACE_LATE_NS : off;
    }
}

/* The machine's speed against the wall clock: the display's, while the
 * machine follows it. */
static inline double pace_speed(const pace_t *p)
{
    return p->follows && p->frame_ns ? (double)PACE_FRAME_NS / (double)p->frame_ns : 1;
}

/* Time the callback slept through itself, when nothing paced it. */
static inline void pace_slept(pace_t *p, uint64_t ns)
{
    p->debt_ns += ns > 3 * VGA_FRAME_NS ? 3 * PACE_FRAME_NS : (int64_t)ns;
}

// Whether a frame is due, which pace_ran then takes off the debt.
static inline bool pace_due(const pace_t *p)
{
    const int64_t period = p->period_ns < PACE_FRAME_NS ? p->period_ns : PACE_FRAME_NS;
    return p->debt_ns >= PACE_FRAME_NS - period / 2 + (p->ran ? PACE_EDGE_NS : -PACE_EDGE_NS);
}

static inline void pace_ran(pace_t *p)
{
    p->debt_ns -= PACE_FRAME_NS;
    p->ran = true;
}

#endif /* _HOST_SOKOL_APP_PACE_H_ */
