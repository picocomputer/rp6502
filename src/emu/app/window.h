/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#ifndef _EMU_APP_WINDOW_H_
#define _EMU_APP_WINDOW_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Open a sokol window and run the machine until closed. The ROM must already be
 * loaded and main_init() called. fb is the caller-owned framebuffer (must hold
 * the largest canvas); vga renders into it and the window presents it. scale
 * may be fractional; have_scale marks an explicit --scale, which beats the
 * remembered debug-session window size. vsync sets the GL swap interval (off =
 * present uncapped; the driver may ignore it either way). The title shows
 * "(stopped)" once the program exits; exit_on_halt closes the window then
 * instead of leaving the final output up. */
int window_run(uint32_t *fb, double scale, bool have_scale, bool vsync, bool exit_on_halt);

/* Letterbox/pillarbox fill color behind the canvas (RGB 0-255, default black). */
void window_set_bgcolor(uint8_t r, uint8_t g, uint8_t b);

/* Scaling filter for the canvas->window blit.
 *   WINDOW_FILTER_NEAREST  crisp blocky pixels (point sampling; uneven pixel
 *                       widths "wobble" at non-integer window scales)
 *   WINDOW_FILTER_LINEAR   plain bilinear (smooth but blurry)
 *   WINDOW_FILTER_SHARP    sharp-bilinear: point-prescale to the largest integer
 *                       multiple that fits, then bilinear-downscale the rest —
 *                       crisp pixels with smooth motion at any window size
 * Call before window_run (or any time; takes effect next frame). The
 * headless --screenshot path renders at native resolution, so the filter has
 * no effect there. */
typedef enum
{
    WINDOW_FILTER_NEAREST,
    WINDOW_FILTER_LINEAR,
    WINDOW_FILTER_SHARP,
} window_scale_filter_t;

void window_set_scale_filter(window_scale_filter_t filter);

/* Resize the window to what --scale <n> opens: the canvas aspect at
 * n x VGA_MAX_HEIGHT, plus the debugger menu strip when the overlay is up —
 * deliberately ignoring docked panels (it is a reset to a known size after a
 * manual resize). The WM may ignore the request. */
void window_set_scale(double scale);

/* The window's current scale by the same formula; 0 when there is no window. */
double window_get_scale(void);

/* On-screen pixels per canvas pixel (the aspect-fit blit scale). The input layer
 * divides host mouse motion by this so pointer speed is window-size independent. */
float window_canvas_scale(void);

/* Map a framebuffer-pixel point (sokol e->mouse_x/y or a touchpoint) to canvas
 * pixel coords, clamped to the canvas. Returns true when the raw point was over
 * the drawn canvas (false = in the letterbox / outside, coords set to 0,0). */
bool window_canvas_from_fb(float px, float py, int *cx, int *cy);

/* Tell the window layer whether the host pointer is over the drawn canvas, so the
 * tablet's requested cursor applies only there and the system cursor shows in the
 * letterbox. */
void window_set_pointer_on_canvas(bool on);

#ifdef __cplusplus
}
#endif

#endif /* _EMU_APP_WINDOW_H_ */
