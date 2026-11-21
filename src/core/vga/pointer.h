/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_VGA_POINTER_H_
#define _CORE_VGA_POINTER_H_

/* The pointer the system draws for the tablet, on top of every plane: one of
 * the tablet's cursor shapes, white with a black outline, at 640-canvas size
 * or, on a canvas 320 wide, at half that. Only the shape showing is kept
 * where the renderers read it; pointer_set copies it there when the shape
 * changes, from the shapes pointer.py makes. */

#include <stdbool.h>
#include <stdint.h>

/* The pointer's shape, 0 for none. */
void pointer_set_shape(uint8_t shape);

/* The pointer's hot spot, at x,y in canvas pixels. */
void pointer_move(int16_t x, int16_t y);

/* Both at once. The scanlines drawn after either show it. */
void pointer_set(uint8_t shape, int16_t x, int16_t y);

/* Draws the pointer's part of scanline y, width pixels wide, into pixels,
 * the topmost plane's line, with the alpha bit set, so it shows over every
 * plane below. A line no plane has filled yet is cleared first, and filled
 * says so. Nothing is drawn on a line the pointer is not on. */
void pointer_render(int16_t y, int16_t width, uint16_t *pixels, bool *filled);

void pointer_init(void);

#endif /* _CORE_VGA_POINTER_H_ */
