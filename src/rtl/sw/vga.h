/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FPGA_SW_VGA_H_
#define _FPGA_SW_VGA_H_

#include <stdint.h>
#include <stdbool.h>

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

void vga_prog_mode(uint8_t mode, uint16_t attr);
bool vga_prog_valid(int16_t plane, int16_t scanline_begin,
                    int16_t *scanline_end);
bool vga_prog_exclusive(int16_t plane, int16_t scanline_begin,
                        int16_t scanline_end, uint16_t config_ptr);
bool vga_set_canvas(uint16_t canvas);
void vga_restore(void);

#endif
