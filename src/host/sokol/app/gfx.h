/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * How the machine's framebuffer lands in the window. gfx_blit and the input
 * layer both convert coordinates through the functions here, so a canvas pixel
 * maps to the same screen position in both.
 */

#ifndef _HOST_SOKOL_APP_GFX_H_
#define _HOST_SOKOL_APP_GFX_H_

#include <stdbool.h>
#include <stdint.h>

/* Letterbox/pillarbox fill color behind the canvas (RGB 0-255, default black). */
void gfx_set_bgcolor(uint8_t r, uint8_t g, uint8_t b);

/* Scaling filter for the blit from canvas to window.
 *   GFX_FILTER_NEAREST  point sampling, so pixels stay crisp but their widths
 *                       come out uneven at a non-integer window scale
 *   GFX_FILTER_LINEAR   plain bilinear, smooth but blurry
 *   GFX_FILTER_SHARP    point-prescale to the largest integer multiple that
 *                       fits, then bilinear down to the rest, which keeps
 *                       pixels crisp and motion smooth at any window size
 * Takes effect on the next frame. The headless --screenshot path renders at
 * native resolution, so the filter does nothing there. */
typedef enum
{
    GFX_FILTER_NEAREST,
    GFX_FILTER_LINEAR,
    GFX_FILTER_SHARP,
} gfx_filter_t;

void gfx_set_filter(gfx_filter_t filter);

/* Resize the window to what --scale <n> opens: the canvas aspect at n times
 * VGA_MAX_HEIGHT, plus the debugger's menu strip when the overlay is up. Docked
 * panels are not counted, because this is a reset to a known size after a manual
 * resize. The window manager may ignore the request. */
void gfx_set_scale(double scale);

/* The window's current scale by the same formula; 0 when there is no window. */
double gfx_get_scale(void);

/* On-screen pixels per canvas pixel. The input layer divides host mouse motion
 * by this, so pointer speed does not change with the window size. */
float gfx_canvas_scale(void);

/* Map a framebuffer-pixel point, such as a sokol event's mouse_x and mouse_y or
 * a touchpoint, to canvas pixel coordinates clamped to the canvas. True when the
 * point was over the drawn canvas, false when it was in the letterbox or outside
 * the window. */
bool gfx_canvas_from_fb(float px, float py, int *cx, int *cy);

/* The rest of these are called in the order declared: gfx_prepare before sokol
 * starts, gfx_setup from the init callback, gfx_canvas_changed through
 * gfx_end_pass once a frame, and gfx_shutdown from cleanup.
 *
 * Seed the canvas from the launch options and report the window's initial size
 * in pixels: the canvas aspect at the requested scale plus the debugger's menu
 * strip, or the size the last debug session was left at. */
void gfx_prepare(uint32_t *fb, double scale, bool have_scale, int *out_w, int *out_h);

/* From the sokol init callback, after sg_setup. */
void gfx_setup(void);

/* A program picking a new mode changes the canvas size mid-run. Notice that,
 * keep the window manager's aspect hint honest, and re-fit a window the user
 * has not resized off-aspect. */
void gfx_canvas_changed(void);

/* Size the canvas to the window and take the frame the machine just rendered,
 * if it rendered one. A duplicate present re-blits what is already uploaded. */
void gfx_upload(bool new_frame);

void gfx_begin_pass(void);

/* Blit the canvas into the current pass, letterboxed. The application's
 * overlays draw after this, in the same pass. */
void gfx_blit(void);

void gfx_end_pass(void);

void gfx_shutdown(void);

#endif /* _HOST_SOKOL_APP_GFX_H_ */
