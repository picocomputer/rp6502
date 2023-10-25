/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_H_
#define _VGA_H_

#include <stdbool.h>
#include <stdint.h>

// Display type. Choose SD for 4:3 displays,
// HD for 16:9 displays, and SXGA for 5:4 displays.
typedef enum
{
    vga_sd,   // 640x480 (480p) default
    vga_hd,   // 640x480 and 1280x720 (720p)
    vga_sxga, // 1280x1024 (5:4)
} vga_display_t;

// Canvas size.
typedef enum
{
    vga_console,
    vga_320_240,
    vga_320_180,
    vga_640_480,
    vga_640_360,
} vga_canvas_t;

void vga_set_display(vga_display_t display);
bool vga_xreg_canvas(uint16_t *xregs);
int16_t vga_canvas_height(void);
void vga_init(void);
void vga_task(void);

bool vga_prog_fill(int16_t plane, int16_t scanline_begin, int16_t scanline_end,
                   uint16_t config_ptr,
                   bool (*fill_fn)(int16_t scanline,
                                   int16_t width,
                                   uint16_t *rgb,
                                   uint16_t config_ptr));

// For singleton fill modes, like the terminal
bool vga_prog_exclusive(int16_t plane, int16_t scanline_begin, int16_t scanline_end,
                        uint16_t config_ptr,
                        bool (*fill_fn)(int16_t scanline,
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

#endif /* _VGA_H_ */
