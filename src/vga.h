/*
 * Copyright (c) 2022 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_H_
#define _VGA_H_

#include <stdbool.h>

// The Picocomputer supports analog VGA output.
// Inexpensive VGA to HDMI converters work perfectly
// on all resolutions without any framebuffer lag.

// Display resolution. Note that choosing vga_hd will only
// activate 720p output on 320x180 and 640x380 resolutions.
// If you have a 1280x1024 SXGA panel that stretches everything
// to 5:4 (which is all of them), you're welcome.
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

void vga_display(vga_display_t display);
void vga_resolution(vga_resolution_t mode);
void vga_terminal(bool show);
void vga_init();
void vga_task();

#endif /* _VGA_H_ */
