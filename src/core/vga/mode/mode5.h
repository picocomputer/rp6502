/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_VGA_MODE5_H_
#define _CORE_VGA_MODE5_H_

#include "core/vga/prog.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* A custom sprite, as MODE5_CUSTOM's sprite array holds them in XRAM. */
typedef struct
{
    int16_t x_pos_px;
    int16_t y_pos_px;
    uint16_t xram_sprite_ptr;
    uint16_t palette_ptr;
    uint8_t width_height;
    uint8_t options;
} mode5_csprite_t;

_Static_assert(sizeof(mode5_csprite_t) == 10, "a custom descriptor is ten bytes");
_Static_assert(offsetof(mode5_csprite_t, width_height) == 8 &&
                   offsetof(mode5_csprite_t, options) == 9,
               "the size and options bytes follow the palette pointer");

bool mode5_prog(uint16_t *xregs);

/* The bytes of a custom sprite's image, from its size and options bytes. */
uint32_t mode5_custom_bytes(uint8_t width_height, uint8_t options);

/* Whether this mode defines that attribute. mode5_prog asks this rather than
 * asking for the renderer, because a fabric build has no renderer to name. */
bool mode5_sprite_valid(uint16_t attributes);

/* The length sprites of a valid attribute at config_ptr are on a word and
 * inside xram. */
bool mode5_config_ok(uint16_t attributes, uint16_t config_ptr, uint16_t length);

/* mode5_sprite_fn returns the renderer an attribute names, and
 * mode5_sprite_attr finds the attribute that names a renderer. A savestate
 * stores the attribute rather than the function address, because the function
 * can be at a different address in the build that loads it. A fabric build
 * has no renderers. */
#ifndef RP6502_VGA_FABRIC
vga_sprite_fn_t mode5_sprite_fn(uint16_t attributes);
bool mode5_sprite_attr(vga_sprite_fn_t fn, uint16_t *attributes);

/* The palette of a custom sprite: the one at palette_ptr in XRAM, or the
 * built-in one for the depth in options. */
const uint16_t *mode5_palette(uint16_t palette_ptr, uint8_t options);

/* Line tex_y of a custom sprite, counted down the sprite as drawn, into
 * rgb, a scanline width pixels wide, with the sprite's left edge at x: the
 * sprite of those size and options bytes, its image and palette already
 * found. Mode 6 draws the pointer with it too. */
void mode5_custom_line(int x, int tex_y, uint8_t width_height, uint8_t options,
                       const uint8_t *image, const uint16_t *palette,
                       int16_t width, uint16_t *rgb);
#endif

#endif /* _CORE_VGA_MODE5_H_ */
