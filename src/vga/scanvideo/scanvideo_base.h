/*
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 * Copyright (c) 2025 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_SCANVIDEO_SCANVIDEO_BASE_H_
#define _VGA_SCANVIDEO_SCANVIDEO_BASE_H_

#include "pico/types.h"

#if !PICO_NO_HARDWARE

#include "hardware/pio.h"

#endif

#ifdef __cplusplus
extern "C"
{
#endif

/** \file scanvideo_base.h
 *  \defgroup pico_scanvideo pico_scanvideo
 *
 * Common Scan-out Video API
 */
// == CONFIG ============
#ifndef PICO_SCANVIDEO_PLANE_COUNT
#define PICO_SCANVIDEO_PLANE_COUNT 1
#endif

#ifndef PICO_SCANVIDEO_SCANLINE_BUFFER_COUNT
#define PICO_SCANVIDEO_SCANLINE_BUFFER_COUNT 8
#endif

#ifndef PICO_SCANVIDEO_COLOR_PIN_BASE
#define PICO_SCANVIDEO_COLOR_PIN_BASE 0
#endif

#ifndef PICO_SCANVIDEO_COLOR_PIN_COUNT
#define PICO_SCANVIDEO_COLOR_PIN_COUNT 16
#endif

#ifndef PICO_SCANVIDEO_SYNC_PIN_BASE
#define PICO_SCANVIDEO_SYNC_PIN_BASE (PICO_SCANVIDEO_COLOR_PIN_BASE + PICO_SCANVIDEO_COLOR_PIN_COUNT)
#endif

#ifndef PICO_SCANVIDEO_MAX_SCANLINE_BUFFER_WORDS
#define PICO_SCANVIDEO_MAX_SCANLINE_BUFFER_WORDS 180
#endif
#ifndef PICO_SCANVIDEO_MAX_SCANLINE_BUFFER2_WORDS
#define PICO_SCANVIDEO_MAX_SCANLINE_BUFFER2_WORDS PICO_SCANVIDEO_MAX_SCANLINE_BUFFER_WORDS
#endif
#ifndef PICO_SCANVIDEO_MAX_SCANLINE_BUFFER3_WORDS
#define PICO_SCANVIDEO_MAX_SCANLINE_BUFFER3_WORDS PICO_SCANVIDEO_MAX_SCANLINE_BUFFER_WORDS
#endif

    // ======================

#define BPP 16

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
    } scanvideo_mode_t;

    extern bool scanvideo_setup(const scanvideo_mode_t *mode);
    extern void scanvideo_teardown(void);
    extern void scanvideo_timing_enable(bool enable);

    // --- scanline management ---

    typedef struct scanvideo_scanline_buffer
    {
        uint32_t scanline_id;
        uint32_t *data;
        uint16_t data_used;
        uint16_t data_max;
#if PICO_SCANVIDEO_PLANE_COUNT > 1
        uint32_t *data2;
        uint16_t data2_used;
        uint16_t data2_max;
#if PICO_SCANVIDEO_PLANE_COUNT > 2
        uint32_t *data3;
        uint16_t data3_used;
        uint16_t data3_max;
#endif
#endif
        void *user_data;
        uint8_t status;
    } scanvideo_scanline_buffer_t;

    enum
    {
        SCANLINE_OK = 1,
        SCANLINE_ERROR,
        SCANLINE_SKIPPED
    };

    static inline uint16_t scanvideo_frame_number(uint32_t scanline_id)
    {
        return (uint16_t)(scanline_id >> 16u);
    }

    static inline uint16_t scanvideo_scanline_number(uint32_t scanline_id)
    {
        return (uint16_t)scanline_id;
    }

    extern bool scanvideo_vsync_pausing();

    scanvideo_scanline_buffer_t *scanvideo_begin_scanline_generation(bool block);

    void scanvideo_end_scanline_generation(scanvideo_scanline_buffer_t *scanline_buffer);

    // mode implementation

    struct scanvideo_pio_program
    {
#if !PICO_NO_HARDWARE
        const pio_program_t *program;
        const uint8_t entry_point;
        bool (*adapt_for_mode)(const scanvideo_pio_program_t *program, const scanvideo_mode_t *mode,
                               scanvideo_scanline_buffer_t *missing_scanline_buffer, uint16_t *modifiable_instructions);
        pio_sm_config (*configure_pio)(pio_hw_t *pio, uint sm, uint offset);
#else
    const char *id;
#endif
    };

    extern const scanvideo_pio_program_t video_24mhz_composable;

#ifndef PICO_SPINLOCK_ID_VIDEO_SCANLINE_LOCK
#define PICO_SPINLOCK_ID_VIDEO_SCANLINE_LOCK 2
#endif

#ifndef PICO_SPINLOCK_ID_VIDEO_FREE_LIST_LOCK
#define PICO_SPINLOCK_ID_VIDEO_FREE_LIST_LOCK 3
#endif

#ifndef PICO_SPINLOCK_ID_VIDEO_DMA_LOCK
#define PICO_SPINLOCK_ID_VIDEO_DMA_LOCK 4
#endif

#ifndef PICO_SPINLOCK_ID_VIDEO_IN_USE_LOCK
#define PICO_SPINLOCK_ID_VIDEO_IN_USE_LOCK 5
#endif

// note this is not necessarily an absolute gpio pin mask, it is still shifted by PICO_SCANVIDEO_COLOR_PIN_BASE
#define PICO_SCANVIDEO_ALPHA_MASK (1u << PICO_SCANVIDEO_ALPHA_PIN)

#ifndef PICO_SCANVIDEO_PIXEL_FROM_RGB8
#define PICO_SCANVIDEO_PIXEL_FROM_RGB8(r, g, b) ((((b) >> 3u) << PICO_SCANVIDEO_PIXEL_BSHIFT) | (((g) >> 3u) << PICO_SCANVIDEO_PIXEL_GSHIFT) | (((r) >> 3u) << PICO_SCANVIDEO_PIXEL_RSHIFT))
#endif

#ifndef PICO_SCANVIDEO_PIXEL_FROM_RGB5
#define PICO_SCANVIDEO_PIXEL_FROM_RGB5(r, g, b) (((b) << PICO_SCANVIDEO_PIXEL_BSHIFT) | ((g) << PICO_SCANVIDEO_PIXEL_GSHIFT) | ((r) << PICO_SCANVIDEO_PIXEL_RSHIFT))
#endif

#ifndef PICO_SCANVIDEO_R5_FROM_PIXEL
#define PICO_SCANVIDEO_R5_FROM_PIXEL(p) (((p) >> PICO_SCANVIDEO_PIXEL_RSHIFT) & 0x1f)
#endif

#ifndef PICO_SCANVIDEO_G5_FROM_PIXEL
#define PICO_SCANVIDEO_G5_FROM_PIXEL(p) (((p) >> PICO_SCANVIDEO_PIXEL_GSHIFT) & 0x1f)
#endif

#ifndef PICO_SCANVIDEO_B5_FROM_PIXEL
#define PICO_SCANVIDEO_B5_FROM_PIXEL(p) (((p) >> PICO_SCANVIDEO_PIXEL_BSHIFT) & 0x1f)
#endif

#ifdef __cplusplus
}
#endif

#endif /* _VGA_SCANVIDEO_SCANVIDEO_BASE_H_ */
