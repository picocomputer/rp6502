/*
 * Copyright (c) 2022 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_H_
#define _VGA_H_

#include <stdbool.h>

// The Picocomputer only supports 60Hz VGA and HDMI video.
// (There already exists 6502 hardware for 15kHz video.)
// Inexpensive VGA to HDMI converters will work perfectly
// on all resolutions without any framebuffer lag.

// By default, only SD output with letterboxing is selected.
// If you're using a wide monitor and don't want windowboxing
// on the wide resolutions, enable HD output.
// If you have a 1280x1024 SXGA panel that stretches everything
// to 5:4 (all of them), you're welcome.

// Display resolution. Note that choosing vga_hd will only
// activate 720p output on 320x180 and 640x380 resolutions.
typedef enum
{
    vga_sd, // 640x480 (480p)
    vga_hd, // 1280x720 (720p)
    vga_sxga, // 1280x1024 (5:4)
} vga_display_t;

// Integer scaled to display resolution and vertically centered
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
