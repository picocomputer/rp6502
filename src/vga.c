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
#include "hardware/dma.h"
#include "hardware/clocks.h"

static mutex_t vga_mutex;
static vga_display_t vga_display_current;
static vga_display_t vga_display_selected;
static vga_resolution_t vga_resolution_current;
static vga_resolution_t vga_resolution_selected;
static bool vga_terminal_current;
static bool vga_terminal_selected;
static scanvideo_mode_t const *vga_mode_current;
static scanvideo_mode_t const *vga_mode_selected;
static bool vga_mode_switch_triggered;

static const scanvideo_timing_t vga_timing_640x480_60_cea = {
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
    .v_sync_polarity = 1};

static const scanvideo_timing_t vga_timing_640x480_wide_60_cea = {
    .clock_freq = 25200000,

    .h_active = 640,
    .v_active = 360,

    .h_front_porch = 16,
    .h_pulse = 96,
    .h_total = 800,
    .h_sync_polarity = 1,

    // porch extended for letterbox effect (480->360)
    .v_front_porch = 70,
    .v_pulse = 2,
    .v_total = 525,
    .v_sync_polarity = 1};

static const scanvideo_timing_t vga_timing_1280x1024_60_dmt = {
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
    .v_sync_polarity = 1};

static const scanvideo_timing_t vga_timing_1280x1024_wide_60_dmt = {
    // half clock rate, effective 2 xscale
    .clock_freq = 54000000,

    .h_active = 640,
    .v_active = 720,

    .h_front_porch = 24,
    .h_pulse = 56,
    .h_total = 844,
    .h_sync_polarity = 0,

    // porch extended for letterbox effect (1024->720)
    .v_front_porch = 153,
    .v_pulse = 3,
    .v_total = 1066,
    .v_sync_polarity = 1};

static const scanvideo_timing_t vga_timing_1280x720_60_cea = {
    // half clock rate, effective 2 xscale
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
    .v_sync_polarity = 1};

static const scanvideo_mode_t vga_mode_320x240 = {
    .default_timing = &vga_timing_640x480_60_cea,
    .pio_program = &video_24mhz_composable,
    .width = 320,
    .height = 240,
    .xscale = 2,
    .yscale = 2};

static const scanvideo_mode_t vga_mode_640x480 = {
    .default_timing = &vga_timing_640x480_60_cea,
    .pio_program = &video_24mhz_composable,
    .width = 640,
    .height = 480,
    .xscale = 1,
    .yscale = 1};

static const scanvideo_mode_t vga_mode_320x180 = {
    .default_timing = &vga_timing_640x480_wide_60_cea,
    .pio_program = &video_24mhz_composable,
    .width = 320,
    .height = 180,
    .xscale = 2,
    .yscale = 2};

static const scanvideo_mode_t vga_mode_640x360 = {
    .default_timing = &vga_timing_640x480_wide_60_cea,
    .pio_program = &video_24mhz_composable,
    .width = 640,
    .height = 360,
    .xscale = 1,
    .yscale = 1};

static const scanvideo_mode_t vga_mode_320x240_sxga = {
    .default_timing = &vga_timing_1280x1024_60_dmt,
    .pio_program = &video_24mhz_composable,
    .width = 320,
    .height = 240,
    .xscale = 2,
    .yscale = 4};

static const scanvideo_mode_t vga_mode_640x480_sxga = {
    .default_timing = &vga_timing_1280x1024_60_dmt,
    .pio_program = &video_24mhz_composable,
    .width = 640,
    .height = 480,
    .xscale = 1,
    .yscale = 2};

static const scanvideo_mode_t vga_mode_320x180_sxga = {
    .default_timing = &vga_timing_1280x1024_wide_60_dmt,
    .pio_program = &video_24mhz_composable,
    .width = 320,
    .height = 180,
    .xscale = 2,
    .yscale = 4};

static const scanvideo_mode_t vga_mode_640x360_sxga = {
    .default_timing = &vga_timing_1280x1024_wide_60_dmt,
    .pio_program = &video_24mhz_composable,
    .width = 640,
    .height = 360,
    .xscale = 1,
    .yscale = 2};

static const scanvideo_mode_t vga_mode_320x180_hd = {
    .default_timing = &vga_timing_1280x720_60_cea,
    .pio_program = &video_24mhz_composable,
    .width = 320,
    .height = 180,
    .xscale = 2,
    .yscale = 4};

static const scanvideo_mode_t vga_mode_640x360_hd = {
    .default_timing = &vga_timing_1280x720_60_cea,
    .pio_program = &video_24mhz_composable,
    .width = 640,
    .height = 360,
    .xscale = 1,
    .yscale = 2};

// Temporary scaffolding
void vga_render_color_bar(scanvideo_scanline_buffer_t *buffer)
{
    uint line_num = scanvideo_scanline_number(buffer->scanline_id);
    int32_t color_step = 1 + (line_num * 7 / vga_mode_current->height);
    color_step = PICO_SCANVIDEO_PIXEL_FROM_RGB5(color_step & 1u, (color_step >> 1u) & 1u, (color_step >> 2u) & 1u);
    uint bar_width = vga_mode_current->width / 16;
    uint16_t *p = (uint16_t *)buffer->data;
    int32_t color = PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 0, 0);
    for (uint bar = 0; bar < 16; bar++)
    {
        *p++ = COMPOSABLE_COLOR_RUN;
        *p++ = color;
        *p++ = bar_width - 3;
        color += color_step;
    }
    *p++ = COMPOSABLE_RAW_1P;
    *p++ = 0;
    *p++ = COMPOSABLE_EOL_SKIP_ALIGN;
    *p++ = 0;
    buffer->data_used = ((uint32_t *)p) - buffer->data;
    buffer->status = SCANLINE_OK;
}

static void __not_in_flash_func(vga_render_terminal)()
{
    struct scanvideo_scanline_buffer *scanline_buffer;
    for (int i = 0; i < 480; i++)
    {
        scanline_buffer = scanvideo_begin_scanline_generation(true);
        term_render(scanline_buffer);
        scanvideo_end_scanline_generation(scanline_buffer);
    }
}

static void __not_in_flash_func(vga_render_320_240)()
{
    struct scanvideo_scanline_buffer *scanline_buffer;
    for (int i = 0; i < 240; i++)
    {
        scanline_buffer = scanvideo_begin_scanline_generation(true);
        vga_render_color_bar(scanline_buffer);
        scanvideo_end_scanline_generation(scanline_buffer);
    }
}

static void __not_in_flash_func(vga_render_640_480)()
{
    struct scanvideo_scanline_buffer *scanline_buffer;
    for (int i = 0; i < 480; i++)
    {
        scanline_buffer = scanvideo_begin_scanline_generation(true);
        vga_render_color_bar(scanline_buffer);
        scanvideo_end_scanline_generation(scanline_buffer);
    }
}

static void __not_in_flash_func(vga_render_320_180)()
{
    struct scanvideo_scanline_buffer *scanline_buffer;
    for (int i = 0; i < 180; i++)
    {
        scanline_buffer = scanvideo_begin_scanline_generation(true);
        vga_render_color_bar(scanline_buffer);
        scanvideo_end_scanline_generation(scanline_buffer);
    }
}

static void __not_in_flash_func(vga_render_640_360)()
{
    struct scanvideo_scanline_buffer *scanline_buffer;
    for (int i = 0; i < 360; i++)
    {
        scanline_buffer = scanvideo_begin_scanline_generation(true);
        vga_render_color_bar(scanline_buffer);
        scanvideo_end_scanline_generation(scanline_buffer);
    }
}

static void __not_in_flash_func(vga_render_loop)()
{
    while (true)
    {
        if (!vga_mode_switch_triggered)
        {
            mutex_enter_blocking(&vga_mutex);
            if (vga_terminal_current)
                vga_render_terminal();
            else
                switch (vga_resolution_current)
                {
                case vga_320_240:
                    vga_render_320_240();
                    break;
                case vga_640_480:
                    vga_render_640_480();
                    break;
                case vga_320_180:
                    vga_render_320_180();
                    break;
                case vga_640_360:
                    vga_render_640_360();
                    break;
                }
            mutex_exit(&vga_mutex);
        }
    }
}

static void vga_find_mode()
{
    // terminal_selected mode goes first
    if (vga_terminal_selected || vga_resolution_selected == vga_640_480)
    {
        if (vga_display_selected == vga_sxga)
            vga_mode_selected = &vga_mode_640x480_sxga;
        else
            vga_mode_selected = &vga_mode_640x480;
    }
    else if (vga_resolution_selected == vga_320_240)
    {
        if (vga_display_selected == vga_sxga)
            vga_mode_selected = &vga_mode_320x240_sxga;
        else
            vga_mode_selected = &vga_mode_320x240;
    }
    else if (vga_resolution_selected == vga_640_360)
    {
        if (vga_display_selected == vga_sxga)
            vga_mode_selected = &vga_mode_640x360_sxga;
        else if (vga_display_selected == vga_hd)
            vga_mode_selected = &vga_mode_640x360_hd;
        else
            vga_mode_selected = &vga_mode_640x360;
    }
    else if (vga_resolution_selected == vga_320_180)
    {
        if (vga_display_selected == vga_sxga)
            vga_mode_selected = &vga_mode_320x180_sxga;
        else if (vga_display_selected == vga_hd)
            vga_mode_selected = &vga_mode_320x180_hd;
        else
            vga_mode_selected = &vga_mode_320x180;
    }
    // trigger only if change detected
    if ((vga_mode_selected != vga_mode_current) ||
        (vga_terminal_selected != vga_terminal_current))
        vga_mode_switch_triggered = true;
}

static void vga_set()
{
    // "video_set_display_mode(...)" "doesn't exist yet!" -scanvideo_base.h
    // Until it does, a brute force shutdown between frames seems to work.

    // Stop and release resources previously held by scanvideo_setup()
    dma_channel_abort(0);
    if (dma_channel_is_claimed(0))
        dma_channel_unclaim(0);
    pio_clear_instruction_memory(pio0);

    // scanvideo_timing_enable is almost able to stop itself
    for (int sm = 0; sm < 4; sm++)
        if (pio_sm_is_claimed(pio0, sm))
            pio_sm_unclaim(pio0, sm);
    scanvideo_timing_enable(false);
    for (int sm = 0; sm < 4; sm++)
        if (pio_sm_is_claimed(pio0, sm))
            pio_sm_unclaim(pio0, sm);

    // begin scanvideo setup with clock setup
    uint32_t clk = vga_mode_selected->default_timing->clock_freq;
    if (clk == 25200000)
        clk = 126000000; // *5
    else if (clk == 54000000)
        clk = 162000000; // *3
    else if (clk == 37125000)
        clk = 148500000; // *4
    assert(clk >= 125000000 && clk <= 166000000);
    if (clk != clock_get_hz(clk_sys))
    {
        set_sys_clock_khz(clk / 1000, true);
#if LIB_PICO_STDIO_UART
        stdio_uart_init();
#endif
    }

    // These two calls are the main scanvideo startup
    scanvideo_setup(vga_mode_selected);
    scanvideo_timing_enable(true);

    // Swap in the new config
    vga_mode_current = vga_mode_selected;
    vga_display_current = vga_display_selected;
    vga_resolution_current = vga_resolution_selected;
    vga_terminal_current = vga_terminal_selected;
}

void vga_display(vga_display_t display)
{
    vga_display_selected = display;
    vga_find_mode();
}

void vga_resolution(vga_resolution_t mode)
{
    vga_resolution_selected = mode;
    vga_find_mode();
}

void vga_terminal(bool show)
{
    vga_terminal_selected = show;
    vga_find_mode();
}

void vga_task()
{
    if (vga_mode_switch_triggered)
    {
        if (!mutex_try_enter(&vga_mutex, 0))
            return;
        vga_set();
        vga_mode_switch_triggered = false;
        mutex_exit(&vga_mutex);
    }
}

void vga_init()
{
    mutex_init(&vga_mutex);
    vga_display(vga_sd);
    vga_resolution(vga_320_240);
    vga_terminal(true);
    vga_set();
    vga_mode_switch_triggered = false;
    multicore_launch_core1(vga_render_loop);
}
