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
#include "scanvideo.pio.h"

// == CONFIG ============
#define SCANVIDEO_PLANE_COUNT 3

// == PIXEL FORMAT ======
#define SCANVIDEO_ALPHA_PIN 5u
#define SCANVIDEO_ALPHA_MASK (1u << SCANVIDEO_ALPHA_PIN)
#define SCANVIDEO_PIXEL_RSHIFT 0u
#define SCANVIDEO_PIXEL_GSHIFT 6u
#define SCANVIDEO_PIXEL_BSHIFT 11u
#define SCANVIDEO_PIXEL_RCOUNT 5u
#define SCANVIDEO_PIXEL_GCOUNT 5u
#define SCANVIDEO_PIXEL_BCOUNT 5u
#define SCANVIDEO_PIXEL_FROM_RGB8(r, g, b) ((((b) >> 3u) << SCANVIDEO_PIXEL_BSHIFT) | (((g) >> 3u) << SCANVIDEO_PIXEL_GSHIFT) | (((r) >> 3u) << SCANVIDEO_PIXEL_RSHIFT))
#define SCANVIDEO_PIXEL_FROM_RGB5(r, g, b) (((b) << SCANVIDEO_PIXEL_BSHIFT) | ((g) << SCANVIDEO_PIXEL_GSHIFT) | ((r) << SCANVIDEO_PIXEL_RSHIFT))
#define SCANVIDEO_R5_FROM_PIXEL(p) (((p) >> SCANVIDEO_PIXEL_RSHIFT) & 0x1f)
#define SCANVIDEO_G5_FROM_PIXEL(p) (((p) >> SCANVIDEO_PIXEL_GSHIFT) & 0x1f)
#define SCANVIDEO_B5_FROM_PIXEL(p) (((p) >> SCANVIDEO_PIXEL_BSHIFT) & 0x1f)

// == COMPOSABLE SCANLINE ==
#define COMPOSABLE_COLOR_RUN ((unsigned)composable_offset_color_run)
#define COMPOSABLE_EOL_ALIGN ((unsigned)composable_offset_end_of_scanline_ALIGN)
#define COMPOSABLE_EOL_SKIP_ALIGN ((unsigned)composable_offset_end_of_scanline_skip_ALIGN)
#define COMPOSABLE_RAW_RUN ((unsigned)composable_offset_raw_run)
#define COMPOSABLE_RAW_1P ((unsigned)composable_offset_raw_1p)
#define COMPOSABLE_RAW_2P ((unsigned)composable_offset_raw_2p)
#define COMPOSABLE_RAW_1P_SKIP_ALIGN ((unsigned)composable_offset_raw_1p_skip_ALIGN)

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

    uint8_t clock_polarity;
} scanvideo_timing_t;

typedef struct scanvideo_view
{
    const scanvideo_timing_t *default_timing;

    uint16_t width;
    uint16_t height;
    uint8_t x_scale;
    uint16_t y_scale;
    uint16_t y_offset;
} scanvideo_view_t;

typedef struct scanvideo_scanline_buffer
{
    uint32_t scanline_id;
    uint32_t *data0;
    uint16_t data0_used;
    uint16_t data0_max;
    uint32_t *data1;
    uint16_t data1_used;
    uint16_t data1_max;
    uint32_t *data2;
    uint16_t data2_used;
    uint16_t data2_max;
    void *user_data;
    uint8_t status;
} scanvideo_scanline_buffer_t;

// == API ===============

extern void scanvideo_set_mode(const scanvideo_view_t *mode);

scanvideo_scanline_buffer_t *scanvideo_begin_scanline_generation(void);
void scanvideo_end_scanline_generation(scanvideo_scanline_buffer_t *scanline_buffer);

static inline uint16_t scanvideo_scanline_number(uint32_t scanline_id)
{
    return (uint16_t)scanline_id;
}

#endif /* _VGA_SCANVIDEO_SCANVIDEO_H_ */
