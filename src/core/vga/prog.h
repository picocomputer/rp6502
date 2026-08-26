/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The scanline program, for a machine that keeps one in memory: which
 * renderer runs on which plane of which line, and what it reads. Both the
 * emulator and the VGA firmware book scanlines this way and then walk the
 * same table to draw, so the booking is written once here.
 *
 * A machine whose scanline program lives in fabric registers does not link
 * this -- it has no table to keep -- and answers vga_prog_fill and its
 * siblings by writing the fabric instead. */

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

/* What line `scanline` is programmed to draw. The renderer's inner loop. */
const vga_prog_t *vga_prog_row(int16_t scanline);

/* Forget all programming: a canvas change, or a machine stopping. */
void vga_prog_reset(void);

/* The last line any program renders, which is where vsync fires. Zero when
 * nothing is programmed. */
int16_t vga_prog_highest(void);

/* Bound a booking against the canvas and this plane, resolving a zero end to
 * "the bottom", and track the highest line. A machine whose program lives in
 * fabric registers bounds its own, because the numbers it bounds against are
 * the fabric's -- see the note above about not linking this file. */
bool vga_prog_valid(int16_t plane, int16_t scanline_begin, int16_t *scanline_end);

#endif /* _CORE_VGA_PROG_H_ */
