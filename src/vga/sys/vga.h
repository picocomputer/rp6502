/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_H_
#define _VGA_H_

#include <stdbool.h>
#include <stdint.h>

#define VGA_PROG_MAX 512
typedef struct
{
    bool (*fill[PICO_SCANVIDEO_PLANE_COUNT])(int16_t scanline_id,
                                             int16_t width,
                                             uint16_t *rgb,
                                             void *config);
    void *fill_config[PICO_SCANVIDEO_PLANE_COUNT];

    void (*sprite[PICO_SCANVIDEO_PLANE_COUNT])(int16_t scanline_id,
                                               int16_t width,
                                               uint16_t *rgb,
                                               void *config,
                                               uint16_t count);
    void *sprite_config[PICO_SCANVIDEO_PLANE_COUNT];
    uint16_t sprite_count[PICO_SCANVIDEO_PLANE_COUNT];
} vga_prog_t;
extern vga_prog_t vga_prog[VGA_PROG_MAX];

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
uint16_t vga_height(void);
void vga_init(void);
void vga_task(void);

#endif /* _VGA_H_ */
