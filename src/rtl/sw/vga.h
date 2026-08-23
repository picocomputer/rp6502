/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_VGA_H_
#define _FPGA_SW_VGA_H_

#include <stdint.h>
#include <stdbool.h>

/* Both sides of the VGA contract on one machine, the emu/sys/vga.h
 * shape. Self-contained because ria/sys/vga.h and vga/sys/vga.h each
 * define their own canvas enum; these stay ABI-compatible with both.
 */

typedef enum
{
    vga_canvas_console = 0,
    vga_canvas_320_240,
    vga_canvas_320_180,
    vga_canvas_640_480,
    vga_canvas_640_360,
} vga_canvas_t;

bool vga_connected(void);
vga_canvas_t vga_get_canvas(void);
uint8_t vga_get_display_type(void);
int16_t vga_canvas_height(void);
int16_t vga_vsync_scanline(void);

/* The mode dispatch announces what the next vga_prog_fill publishes —
 * a fill-function pointer means nothing to hardware. */
void vga_prog_mode(uint8_t mode, uint16_t attr);
bool vga_prog_valid(int16_t plane, int16_t scanline_begin,
                    int16_t *scanline_end);
/* Mode 0's registration: one instance globally — the previous entries
 * are swept wherever they survive, contiguous or not. */
/* fill_fn is the renderer itself on a machine that renders in software.
 * Here the fabric does, told which mode by vga_prog_mode before the call,
 * so it is ignored -- but it stays in the signature because core/vga/mode0.c
 * passes it and term.c drags the five-parameter declaration into this same
 * link through vga/sys/vga.h. */
bool vga_prog_exclusive(int16_t plane, int16_t scanline_begin,
                        int16_t scanline_end, uint16_t config_ptr,
                        bool (*fill_fn)(int16_t, int16_t, int16_t,
                                        uint16_t *, uint16_t));
bool vga_set_canvas(uint16_t canvas);
/* The two raster registers a blob cannot carry, put back from the
 * shadows it did. */
void vga_restore(void);

#endif /* _FPGA_SW_VGA_H_ */
