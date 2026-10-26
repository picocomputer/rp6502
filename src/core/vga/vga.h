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
    vga_canvas_320_200,
    vga_canvas_640_400,
    VGA_CANVAS_COUNT,
} vga_canvas_t;

unsigned long vga_frame_count(void);

/* False where no display is attached. Always true where the display cannot be
 * unplugged. */
bool vga_connected(void);

vga_canvas_t vga_get_canvas(void);

void vga_canvas_size(int *w, int *h);
int16_t vga_canvas_height(void);
int16_t vga_canvas_width(void);
int16_t vga_canvas_y_scale(void);

/* The size of the console canvas. A Pico RIA derives it from the display the
 * VGA is plugged into. A machine that draws its own console holds it in
 * canvas.c, where it is 640x480 unless the host sets another size before the
 * machine starts, and a savestate load sets the size the savestate carries. A
 * new size takes effect at the next vga_canvas_select or vga_canvas_load. */
void vga_console_size(int *w, int *h);
void vga_set_console_size(int w, int h);

bool vga_canvas_is_console(void);

void vga_set_code_page(uint16_t cp);

void vga_load_code_page(uint16_t cp);

bool vga_canvas_select(uint16_t canvas);

void vga_canvas_reset(void);

/* What a stop does to the console on a machine that draws its own: the
 * console canvas is selected, the terminal is reset without clearing its
 * screen, and a mode program left half assembled in the xregs is dropped. */
void vga_console_reset(void);

void vga_canvas_publish(vga_canvas_t canvas);

/* The mode a program is about to be laid down for, announced before any of its
 * planes are booked. A machine that rasterizes in the FPGA needs this, because
 * its scanline table holds a mode number and vga_prog_fill and vga_prog_sprite
 * identify the mode only by a function pointer; a machine that rasterizes in
 * software ignores it. */
void vga_mode_begin(uint8_t mode, uint16_t attr);

bool vga_canvas_load(uint16_t canvas);
vga_canvas_t vga_canvas_code(void);

bool vga_mode_prog(uint16_t mode, uint16_t *xregs);

/* The size of every canvas, and the one list of the canvases there are. The
 * console's size belongs to the machine, so it comes from vga_console_size.
 * False for a code that names no canvas. */
static inline bool vga_canvas_geometry(uint16_t code, int *w, int *h)
{
    switch (code)
    {
    case vga_canvas_console: vga_console_size(w, h); return true;
    case vga_canvas_320_240: *w = 320; *h = 240; return true;
    case vga_canvas_320_180: *w = 320; *h = 180; return true;
    case vga_canvas_640_480: *w = 640; *h = 480; return true;
    case vga_canvas_640_360: *w = 640; *h = 360; return true;
    case vga_canvas_320_200: *w = 320; *h = 200; return true;
    case vga_canvas_640_400: *w = 640; *h = 400; return true;
    }
    return false;
}

/* The console has 40 columns in an 8x8 font on a canvas 320 pixels wide, and
 * 80 columns in an 8x16 font on one 640 wide, whatever the canvas's height. */
static inline int vga_term_columns(int w)
{
    return w == 320 ? 40 : 80;
}

static inline int vga_term_row_px(int w)
{
    return w == 320 ? 8 : 16;
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
    if (!(highest > 0 && highest <= vga_canvas_height()))
        highest = vga_canvas_height();
    /* In lines of timing, which is what the beam counts. */
    return highest * vga_canvas_y_scale();
}

/* Book scanlines for a mode. fill_fn is the renderer itself where the machine
 * rasterizes in software; where it rasterizes in the FPGA, fill_fn is ignored
 * and the mode announced by vga_mode_begin is written to the scanline table
 * instead. */
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
