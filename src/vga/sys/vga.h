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
    void (*fill320[PICO_SCANVIDEO_PLANE_COUNT])(void *ctx, int16_t scanline, uint16_t *rgb);
    void (*fill640[PICO_SCANVIDEO_PLANE_COUNT])(void *ctx, int16_t scanline, uint16_t *rgb);
    void *fill_ctx[PICO_SCANVIDEO_PLANE_COUNT];
    void (*sprite320[PICO_SCANVIDEO_PLANE_COUNT])(void *ctx, int16_t scanline, uint16_t *rgb);
    void (*sprite640[PICO_SCANVIDEO_PLANE_COUNT])(void *ctx, int16_t scanline, uint16_t *rgb);
    void *sprite_ctx[PICO_SCANVIDEO_PLANE_COUNT];
} vga_prog_t;
extern vga_prog_t vga_prog[VGA_PROG_MAX];

// Display type. Choose SD for 4:3 displays,
// HD for 16:9 displays, and SXGA for 5:4 displays.
// Note that choosing vga_hd will only activate 720p
// output on 320x180 and 640x380 resolutions.
typedef enum
{
    vga_sd,   // 640x480 (480p) default
    vga_hd,   // 1280x720 (720p)
    vga_sxga, // 1280x1024 (5:4)
} vga_display_t;

// Internal resolution, before scaling for display.
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
bool vga_xreg_mode(uint16_t *xregs);
uint16_t vga_height(void);
void vga_init(void);
void vga_task(void);

#endif /* _VGA_H_ */
