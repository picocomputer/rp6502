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
 *   GFX_FILTER_SHARP    each pixel a solid block, blended only across the
 *                       one window pixel where two meet, which keeps pixels
 *                       crisp and even at any window size
 * Takes effect on the next frame. The headless --screenshot path renders at
 * native resolution, so the filter does nothing there. */
typedef enum
{
    GFX_FILTER_NEAREST,
    GFX_FILTER_LINEAR,
    GFX_FILTER_SHARP,
} gfx_filter_t;

void gfx_set_filter(gfx_filter_t filter);

/* The frame is the shape the window, or the web's player box, keeps. It is
 * dynamic by default: the frame is the canvas the program is on, so the window
 * follows each mode change. A fixed frame is the console, at the size the host
 * gave it or a savestate restored, and every other canvas is letterboxed or
 * pillarboxed inside it. Set before gfx_prepare. */
void gfx_set_fixed_aspect(bool fixed);

#ifdef EMU_WITH_DEBUGGER
/* The debugger's Window Scale menu is the only caller of these two, so they are
 * built only with it.
 *
 * Resize the window to what --scale <n> opens: the frame aspect at n times
 * VGA_MAX_HEIGHT, plus the debugger's menu strip when the overlay is up. Docked
 * panels are not counted, because this is a reset to a known size after a manual
 * resize. The window manager may ignore the request. The menu runs under the
 * machine's lock, so the resize waits for gfx_canvas_changed, which does not. */
void gfx_set_scale(double scale);

/* The window's current scale by the same formula; 0 when there is no window. */
double gfx_get_scale(void);
#endif

/* The canvas's width on screen, in framebuffer pixels. The input layer
 * measures host mouse motion against it, so pointer speed does not change
 * with the window size. */
float gfx_canvas_width(void);

/* Map a framebuffer-pixel point, such as a sokol event's mouse_x and mouse_y or
 * a touchpoint, to canvas pixel coordinates clamped to the canvas. True when the
 * point was over the drawn canvas, false when it was in the letterbox or outside
 * the window. */
bool gfx_canvas_from_fb(float px, float py, int *cx, int *cy);

/* The frame the window shows (run.h), whose shape the window, the viewport
 * and the pointer's mapping go by: what is on screen, which can be a frame
 * behind the machine. Set before gfx_prepare, and at every frame. */
struct run_frame_t;
void gfx_set_frame(const struct run_frame_t *frame);

/* The rest of these are called in the order declared: gfx_prepare before sokol
 * starts, gfx_setup from the init callback, gfx_canvas_changed through
 * gfx_end_pass once a frame, and gfx_shutdown from cleanup.
 *
 * Seed the canvas from the launch options and report the window's initial size
 * in pixels: the frame aspect at the requested scale plus the debugger's menu
 * strip, or the size the last debug session was left at. */
void gfx_prepare(double scale, bool have_scale, int *out_w, int *out_h);

/* From the sokol init callback, after sg_setup. */
void gfx_setup(void);

/* A program picking a new mode changes the canvas size mid-run, which changes
 * a dynamic frame, and a savestate load can change the console, which changes a
 * fixed one. Notice a new frame shape, keep the window's aspect lock honest,
 * and re-fit a window the user has not resized off-aspect. Also makes a resize
 * gfx_set_scale asked for. Outside the machine's lock: these are calls to the
 * window system. */
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
