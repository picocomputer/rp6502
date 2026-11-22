/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_VGA_VGA_H_
#define _CORE_VGA_VGA_H_

#include "machine.h"
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
 * new size takes effect at the next vga_canvas_select or savestate load. */
void vga_console_size(int *w, int *h);
void vga_set_console_size(int w, int h);

/* A console size a machine that draws its own console can have: 640 wide
 * and 480, 400 or 360 high, which are the sizes every host gives. */
static inline bool vga_console_size_ok(int w, int h)
{
    return w == 640 && (h == 480 || h == 400 || h == 360);
}

/* The shape of the picture the canvas is shown in, which is what a
 * touchscreen laid over the display covers. Every canvas fills the picture's
 * width and is centred down it with square pixels. A Pico RIA derives it from
 * the display type and the canvas. A machine that draws its own console shows
 * every canvas in the console's shape. */
void vga_display_size(int *w, int *h);

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

bool vga_mode_prog(uint16_t mode, uint16_t *xregs);

/* The size of every canvas, and the one list of the canvases there are. The
 * console's size belongs to the machine, so it comes from vga_console_size.
 * False for a code that names no canvas. */
static inline bool HOST_IN_RAM_FUNC vga_canvas_geometry(uint16_t code, int *w, int *h)
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
static inline int HOST_IN_RAM_FUNC vga_term_columns(int w)
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

/* What a plane's fill renderer keeps from one line to the next. The caller
 * keeps one per plane, and a set for each thread that renders lines at the
 * same time as another, and starts them zeroed.
 *
 * A scanline of a plane is drawn by one mode, so the cache holds one table
 * at a time, and key says which and what it was built from. A renderer
 * compares the whole key with what it would build from and builds on any
 * difference, so it never draws from a table another left. key[0] is a
 * vga_fill_kind_t.
 *
 * pairs_4bpp is every byte of 4bpp as its two pixels. Its key is the kind,
 * which says the bit order, and the 16 palette entries.
 *
 * row_pair is a row of text: for each cell across the line, the four ways
 * two of its pixels can land, in the cell's colors. A row is 8 or 16 lines
 * in the same colors, so the renderer builds them for the first line it
 * draws of the row and only looks glyphs up for the rest. Its key always
 * includes the frame, so a color that changes while its row is drawn shows
 * on the next frame at the latest. */
typedef enum
{
    VGA_FILL_NONE, /* nothing built: a zeroed cache */
    VGA_FILL_MODE0_ROW,
    VGA_FILL_MODE1_4BPPR, /* mode 1's depths with a color in every cell, */
    VGA_FILL_MODE1_4BPP,  /* in the order of their cells' sizes */
    VGA_FILL_MODE1_8BPP,
    VGA_FILL_MODE1_16BPP,
    VGA_FILL_4BPP,
    VGA_FILL_4BPP_REVERSE,
} vga_fill_kind_t;
#define VGA_FILL_ROW_CELLS 81
typedef struct
{
    uint32_t key[9];
    union
    {
        uint32_t pairs_4bpp[256];
        uint32_t row_pair[VGA_FILL_ROW_CELLS][4];
    };
} vga_fill_cache_t;

/* Forgets a row of text, so the next line builds its pairs again. */
static inline void vga_fill_cache_forget_row(vga_fill_cache_t *cache)
{
    if (cache->key[0] >= VGA_FILL_MODE0_ROW && cache->key[0] <= VGA_FILL_MODE1_16BPP)
        cache->key[0] = VGA_FILL_NONE;
}

/* A fill renderer is handed the options word its XREG call gave, which the
 * scanline table keeps beside it. */
typedef bool (*vga_fill_fn_t)(uint16_t options, int16_t scanline, int16_t width,
                              uint16_t *rgb, uint16_t config_ptr,
                              vga_fill_cache_t *cache);

/* Book scanlines for a mode, by its number and the options word of its XREG
 * call. A machine that rasterizes in software finds the renderer they name
 * and refuses a booking that names none. One that rasterizes in the FPGA
 * writes them to its scanline table. */
bool vga_prog_fill(int16_t plane, int16_t scanline_begin, int16_t scanline_end,
                   uint16_t config_ptr, uint8_t mode, uint16_t options);

/* Books like vga_prog_fill, but is allowed on the console canvas, and first
 * clears the rows its mode's last booking covered, so a mode that
 * re-programs itself over a different range leaves nothing behind. */
bool vga_prog_exclusive(int16_t plane, int16_t scanline_begin, int16_t scanline_end,
                        uint16_t config_ptr, uint8_t mode, uint16_t options);

bool vga_prog_sprite(int16_t plane, int16_t scanline_begin, int16_t scanline_end,
                     uint16_t config_ptr, uint16_t length,
                     uint8_t mode, uint16_t options);

#endif /* _CORE_VGA_VGA_H_ */
