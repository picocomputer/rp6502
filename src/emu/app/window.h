/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Windowed presentation (app_sokol.c): open a sokol window and run the machine
 * until closed, plus the letterbox fill color and the canvas->window scale
 * filter. A stub keeps the headless (--screenshot) build self-contained.
 */

#ifndef _EMU_WINDOW_H_
#define _EMU_WINDOW_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* The app-owned framebuffer geometry: the largest canvas (the 640x480 boot
 * console). Owners size their storage with these and register it with
 * vga_set_framebuffer before running frames. */
#define EMU_FB_WIDTH 640
#define EMU_FB_HEIGHT 480

/* Open a sokol window and run the machine until closed. The ROM must already be
 * loaded and emu_init() called. scale may be fractional. vsync sets the GL swap
 * interval (off = present uncapped; the driver may ignore it either way). The
 * title shows "(stopped)" once the program exits; exit_on_halt closes the window
 * then instead of leaving the final output up. */
int emu_run_window(double scale, bool vsync, bool exit_on_halt);

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

#ifdef __cplusplus
}
#endif

#endif /* _EMU_WINDOW_H_ */
