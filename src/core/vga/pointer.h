/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_VGA_POINTER_H_
#define _CORE_VGA_POINTER_H_

/* The pointer the system draws for the tablet, on top of every plane, with
 * its hot spot on the pointer's place. Shape n is sprite n - 1 of the
 * program's pointers, mode 6's array of mode 5 custom sprites in XRAM, each
 * one's x and y its hot spot. With none, it is one of the tablet's cursor
 * shapes, white with a black outline, at 640-canvas size or, on a canvas
 * 320 wide, at half that: custom sprites too, which pointer.py makes. Only
 * the system shape showing is kept where the renderers read it;
 * pointer_set_shape copies it there when the shape changes. */

#include "core/sys/sst.h"
#include <stdbool.h>
#include <stdint.h>

/* The pointer's shape, 0 for none. */
void pointer_set_shape(uint8_t shape);

/* The pointer's hot spot, at x,y in canvas pixels. */
void pointer_move(int16_t x, int16_t y);

/* Both at once. The scanlines drawn after either show it. */
void pointer_set(uint8_t shape, int16_t x, int16_t y);

/* The XRAM address of the program's pointers, or $FFFF for the system's. */
void pointer_set_custom(uint16_t config);

/* Whether the program's pointers are drawn, not the system's. */
bool pointer_custom(void);

/* Draws the pointer's part of scanline y, width pixels wide, into pixels,
 * the topmost plane's line, with the alpha bit set, so it shows over every
 * plane below. A line no plane has filled yet is cleared first, and filled
 * says so. Nothing is drawn on a line the pointer is not on, nor for a
 * shape there is no sprite for. */
void pointer_render(int16_t y, int16_t width, uint16_t *pixels, bool *filled);

void pointer_init(void);

/* The savestate carries the program's pointers. The shape and the place are
 * the tablet's, which sets them again after a load. POINTER_DRIVER follows
 * CANVAS_DRIVER, whose load can go back to the console. */
extern const sst_table_t pointer_sst;

#define POINTER_DRIVER DRIVER(                                 \
    SST("PNTR", pointer))

#endif /* _CORE_VGA_POINTER_H_ */
