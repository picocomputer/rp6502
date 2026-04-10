/*
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_SCANVIDEO_SCANVIDEO_H_
#define _VGA_SCANVIDEO_SCANVIDEO_H_

#include "pico/types.h"
#include "hardware/pio.h"
#include "scanvideo.pio.h"

// == CONFIG ============
#define PICO_SCANVIDEO_PLANE_COUNT 3
#define PICO_SCANVIDEO_SCANLINE_BUFFER_COUNT 10
#define PICO_SCANVIDEO_MAX_SCANLINE_BUFFER_WORDS 323
#define PICO_SCANVIDEO_COLOR_PIN_BASE 6
#define PICO_SCANVIDEO_COLOR_PIN_COUNT 16
#define PICO_SCANVIDEO_SYNC_PIN_BASE 26
#define PICO_SPINLOCK_ID_VIDEO_SCANLINE_LOCK 2
#define PICO_SPINLOCK_ID_VIDEO_FREE_LIST_LOCK 3
#define PICO_SPINLOCK_ID_VIDEO_DMA_LOCK 4
#define PICO_SPINLOCK_ID_VIDEO_IN_USE_LOCK 5

// == PIXEL FORMAT ======
#define BPP 16
#define PICO_SCANVIDEO_ALPHA_PIN 5u
#define PICO_SCANVIDEO_PIXEL_RSHIFT 0u
#define PICO_SCANVIDEO_PIXEL_GSHIFT 6u
#define PICO_SCANVIDEO_PIXEL_BSHIFT 11u
#define PICO_SCANVIDEO_PIXEL_RCOUNT 5
#define PICO_SCANVIDEO_PIXEL_GCOUNT 5
#define PICO_SCANVIDEO_PIXEL_BCOUNT 5
#define PICO_SCANVIDEO_ALPHA_MASK (1u << PICO_SCANVIDEO_ALPHA_PIN)

#define PICO_SCANVIDEO_PIXEL_FROM_RGB8(r, g, b) ((((b) >> 3u) << PICO_SCANVIDEO_PIXEL_BSHIFT) | (((g) >> 3u) << PICO_SCANVIDEO_PIXEL_GSHIFT) | (((r) >> 3u) << PICO_SCANVIDEO_PIXEL_RSHIFT))
#define PICO_SCANVIDEO_PIXEL_FROM_RGB5(r, g, b) (((b) << PICO_SCANVIDEO_PIXEL_BSHIFT) | ((g) << PICO_SCANVIDEO_PIXEL_GSHIFT) | ((r) << PICO_SCANVIDEO_PIXEL_RSHIFT))
#define PICO_SCANVIDEO_R5_FROM_PIXEL(p) (((p) >> PICO_SCANVIDEO_PIXEL_RSHIFT) & 0x1f)
#define PICO_SCANVIDEO_G5_FROM_PIXEL(p) (((p) >> PICO_SCANVIDEO_PIXEL_GSHIFT) & 0x1f)
#define PICO_SCANVIDEO_B5_FROM_PIXEL(p) (((p) >> PICO_SCANVIDEO_PIXEL_BSHIFT) & 0x1f)

// == COMPOSABLE SCANLINE ==
#define video_24mhz_composable_prefix video_24mhz_composable_default
#define __EXTRA_CONCAT(x, y) __CONCAT(x, y)
#define video_24mhz_composable_program_extern(x) __EXTRA_CONCAT(__EXTRA_CONCAT(video_24mhz_composable_prefix, _offset_), x)
#define __DVP_JMP(x) ((unsigned)video_24mhz_composable_program_extern(x))
#define COMPOSABLE_COLOR_RUN __DVP_JMP(color_run)
#define COMPOSABLE_EOL_ALIGN __DVP_JMP(end_of_scanline_ALIGN)
#define COMPOSABLE_EOL_SKIP_ALIGN __DVP_JMP(end_of_scanline_skip_ALIGN)
#define COMPOSABLE_RAW_RUN __DVP_JMP(raw_run)
#define COMPOSABLE_RAW_1P __DVP_JMP(raw_1p)
#define COMPOSABLE_RAW_2P __DVP_JMP(raw_2p)
#define COMPOSABLE_RAW_1P_SKIP_ALIGN __DVP_JMP(raw_1p_skip_ALIGN)

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

typedef struct scanvideo_pio_program scanvideo_pio_program_t;

typedef struct scanvideo_mode
{
    const scanvideo_timing_t *default_timing;
    const scanvideo_pio_program_t *pio_program;

    uint16_t width;
    uint16_t height;
    uint8_t xscale;
    uint16_t yscale;
    uint16_t yscale_denominator;
    uint16_t v_offset;
} scanvideo_mode_t;

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

enum
{
    SCANLINE_OK = 1,
    SCANLINE_ERROR,
    SCANLINE_SKIPPED
};

struct scanvideo_pio_program
{
    const pio_program_t *program;
    const uint8_t entry_point;
    bool (*adapt_for_mode)(const scanvideo_pio_program_t *program, const scanvideo_mode_t *mode,
                           scanvideo_scanline_buffer_t *missing_scanline_buffer, uint16_t *modifiable_instructions);
    pio_sm_config (*configure_pio)(pio_hw_t *pio, uint sm, uint offset);
};

// == API ===============

extern const scanvideo_pio_program_t video_24mhz_composable;

extern void scanvideo_set_mode(const scanvideo_mode_t *mode);

scanvideo_scanline_buffer_t *scanvideo_begin_scanline_generation(void);
void scanvideo_end_scanline_generation(scanvideo_scanline_buffer_t *scanline_buffer);

static inline uint16_t scanvideo_frame_number(uint32_t scanline_id)
{
    return (uint16_t)(scanline_id >> 16u);
}

static inline uint16_t scanvideo_scanline_number(uint32_t scanline_id)
{
    return (uint16_t)scanline_id;
}

#endif /* _VGA_SCANVIDEO_SCANVIDEO_H_ */
