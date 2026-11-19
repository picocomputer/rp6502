/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_VGA_PROG_H_
#define _CORE_VGA_PROG_H_

#include "core/sys/sst.h"
#include "core/vga/pixel_format.h"
#include "core/vga/vga.h"
#include <stdbool.h>
#include <stdint.h>

typedef void (*vga_sprite_fn_t)(int16_t scanline, int16_t width, uint16_t *rgb,
                                uint16_t config_ptr, uint16_t length);

/* A row of the scanline table: for each plane, what fills it and what draws
 * sprites over it. A fill has the mode and the options word that were
 * booked, which a savestate carries, and the renderer they name, which a
 * scanline calls. The mode means something only where there is a renderer. */
typedef struct
{
    vga_fill_fn_t fill_fn[SCANVIDEO_PLANE_COUNT];
    uint16_t fill_config[SCANVIDEO_PLANE_COUNT];
    uint16_t fill_options[SCANVIDEO_PLANE_COUNT];
    vga_sprite_fn_t sprite_fn[SCANVIDEO_PLANE_COUNT];
    uint16_t sprite_config[SCANVIDEO_PLANE_COUNT];
    uint16_t sprite_length[SCANVIDEO_PLANE_COUNT];
    uint8_t fill_mode[SCANVIDEO_PLANE_COUNT];
} vga_prog_t;

/* The renderer a mode and its options name, or NULL for none. */
vga_fill_fn_t vga_mode_fill_fn(uint8_t mode, uint16_t options);
vga_sprite_fn_t vga_mode_sprite_fn(uint8_t mode, uint16_t options);

/* A config of size bytes at config_ptr is on a word and inside xram. */
static inline bool vga_xram_ok(uint16_t config_ptr, uint32_t size)
{
    return !(config_ptr & 1) && size <= 0x10000 && config_ptr <= 0x10000 - size;
}

/* Whether a row's config is one its mode's prog books, so the renderer reads
 * only xram. length is a sprite mode's count of sprites. */
bool vga_mode_config_ok(uint8_t mode, uint16_t options, uint16_t config_ptr,
                        uint16_t length);

/* Puts a fill in a row a savestate is loading. False for a mode and options
 * that name no renderer. */
bool vga_prog_row_fill(vga_prog_t *row, int16_t plane, uint8_t mode,
                       uint16_t options, uint16_t config_ptr);

/* The mode and options that name a sprite renderer, VGA_MODE_NONE and 0 for
 * none. False for a renderer no mode names. */
#define VGA_MODE_NONE 0xFF
bool vga_mode_sprite_id(vga_sprite_fn_t fn, uint8_t *mode, uint16_t *attributes);

const vga_prog_t *vga_prog_row(int16_t scanline);
void vga_prog_reset(void);

/* The savestate carries the highest scanline booked and each row's
 * renderers by their modes and options, which name them in every build.
 * Rows at or past 480 are never booked, because every booking is bounded
 * against the canvas and no canvas the emulator has is taller than that. */
extern const sst_table_t prog_sst;

#define PROG_DRIVER DRIVER(                                    \
    SST("PROG", prog))

#endif /* _CORE_VGA_PROG_H_ */
