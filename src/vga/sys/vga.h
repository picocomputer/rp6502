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
    vga_320_240,
    vga_640_480,
    vga_320_180,
    vga_640_360,
} vga_resolution_t;

void vga_init(void);
void vga_task(void);
void vga_display(vga_display_t display);
void vga_resolution(vga_resolution_t mode);
void vga_terminal(bool show);

#endif /* _VGA_H_ */
