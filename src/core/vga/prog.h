/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_VGA_PROG_H_
#define _CORE_VGA_PROG_H_

#include "core/vga/pixel_format.h"
#include "core/vga/vga.h"
#include <stdbool.h>
#include <stdint.h>

typedef bool (*vga_fill_fn_t)(int16_t plane_id, int16_t scanline, int16_t width,
                              uint16_t *rgb, uint16_t config_ptr);
typedef void (*vga_sprite_fn_t)(int16_t scanline, int16_t width, uint16_t *rgb,
                                uint16_t config_ptr, uint16_t length);

typedef struct
{
    vga_fill_fn_t fill_fn[SCANVIDEO_PLANE_COUNT];
    uint16_t fill_config[SCANVIDEO_PLANE_COUNT];
    vga_sprite_fn_t sprite_fn[SCANVIDEO_PLANE_COUNT];
    uint16_t sprite_config[SCANVIDEO_PLANE_COUNT];
    uint16_t sprite_length[SCANVIDEO_PLANE_COUNT];
} vga_prog_t;

const vga_prog_t *vga_prog_row(int16_t scanline);
void vga_prog_reset(void);

void vga_prog_load_row(int16_t scanline, const vga_prog_t *row);
void vga_prog_set_highest(int16_t scanline);

#endif /* _CORE_VGA_PROG_H_ */
