/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_VGA_MODE6_H_
#define _CORE_VGA_MODE6_H_

/* Mode 6, the pointer the system draws for the tablet, on top of every
 * plane, with its hot spot on the pointer's place.
 *
 * A program gives it its own shapes with CONFIG, the XRAM address of an
 * array of mode 5 custom sprites: shape n draws sprite n - 1, its x and y
 * the hot spot. $FFFF puts back the system's shapes, the tablet's cursor
 * shapes, white with a black outline, at 640-canvas size or, on a canvas
 * 320 wide, at half that: custom sprites too, which mode6.py makes. Only
 * the system shape showing is kept where the renderers read it;
 * mode6_set_shape copies it there when the shape changes.
 *
 * Mode 6 is not a plane, so it outlives a change between the program's
 * canvases; going back to the console puts the system's shapes back. A
 * fabric build takes CONFIG (vga_prog_pointer) and draws in the fabric. */

#include "core/sys/sst.h"
#include <stdbool.h>
#include <stdint.h>

bool mode6_prog(uint16_t *xregs);

#ifndef RP6502_VGA_FABRIC
/* The pointer's shape, 0 for none. */
void mode6_set_shape(uint8_t shape);

/* The pointer's hot spot, at x,y in canvas pixels. */
void mode6_move(int16_t x, int16_t y);

/* Both at once. The scanlines drawn after either show it. */
void mode6_set(uint8_t shape, int16_t x, int16_t y);

/* Whether the program's shapes are drawn, not the system's. */
bool mode6_custom(void);

/* Draws the pointer's part of scanline y, width pixels wide, into pixels,
 * the topmost plane's line, with the alpha bit set, so it shows over every
 * plane below. A line no plane has filled yet is cleared first, and filled
 * says so. Nothing is drawn on a line the pointer is not on, nor for a
 * shape there is no sprite for. */
void mode6_render(int16_t y, int16_t width, uint16_t *pixels, bool *filled);

void mode6_init(void);

/* The savestate carries the program's shapes. The shape and the place are
 * the tablet's, which sets them again after a load. MODE6_DRIVER follows
 * CANVAS_DRIVER, whose load can go back to the console. */
extern const sst_table_t mode6_sst;

#define MODE6_DRIVER DRIVER( \
    SST("MOD6", mode6))
#endif

#endif /* _CORE_VGA_MODE6_H_ */
