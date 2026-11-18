/*
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_SCANVIDEO_SCANVIDEO_H_
#define _VGA_SCANVIDEO_SCANVIDEO_H_

// This is a fork of pico_scanvideo_dpi from:
// https://github.com/raspberrypi/pico-extras
// It has been heavily modified for RP6502.

#include "pico/types.h"

#include "core/vga/pixel_format.h"

// == TYPES =============

typedef struct scanvideo_timing
{
    uint32_t clock_freq;

    uint16_t h_active;
    uint16_t v_active;

    uint16_t h_front_porch;
    uint16_t h_pulse;
    uint16_t h_total;
    uint8_t h_sync_polarity;

    uint16_t v_front_porch;
    uint16_t v_pulse;
    uint16_t v_total;
    uint8_t v_sync_polarity;
} scanvideo_timing_t;

typedef struct scanvideo_view
{
    const scanvideo_timing_t *default_timing;

    uint16_t width;
    uint16_t height;
    uint8_t x_scale;
    uint8_t y_scale;
    uint16_t y_offset;
} scanvideo_view_t;

// A line to draw: which line of which frame, and for each plane the pixels
// of the line, the view's width of them.
typedef struct scanvideo_scanline_buffer
{
    uint32_t scanline_id;
    uint16_t *pixels[SCANVIDEO_PLANE_COUNT];
} scanvideo_scanline_buffer_t;

// == API ===============

void scanvideo_set_mode(const scanvideo_view_t *mode);

// A line to draw, or NULL when there is none to draw yet. It is handed back
// with which planes had pixels written; a plane that had none is transparent.
scanvideo_scanline_buffer_t *scanvideo_begin_scanline_generation(void);
void scanvideo_end_scanline_generation(scanvideo_scanline_buffer_t *scanline_buffer,
                                       const bool filled[SCANVIDEO_PLANE_COUNT]);

static inline uint16_t scanvideo_scanline_number(uint32_t scanline_id)
{
    return (uint16_t)scanline_id;
}

static inline uint16_t scanvideo_frame_number(uint32_t scanline_id)
{
    return (uint16_t)(scanline_id >> 16u);
}

#endif /* _VGA_SCANVIDEO_SCANVIDEO_H_ */
