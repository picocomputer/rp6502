/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* What canvas is selected, how tall it is, and how a mode books the scanlines
 * it will draw. Nothing about how the pixels get out: the Pico's backchannel
 * and vsync timing, the VGA firmware's scanline programs, the Pocket's fabric
 * registers. */

#ifndef _CORE_VGA_VGA_H_
#define _CORE_VGA_VGA_H_

#include <stdbool.h>
#include <stdint.h>

// Canvas size.
typedef enum
{
    vga_canvas_console = 0,
    vga_canvas_320_240,
    vga_canvas_320_180,
    vga_canvas_640_480,
    vga_canvas_640_360,
} vga_canvas_t;

/* False where nothing is attached, so a caller can lay text out for the console
 * it does have. Always true on a machine whose display cannot be unplugged. */
bool vga_connected(void);

vga_canvas_t vga_get_canvas(void);
uint8_t vga_get_display_type(void);

// Pixel dimensions of the current canvas.
void vga_canvas_size(int *w, int *h);
int16_t vga_canvas_height(void);
int16_t vga_canvas_width(void);

/* The console canvas is the one a machine boots into and returns to; the
 * graphics modes are the other four. */
bool vga_canvas_is_console(void);

// The code page the display renders text in.
void vga_set_code_page(uint16_t cp);

/* Select a canvas, which discards whatever was programmed on the last one.
 * False for a canvas this machine does not have. A machine whose video device
 * is across a bus does not choose here -- it writes the wire and shadows what
 * it wrote (host/pico/ria/sys/vga.c). */
bool vga_canvas_select(uint16_t canvas);

/* How this machine forgets a mode program, because the canvas it described is
 * gone: a table to sweep, or registers to blank. */
void vga_canvas_reset(void);

/* Tell whatever has to be told which canvas is up. Nothing to do where the
 * selection is only a variable. */
void vga_canvas_publish(vga_canvas_t canvas);

/* A mode program is about to be laid down, before any of its planes are
 * booked. A machine whose renderer needs to be told which mode it is about to
 * draw -- fabric does, software does not, because software is told by the
 * plane it is handed -- publishes it here. */
void vga_mode_begin(uint8_t mode, uint16_t attr);

/* Program the canvas for a mode number, as the mode xreg asked. */
bool vga_mode_prog(uint16_t mode, uint16_t *xregs);

/* How big a canvas is. A pure fact about the code, which is why every
 * machine had written it out: the console is the 640x480 one. */
static inline void vga_canvas_geometry(vga_canvas_t code, int *w, int *h)
{
    switch (code)
    {
    case vga_canvas_320_240: *w = 320; *h = 240; break;
    case vga_canvas_320_180: *w = 320; *h = 180; break;
    case vga_canvas_640_360: *w = 640; *h = 360; break;
    case vga_canvas_console:
    case vga_canvas_640_480:
    default: *w = 640; *h = 480; break;
    }
}

// Number of programmable scanlines, also bounds scanline_id.
#define VGA_PROG_MAX 512

/* Booking scanlines for a mode. fill_fn is the renderer itself where the
 * machine rasterizes in software; where the fabric does, it is ignored and the
 * mode is announced out of band instead. */
bool vga_prog_fill(int16_t plane, int16_t scanline_begin, int16_t scanline_end,
                   uint16_t config_ptr,
                   bool (*fill_fn)(int16_t plane_id,
                                   int16_t scanline,
                                   int16_t width,
                                   uint16_t *rgb,
                                   uint16_t config_ptr));

// For singleton fill modes, like the terminal
bool vga_prog_exclusive(int16_t plane, int16_t scanline_begin, int16_t scanline_end,
                        uint16_t config_ptr,
                        bool (*fill_fn)(int16_t plane_id,
                                        int16_t scanline,
                                        int16_t width,
                                        uint16_t *rgb,
                                        uint16_t config_ptr));

bool vga_prog_sprite(int16_t plane, int16_t scanline_begin, int16_t scanline_end,
                     uint16_t config_ptr, uint16_t length,
                     void (*sprite_fn)(int16_t scanline,
                                       int16_t width,
                                       uint16_t *rgb,
                                       uint16_t config_ptr,
                                       uint16_t length));

#endif /* _CORE_VGA_VGA_H_ */
