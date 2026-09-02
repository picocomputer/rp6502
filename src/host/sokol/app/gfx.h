/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The canvas: how the machine's framebuffer lands in the window. Everything
 * about size, aspect and filtering is here, including the coordinate map both
 * the render pass and the input layer read, so there is one answer to where a
 * canvas pixel is on screen.
 */

#ifndef _HOST_SOKOL_APP_GFX_H_
#define _HOST_SOKOL_APP_GFX_H_

#include <stdbool.h>
#include <stdint.h>

/* Letterbox/pillarbox fill color behind the canvas (RGB 0-255, default black). */
void gfx_set_bgcolor(uint8_t r, uint8_t g, uint8_t b);

/* Scaling filter for the canvas->window blit.
 *   GFX_FILTER_NEAREST  crisp blocky pixels (point sampling; uneven pixel
 *                       widths "wobble" at non-integer window scales)
 *   GFX_FILTER_LINEAR   plain bilinear (smooth but blurry)
 *   GFX_FILTER_SHARP    sharp-bilinear: point-prescale to the largest integer
 *                       multiple that fits, then bilinear-downscale the rest —
 *                       crisp pixels with smooth motion at any window size
 * Call before entry_run (or any time; takes effect next frame). The
 * headless --screenshot path renders at native resolution, so the filter has
 * no effect there. */
typedef enum
{
    GFX_FILTER_NEAREST,
    GFX_FILTER_LINEAR,
    GFX_FILTER_SHARP,
} gfx_filter_t;

void gfx_set_filter(gfx_filter_t filter);

/* Resize the window to what --scale <n> opens: the canvas aspect at
 * n x VGA_MAX_HEIGHT, plus the debugger menu strip when the overlay is up —
 * deliberately ignoring docked panels (it is a reset to a known size after a
 * manual resize). The WM may ignore the request. */
void gfx_set_scale(double scale);

/* The window's current scale by the same formula; 0 when there is no window. */
double gfx_get_scale(void);

/* On-screen pixels per canvas pixel (the aspect-fit blit scale). The input layer
 * divides host mouse motion by this so pointer speed is window-size independent. */
float gfx_canvas_scale(void);

/* Map a framebuffer-pixel point (sokol e->mouse_x/y or a touchpoint) to canvas
 * pixel coords, clamped to the canvas. Returns true when the raw point was over
 * the drawn canvas (false = in the letterbox / outside, coords set to 0,0). */
bool gfx_canvas_from_fb(float px, float py, int *cx, int *cy);

/* ---- the frame, in the order the application calls it ---- */

/* Seed the canvas from the launch options and report the window's initial pixel
 * size: the canvas aspect at the requested scale, plus the debugger's menu
 * strip, or the size the last debug session was left at. */
void gfx_prepare(uint32_t *fb, double scale, bool have_scale, int *out_w, int *out_h);

/* The framebuffer object and the first canvas size. From the sokol init
 * callback, after sg_setup. */
void gfx_setup(void);

/* The canvas the machine renders can change size mid-run when a program picks a
 * new mode: notice that, keep the WM's aspect hint honest, and re-fit a window
 * the user has not resized off-aspect. */
void gfx_canvas_changed(void);

/* Size the canvas to the window and take the frame the machine just rendered,
 * if it rendered one -- a duplicate present re-blits what is already uploaded. */
void gfx_upload(bool new_frame);

/* The swapchain pass, cleared to the letterbox color. */
void gfx_begin_pass(void);

/* Blit the canvas into it, letterboxed. Overlays draw after this, in the same
 * pass, and are the application's. */
void gfx_blit(void);

/* End the pass and commit the frame. */
void gfx_end_pass(void);

void gfx_shutdown(void);

#endif /* _HOST_SOKOL_APP_GFX_H_ */
