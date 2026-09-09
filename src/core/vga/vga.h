/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_VGA_VGA_H_
#define _CORE_VGA_VGA_H_

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    vga_canvas_console = 0,
    vga_canvas_320_240,
    vga_canvas_320_180,
    vga_canvas_640_480,
    vga_canvas_640_360,
} vga_canvas_t;

unsigned long vga_frame_count(void);

/* False where no display is attached. Always true where the display cannot be
 * unplugged. */
bool vga_connected(void);

vga_canvas_t vga_get_canvas(void);
uint8_t vga_get_display_type(void);

void vga_canvas_size(int *w, int *h);
int16_t vga_canvas_height(void);
int16_t vga_canvas_width(void);

bool vga_canvas_is_console(void);

void vga_set_code_page(uint16_t cp);

void vga_load_code_page(uint16_t cp);

bool vga_canvas_select(uint16_t canvas);

void vga_canvas_reset(void);

void vga_canvas_publish(vga_canvas_t canvas);

/* The mode a program is about to be laid down for, announced before any of its
 * planes are booked. A machine whose fabric rasterizes needs this, because a
 * fill function pointer means nothing to it; a machine that rasterizes in
 * software ignores it. */
void vga_mode_begin(uint8_t mode, uint16_t attr);

bool vga_canvas_load(uint16_t canvas);
vga_canvas_t vga_canvas_code(void);

bool vga_mode_prog(uint16_t mode, uint16_t *xregs);

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

/* Rows in a scanline program, which also bounds a scanline number. It is 512
 * rather than 480 because the VGA firmware draws the console canvas on a
 * 1280x1024 display as a 640x512 view. */
#define VGA_PROG_MAX 512

int16_t vga_prog_highest(void);
bool vga_prog_valid(int16_t plane, int16_t scanline_begin, int16_t *scanline_end);

static inline int16_t vga_vsync_line(void)
{
    int16_t highest = vga_prog_highest();
    if (highest > 0 && highest <= vga_canvas_height())
        return highest;
    return vga_canvas_height();
}

/* Book scanlines for a mode. fill_fn is the renderer itself where the machine
 * rasterizes in software; where the fabric rasterizes it is ignored, and the
 * mode announced by vga_mode_begin is written to the fabric instead. */
bool vga_prog_fill(int16_t plane, int16_t scanline_begin, int16_t scanline_end,
                   uint16_t config_ptr,
                   bool (*fill_fn)(int16_t plane_id,
                                   int16_t scanline,
                                   int16_t width,
                                   uint16_t *rgb,
                                   uint16_t config_ptr));

/* Books like vga_prog_fill, but is allowed on the console canvas, and first
 * clears the rows its last booking covered, so a renderer that re-programs
 * itself over a different range leaves nothing behind. */
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
