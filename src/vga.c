/*
 * Copyright (c) 2022 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "vga.h"
#include "term.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/scanvideo.h"
#include "pico/scanvideo/composable_scanline.h"

// The Picocomputer is a 640x480 60Hz VGA system.
// Old 15 kHz CRT televisions and PVMs are not supported.
// We don't want to make resolution selection complicated but there
// are a couple of scenarios that seem worth configuration for.
//
//   1. Most 1280x1024 SXGA panels will stretch a 640x480 4:3 image to 5:4.
//      We provide a native resolution option to maintain square pixels.
//   2. 640x360 is commonly used in pixel art games intended for 16:9 displays.
//      We provide an optional 720p HD output to avoid windowboxing.

static const scanvideo_timing_t vga_timing_640x480_60_cea =
    {
        .clock_freq = 25200000,

        .h_active = 640,
        .v_active = 480,

        .h_front_porch = 16,
        .h_pulse = 96,
        .h_total = 800,
        .h_sync_polarity = 1,

        .v_front_porch = 10,
        .v_pulse = 2,
        .v_total = 525,
        .v_sync_polarity = 1,

        .enable_clock = 0,
        .clock_polarity = 0,

        .enable_den = 0};

static const scanvideo_mode_t vga_mode_640x480 =
    {
        .default_timing = &vga_timing_640x480_60_cea,
        .pio_program = &video_24mhz_composable,
        .width = 640,
        .height = 480,
        .xscale = 1,
        .yscale = 1};

static const scanvideo_timing_t vga_timing_1280x1024_60_dmt =
    {
        // half clock rate, effective 2 xscale
        .clock_freq = 54000000,

        .h_active = 640,
        .v_active = 960,

        .h_front_porch = 24,
        .h_pulse = 56,
        .h_total = 844,
        .h_sync_polarity = 0,

        // porch extended for letterbox effect (1024->960)
        .v_front_porch = 33,
        .v_pulse = 3,
        .v_total = 1066,
        .v_sync_polarity = 1,
};

static const scanvideo_mode_t vga_mode_1280x1024 =
    {
        .default_timing = &vga_timing_1280x1024_60_dmt,
        .pio_program = &video_24mhz_composable,
        .width = 640,
        .height = 480,
        .xscale = 1,
        .yscale = 2,
};

static const scanvideo_timing_t vga_timing_1280x720_60_cea =
    {
        .clock_freq = 37125000,

        .h_active = 640,
        .v_active = 720,

        .h_front_porch = 55,
        .h_pulse = 20,
        .h_total = 825,
        .h_sync_polarity = 1,

        .v_front_porch = 5,
        .v_pulse = 5,
        .v_total = 750,
        .v_sync_polarity = 1,

        .enable_clock = 0,
        .clock_polarity = 0,

        .enable_den = 0};

static const scanvideo_mode_t vga_mode_720p =
    {
        .default_timing = &vga_timing_1280x720_60_cea,
        .pio_program = &video_24mhz_composable,
        .width = 640,
        .height = 360,
        .xscale = 1,
        .yscale = 2,
};

static void vga_render_loop()
{
    while (true)
    {
        struct scanvideo_scanline_buffer *scanline_buffer = scanvideo_begin_scanline_generation(true);
        term_render(scanline_buffer);
        scanvideo_end_scanline_generation(scanline_buffer);
    }
}

void vga_init()
{
    multicore_launch_core1(vga_render_loop);

    set_sys_clock_khz(126000, true); // 640x480
    scanvideo_setup(&vga_mode_640x480);

    // set_sys_clock_khz(108000, true); // 1280x1024
    // set_sys_clock_khz(162000, true); // 1280x1024
    // scanvideo_setup(&vga_mode_1280x1024);

    // set_sys_clock_khz(74250, true); // 720p
    // set_sys_clock_khz(148500, true); // 720p
    // scanvideo_setup(&vga_mode_720p);

    scanvideo_timing_enable(true);

    // TODO use this when implementing mode changes
    // #if LIB_PICO_STDIO_UART
    //     // correct for clock change
    //     stdio_uart_init();
    // #endif
}
