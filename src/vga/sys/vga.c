/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "sys/ria.h"
#include "sys/vga.h"
#include "sys/xram.h"
#include "term/term.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/scanvideo.h"
#include "pico/scanvideo/composable_scanline.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include <string.h>

vga_prog_t vga_prog[VGA_PROG_MAX];

static mutex_t vga_mutex;
static volatile vga_display_t vga_display_current;
static vga_display_t vga_display_selected;
static volatile vga_canvas_t vga_canvas_current;
static vga_canvas_t vga_canvas_selected;
static volatile scanvideo_mode_t const *vga_scanvideo_mode_current;
static scanvideo_mode_t const *vga_scanvideo_mode_selected;
static volatile bool vga_scanvideo_mode_switching;

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

static const scanvideo_timing_t vga_timing_1280x1024_tall_60_dmt = {
    // half clock rate, effective 2 xscale
    .clock_freq = 54000000,

    .h_active = 640,
    .v_active = 1024,

    .h_front_porch = 24,
    .h_pulse = 56,
    .h_total = 844,
    .h_sync_polarity = 0,

    .v_front_porch = 1,
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

static const scanvideo_mode_t vga_scanvideo_mode_320x240 = {
    .default_timing = &vga_timing_640x480_60_cea,
    .pio_program = &video_24mhz_composable,
    .width = 320,
    .height = 240,
    .xscale = 2,
    .yscale = 2};

static const scanvideo_mode_t vga_scanvideo_mode_640x480 = {
    .default_timing = &vga_timing_640x480_60_cea,
    .pio_program = &video_24mhz_composable,
    .width = 640,
    .height = 480,
    .xscale = 1,
    .yscale = 1};

static const scanvideo_mode_t vga_scanvideo_mode_320x180 = {
    .default_timing = &vga_timing_640x480_wide_60_cea,
    .pio_program = &video_24mhz_composable,
    .width = 320,
    .height = 180,
    .xscale = 2,
    .yscale = 2};

static const scanvideo_mode_t vga_scanvideo_mode_640x360 = {
    .default_timing = &vga_timing_640x480_wide_60_cea,
    .pio_program = &video_24mhz_composable,
    .width = 640,
    .height = 360,
    .xscale = 1,
    .yscale = 1};

static const scanvideo_mode_t vga_scanvideo_mode_320x240_sxga = {
    .default_timing = &vga_timing_1280x1024_60_dmt,
    .pio_program = &video_24mhz_composable,
    .width = 320,
    .height = 240,
    .xscale = 2,
    .yscale = 4};

static const scanvideo_mode_t vga_scanvideo_mode_640x480_sxga = {
    .default_timing = &vga_timing_1280x1024_60_dmt,
    .pio_program = &video_24mhz_composable,
    .width = 640,
    .height = 480,
    .xscale = 1,
    .yscale = 2};

static const scanvideo_mode_t vga_scanvideo_mode_640x512_sxga = {
    .default_timing = &vga_timing_1280x1024_tall_60_dmt,
    .pio_program = &video_24mhz_composable,
    .width = 640,
    .height = 512,
    .xscale = 1,
    .yscale = 2};

static const scanvideo_mode_t vga_scanvideo_mode_320x180_sxga = {
    .default_timing = &vga_timing_1280x1024_wide_60_dmt,
    .pio_program = &video_24mhz_composable,
    .width = 320,
    .height = 180,
    .xscale = 2,
    .yscale = 4};

static const scanvideo_mode_t vga_scanvideo_mode_640x360_sxga = {
    .default_timing = &vga_timing_1280x1024_wide_60_dmt,
    .pio_program = &video_24mhz_composable,
    .width = 640,
    .height = 360,
    .xscale = 1,
    .yscale = 2};

static const scanvideo_mode_t vga_scanvideo_mode_320x180_hd = {
    .default_timing = &vga_timing_1280x720_60_cea,
    .pio_program = &video_24mhz_composable,
    .width = 320,
    .height = 180,
    .xscale = 2,
    .yscale = 4};

static const scanvideo_mode_t vga_scanvideo_mode_640x360_hd = {
    .default_timing = &vga_timing_1280x720_60_cea,
    .pio_program = &video_24mhz_composable,
    .width = 640,
    .height = 360,
    .xscale = 1,
    .yscale = 2};

static inline void __attribute__((optimize("O1")))
vga_render_scanline(scanvideo_scanline_buffer_t *scanline_buffer)
{
    const uint16_t width = vga_scanvideo_mode_current->width;
    const int16_t scanline_id = scanline_buffer->scanline_id;
    uint32_t *const data[3] = {scanline_buffer->data, scanline_buffer->data2, scanline_buffer->data3};
    bool filled[3] = {false, false, false};
    uint32_t *foreground = NULL;
    vga_prog_t prog = vga_prog[scanline_id];
    for (int16_t i = 0; i < 3; i++)
    {
        if (prog.fill[i])
        {
            filled[i] = prog.fill[i](i, scanline_id,
                                     vga_scanvideo_mode_current->width,
                                     (uint16_t *)(data[i] + 1),
                                     vga_prog->fill_ctx[scanline_id]);
            if (filled[i])
                foreground = data[i];
        }
        if (prog.sprite[i])
        {
            if (!foreground)
            {
                foreground = data[i];
                memset(foreground, 0, width * 2);
                filled[i] = true;
            }
            prog.sprite[i](i, scanline_id,
                           vga_scanvideo_mode_current->width,
                           (uint16_t *)(foreground + 1));
        }
    }
    for (int16_t i = 0; i < 3; i++)
    {
        uint16_t data_used;
        if (filled[i])
        {
            data[i][0] = COMPOSABLE_RAW_RUN | (data[i][1] << 16);
            data[i][1] = width - 3 | (data[i][1] & 0xFFFF0000);
            data[i][width / 2 + 1] = COMPOSABLE_RAW_1P | (0 << 16);
            data[i][width / 2 + 2] = COMPOSABLE_EOL_SKIP_ALIGN;
            data_used = width / 2 + 3;
        }
        else
        {
            data[i][0] = COMPOSABLE_RAW_1P | (0 << 16);
            data[i][1] = COMPOSABLE_EOL_SKIP_ALIGN;
            data_used = 2;
        }
        switch (i)
        {
        case 0:
            scanline_buffer->data_used = data_used;
            break;
        case 1:
            scanline_buffer->data2_used = data_used;
            break;
        case 2:
            scanline_buffer->data3_used = data_used;
            break;
        }
    }
}

static void __attribute__((optimize("O1")))
vga_render_loop(void)
{
    assert(PICO_SCANVIDEO_PLANE_COUNT == 3);
    while (true)
    {
        if (!vga_scanvideo_mode_switching)
        {
            mutex_enter_blocking(&vga_mutex);
            const int16_t height = vga_scanvideo_mode_current->height;
            for (int16_t i = 0; i < height; i++)
            {
                scanvideo_scanline_buffer_t *const scanline_buffer =
                    scanvideo_begin_scanline_generation(true);
                vga_render_scanline(scanline_buffer);
                scanvideo_end_scanline_generation(scanline_buffer);
            }
            mutex_exit(&vga_mutex);
            ria_vsync();
        }
    }
}

static void vga_scanvideo_update(void)
{
    if (vga_canvas_selected == vga_console)
    {
        if (vga_display_selected == vga_sxga)
            vga_scanvideo_mode_selected = &vga_scanvideo_mode_640x512_sxga;
        else
            vga_scanvideo_mode_selected = &vga_scanvideo_mode_640x480;
    }
    else if (vga_canvas_selected == vga_320_240)
    {
        if (vga_display_selected == vga_sxga)
            vga_scanvideo_mode_selected = &vga_scanvideo_mode_320x240_sxga;
        else
            vga_scanvideo_mode_selected = &vga_scanvideo_mode_320x240;
    }
    else if (vga_canvas_selected == vga_640_480)
    {
        if (vga_display_selected == vga_sxga)
            vga_scanvideo_mode_selected = &vga_scanvideo_mode_640x480_sxga;
        else
            vga_scanvideo_mode_selected = &vga_scanvideo_mode_640x480;
    }
    else if (vga_canvas_selected == vga_320_180)
    {
        if (vga_display_selected == vga_sxga)
            vga_scanvideo_mode_selected = &vga_scanvideo_mode_320x180_sxga;
        else if (vga_display_selected == vga_hd)
            vga_scanvideo_mode_selected = &vga_scanvideo_mode_320x180_hd;
        else
            vga_scanvideo_mode_selected = &vga_scanvideo_mode_320x180;
    }
    else if (vga_canvas_selected == vga_640_360)
    {
        if (vga_display_selected == vga_sxga)
            vga_scanvideo_mode_selected = &vga_scanvideo_mode_640x360_sxga;
        else if (vga_display_selected == vga_hd)
            vga_scanvideo_mode_selected = &vga_scanvideo_mode_640x360_hd;
        else
            vga_scanvideo_mode_selected = &vga_scanvideo_mode_640x360;
    }
    // trigger only if change detected
    if (vga_scanvideo_mode_selected != vga_scanvideo_mode_current)
        vga_scanvideo_mode_switching = true;
}

static void vga_scanvideo_start(void)
{
    // "video_set_display_mode(...)" "doesn't exist yet!" -scanvideo_base.h
    // Until it does, a brute force shutdown between frames seems to work.

    // Stop and release resources previously held by scanvideo_setup()
    for (int i = 0; i < 3; i++)
    {
        dma_channel_abort(i);
        if (dma_channel_is_claimed(i))
            dma_channel_unclaim(i);
    }
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
    uint32_t clk = vga_scanvideo_mode_selected->default_timing->clock_freq;
    if (clk == 25200000)
        clk = 25200000 * 8; // 201.6 MHz
    else if (clk == 54000000)
        clk = 54000000 * 4; // 216 MHz
    else if (clk == 37125000)
        clk = 37125000 * 5; // 185.625 MHz
    assert(clk >= 120000000 && clk <= 266000000);
    if (clk != clock_get_hz(clk_sys))
    {
        set_sys_clock_khz(clk / 1000, true);
        main_reclock();
    }

    // These two calls are the main scanvideo startup
    scanvideo_setup(vga_scanvideo_mode_selected);
    scanvideo_timing_enable(true);

    // Swap in the new config
    vga_scanvideo_mode_current = vga_scanvideo_mode_selected;
    vga_display_current = vga_display_selected;
    vga_canvas_current = vga_canvas_selected;
}

static void vga_reset_console_prog()
{
    uint16_t xregs_console[] = {0, vga_console, 0, 0, 0};
    main_prog(xregs_console);
}

void vga_set_display(vga_display_t display)
{
    vga_display_selected = display;
    vga_scanvideo_update();
    if (vga_scanvideo_mode_switching && vga_canvas_selected == vga_console)
        vga_reset_console_prog();
}

// Also accepts NULL for reset to vga_console
bool vga_xreg_canvas(uint16_t *xregs)
{
    uint16_t canvas = xregs ? xregs[0] : 0;
    switch (canvas)
    {
    case vga_console:
    case vga_320_240:
    case vga_320_180:
    case vga_640_480:
    case vga_640_360:
        vga_canvas_selected = canvas;
        vga_scanvideo_update();
        break;
    default:
        return false;
    }
    memset(&vga_prog, 0, sizeof(vga_prog));
    if (canvas == vga_console)
        vga_reset_console_prog();
    return true;
}

uint16_t vga_height(void)
{
    return vga_scanvideo_mode_selected->height;
}

void vga_init(void)
{
    // safety check for compiler alignment
    assert(!((uintptr_t)xram & 0xFFFF));

    mutex_init(&vga_mutex);
    vga_set_display(vga_sd);
    vga_xreg_canvas(NULL);
    vga_scanvideo_start();
    vga_scanvideo_mode_switching = false;
    multicore_launch_core1(vga_render_loop);
}

void vga_task(void)
{
    if (vga_scanvideo_mode_switching)
    {
        if (!mutex_try_enter(&vga_mutex, 0))
            return;
        vga_scanvideo_start();
        vga_scanvideo_mode_switching = false;
        mutex_exit(&vga_mutex);
    }
}
