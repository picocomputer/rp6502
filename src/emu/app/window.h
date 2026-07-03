/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_WINDOW_H_
#define _EMU_WINDOW_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Open a sokol window and run the machine until closed. The ROM must already be
 * loaded and emu_init() called. fb is the caller-owned framebuffer (must hold
 * the largest canvas); vga renders into it and the window presents it. scale
 * may be fractional; have_scale marks an explicit --scale, which beats the
 * remembered debug-session window size. vsync sets the GL swap interval (off =
 * present uncapped; the driver may ignore it either way). The title shows
 * "(stopped)" once the program exits; exit_on_halt closes the window then
 * instead of leaving the final output up. */
int emu_run_window(uint32_t *fb, double scale, bool have_scale, bool vsync, bool exit_on_halt);

/* Letterbox/pillarbox fill color behind the canvas (RGB 0-255, default black). */
void emu_set_bgcolor(uint8_t r, uint8_t g, uint8_t b);

/* Scaling filter for the canvas->window blit.
 *   EMU_FILTER_NEAREST  crisp blocky pixels (point sampling; uneven pixel
 *                       widths "wobble" at non-integer window scales)
 *   EMU_FILTER_LINEAR   plain bilinear (smooth but blurry)
 *   EMU_FILTER_SHARP    sharp-bilinear: point-prescale to the largest integer
 *                       multiple that fits, then bilinear-downscale the rest —
 *                       crisp pixels with smooth motion at any window size
 * Call before emu_run_window (or any time; takes effect next frame). The
 * headless --screenshot path renders at native resolution, so the filter has
 * no effect there. */
typedef enum
{
    EMU_FILTER_NEAREST,
    EMU_FILTER_LINEAR,
    EMU_FILTER_SHARP,
} emu_scale_filter_t;

void emu_set_scale_filter(emu_scale_filter_t filter);

/* Resize the window to what --scale <n> opens: the canvas aspect at
 * n x VGA_MAX_HEIGHT, plus the debugger menu strip when the overlay is up —
 * deliberately ignoring docked panels (it is a reset to a known size after a
 * manual resize). The WM may ignore the request. */
void emu_set_window_scale(double scale);

/* The window's current scale by the same formula; 0 when there is no window. */
double emu_get_window_scale(void);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_WINDOW_H_ */
