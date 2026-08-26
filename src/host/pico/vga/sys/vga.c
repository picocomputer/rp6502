/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "vga/main.h"
#include "vga/sys/ria.h"
#include "vga/sys/vga.h"
#include "core/vga/prog.h"
#include "core/mem.h"
#include "core/term/term.h"
#include "vga/scanvideo/scanvideo.h"
#include <pico/stdlib.h>
#include <pico/multicore.h>
#include <hardware/clocks.h>
#include <string.h>

#pragma GCC push_options
#pragma GCC optimize("O3")

static mutex_t vga_scanline_mutex;
static volatile bool vga_rendering[2];
static volatile uint16_t vga_vsync_frame_fired;
static volatile vga_display_t vga_display_current;
static vga_display_t vga_display_selected;
static volatile vga_canvas_t vga_canvas_current;
static vga_canvas_t vga_canvas_selected;
static volatile scanvideo_view_t const *vga_view_current;
static scanvideo_view_t const *vga_view_selected;
static volatile bool vga_view_switching;

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

static const scanvideo_timing_t vga_timing_1280x1024_60_dmt = {
    // half clock rate, effective 2 x_scale
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

static const scanvideo_timing_t vga_timing_1280x720_60_cea = {
    // half clock rate, effective 2 x_scale
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

static const scanvideo_view_t vga_view_320x240 = {
    .default_timing = &vga_timing_640x480_60_cea,
    .width = 320,
    .height = 240,
    .x_scale = 2,
    .y_scale = 2,
    .y_offset = 0};

static const scanvideo_view_t vga_view_640x480 = {
    .default_timing = &vga_timing_640x480_60_cea,
    .width = 640,
    .height = 480,
    .x_scale = 1,
    .y_scale = 1,
    .y_offset = 0};

static const scanvideo_view_t vga_view_320x180 = {
    .default_timing = &vga_timing_640x480_60_cea,
    .width = 320,
    .height = 180,
    .x_scale = 2,
    .y_scale = 2,
    .y_offset = 60};

static const scanvideo_view_t vga_view_640x360 = {
    .default_timing = &vga_timing_640x480_60_cea,
    .width = 640,
    .height = 360,
    .x_scale = 1,
    .y_scale = 1,
    .y_offset = 60};

static const scanvideo_view_t vga_view_320x240_sxga = {
    .default_timing = &vga_timing_1280x1024_60_dmt,
    .width = 320,
    .height = 240,
    .x_scale = 2,
    .y_scale = 4,
    .y_offset = 32};

static const scanvideo_view_t vga_view_640x480_sxga = {
    .default_timing = &vga_timing_1280x1024_60_dmt,
    .width = 640,
    .height = 480,
    .x_scale = 1,
    .y_scale = 2,
    .y_offset = 32};

static const scanvideo_view_t vga_view_640x512_sxga = {
    .default_timing = &vga_timing_1280x1024_60_dmt,
    .width = 640,
    .height = 512,
    .x_scale = 1,
    .y_scale = 2,
    .y_offset = 0};

static const scanvideo_view_t vga_view_320x180_sxga = {
    .default_timing = &vga_timing_1280x1024_60_dmt,
    .width = 320,
    .height = 180,
    .x_scale = 2,
    .y_scale = 4,
    .y_offset = 152};

static const scanvideo_view_t vga_view_640x360_sxga = {
    .default_timing = &vga_timing_1280x1024_60_dmt,
    .width = 640,
    .height = 360,
    .x_scale = 1,
    .y_scale = 2,
    .y_offset = 152};

static const scanvideo_view_t vga_view_320x180_hd = {
    .default_timing = &vga_timing_1280x720_60_cea,
    .width = 320,
    .height = 180,
    .x_scale = 2,
    .y_scale = 4,
    .y_offset = 0};

static const scanvideo_view_t vga_view_640x360_hd = {
    .default_timing = &vga_timing_1280x720_60_cea,
    .width = 640,
    .height = 360,
    .x_scale = 1,
    .y_scale = 2,
    .y_offset = 0};

static void vga_scanvideo_switch(void)
{
    if (!vga_view_switching)
        return;

    // Prevent new renders from starting, then wait for any in-flight
    // render to finish, since shared state including spinlocks gets zeroed.
    mutex_enter_blocking(&vga_scanline_mutex);
    while (vga_rendering[0] || vga_rendering[1])
        tight_loop_contents();

    // Set system clock for the new video mode; must run before scanvideo_set_mode()
    uint32_t clk = vga_view_selected->default_timing->clock_freq;
    if (clk == 25200000)
        clk = 25200000 * 8; // 201.6 MHz
    else if (clk == 54000000)
        clk = 54000000 * 4; // 216.0 MHz
    else if (clk == 37125000)
        clk = 37125000 * 4; // 148.5 MHz
    assert(clk >= 120000000 && clk <= 266000000);
    if (clk != clock_get_hz(clk_sys))
    {
        main_pre_reclock();
        set_sys_clock_khz(clk / 1000, true);
        main_post_reclock();
    }

    scanvideo_set_mode(vga_view_selected);

    vga_view_current = vga_view_selected;
    vga_display_current = vga_display_selected;
    vga_canvas_current = vga_canvas_selected;
    vga_view_switching = false;

    mutex_exit(&vga_scanline_mutex);
}

// Fires ria_vsync once per frame at the highest scanline touched by any program,
// with a scanline-0 fallback if that threshold was never reached. Keyed on the
// frame number because both cores render concurrently and may complete out of
// order; the RIA edge-triggers VSYNC, so a second byte would be a phantom frame.
static void __not_in_flash_func(vga_scanline_complete)(uint32_t scanline_id)
{
    int16_t scanline = scanvideo_scanline_number(scanline_id);
    int16_t view_height = vga_view_current->height;
    int16_t highest = vga_prog_highest() > 0 && vga_prog_highest() <= view_height
                          ? vga_prog_highest()
                          : view_height;
    if (scanline + 1 < highest && scanline != 0)
        return;
    uint16_t frame = scanvideo_frame_number(scanline_id);
    if (scanline == 0 && scanline + 1 < highest)
        // Fallback for a frame that never reached highest. The previous frame
        // owns it; scanvideo skipped ahead before rendering its threshold line.
        --frame;
    if (__atomic_exchange_n(&vga_vsync_frame_fired, frame, __ATOMIC_ACQ_REL) != frame)
        ria_vsync();
}

static void vga_render_scanline(void)
{
    if (!mutex_try_enter(&vga_scanline_mutex, 0))
        return;
    scanvideo_scanline_buffer_t *const scanline_buffer =
        scanvideo_begin_scanline_generation();
    if (!scanline_buffer)
    {
        mutex_exit(&vga_scanline_mutex);
        return;
    }
    vga_rendering[get_core_num()] = true;
    mutex_exit(&vga_scanline_mutex);

    const uint16_t width = vga_view_current->width;
    const uint32_t buffer_scanline_id = scanline_buffer->scanline_id;
    const int16_t scanline_id = scanvideo_scanline_number(buffer_scanline_id);
    uint32_t *const data[SCANVIDEO_PLANE_COUNT] = {scanline_buffer->data0, scanline_buffer->data1, scanline_buffer->data2};
    bool filled[SCANVIDEO_PLANE_COUNT] = {false, false, false};
    uint32_t *foreground = NULL;
    vga_prog_t prog = *vga_prog_row(scanline_id);
    for (int i = 0; i < SCANVIDEO_PLANE_COUNT; i++)
    {
        if (prog.fill_fn[i])
        {
            filled[i] = prog.fill_fn[i](i,
                                        scanline_id,
                                        width,
                                        (uint16_t *)(data[i] + 1),
                                        prog.fill_config[i]);
            if (filled[i])
                foreground = data[i];
        }
        if (prog.sprite_fn[i])
        {
            if (!foreground)
            {
                foreground = data[i];
                memset(foreground + 1, 0, width * 2);
                filled[i] = true;
            }
            prog.sprite_fn[i](scanline_id,
                              width,
                              (uint16_t *)(foreground + 1),
                              prog.sprite_config[i],
                              prog.sprite_length[i]);
        }
    }
    for (int i = 0; i < SCANVIDEO_PLANE_COUNT; i++)
    {
        uint16_t data_used;
        if (filled[i])
        {
            data[i][0] = COMPOSABLE_RAW_RUN | (data[i][1] << 16);
            data[i][1] = (width - 3) | (data[i][1] & 0xFFFF0000);
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
            scanline_buffer->data0_used = data_used;
            break;
        case 1:
            scanline_buffer->data1_used = data_used;
            break;
        case 2:
            scanline_buffer->data2_used = data_used;
            break;
        }
    }
    scanvideo_end_scanline_generation(scanline_buffer);
    vga_scanline_complete(buffer_scanline_id);
    vga_rendering[get_core_num()] = false;
}

// [canvas][display: sd, hd, sxga]
__in_flash("vga_canvas_view_table") static const scanvideo_view_t *const vga_canvas_view_table[][3] = {
    [vga_canvas_console] = {&vga_view_640x480, &vga_view_640x480, &vga_view_640x512_sxga},
    [vga_canvas_320_240] = {&vga_view_320x240, &vga_view_320x240, &vga_view_320x240_sxga},
    [vga_canvas_320_180] = {&vga_view_320x180, &vga_view_320x180_hd, &vga_view_320x180_sxga},
    [vga_canvas_640_480] = {&vga_view_640x480, &vga_view_640x480, &vga_view_640x480_sxga},
    [vga_canvas_640_360] = {&vga_view_640x360, &vga_view_640x360_hd, &vga_view_640x360_sxga},
};

static void vga_scanvideo_update(void)
{
    vga_view_selected = vga_canvas_view_table[vga_canvas_selected][vga_display_selected];
    if (vga_view_selected != vga_view_current)
        vga_view_switching = true;
}

static void vga_reset_console_prog(void)
{
    uint16_t xregs_console[] = {0, vga_canvas_console, 0, 0, 0};
    main_prog(xregs_console);
}

void vga_set_display(vga_display_t display)
{
    vga_display_selected = display;
    vga_scanvideo_update();
    if (vga_view_switching && vga_canvas_selected == vga_canvas_console)
        vga_reset_console_prog();
}

// Also accepts NULL for reset to vga_canvas_console.
// When xregs is non-NULL (pix xreg), sends ACK/NAK via backchannel.
// The ACK goes out here, not after the switch vga_scanvideo_update queues.
void vga_xreg_canvas(uint16_t *xregs)
{
    vga_canvas_t canvas = xregs ? xregs[0] : vga_canvas_console;
    switch (canvas)
    {
    case vga_canvas_console:
        // prevent flicker when reset not needed
        if (vga_canvas_selected == vga_canvas_console)
        {
            if (xregs)
                ria_ack();
            return;
        }
        __attribute__((fallthrough));
    case vga_canvas_320_240:
    case vga_canvas_320_180:
    case vga_canvas_640_480:
    case vga_canvas_640_360:
        vga_canvas_selected = canvas;
        vga_scanvideo_update();
        break;
    default:
        if (xregs)
            ria_nak();
        return;
    }
    vga_prog_reset();
    if (canvas == vga_canvas_console)
        vga_reset_console_prog();
    if (xregs)
        ria_ack();
}

int16_t vga_canvas_height(void)
{
    return vga_view_selected->height;
}

static void vga_render_loop(void)
{
    while (true)
        vga_render_scanline();
}

void vga_init(void)
{
    // xram must be 64K-aligned for 6502 address wrap
    assert(!((uintptr_t)xram & 0xFFFF));

    mutex_init(&vga_scanline_mutex);
    vga_set_display(vga_sd);
    vga_xreg_canvas(NULL);
    vga_scanvideo_switch();
    multicore_launch_core1(vga_render_loop);
}

void vga_task(void)
{
    vga_scanvideo_switch();
    vga_render_scanline();
}

bool vga_canvas_is_console(void)
{
    return vga_canvas_selected == vga_canvas_console;
}

#pragma GCC pop_options
