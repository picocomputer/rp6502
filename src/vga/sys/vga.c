/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "main.h"
#include "xram.h"
#include "vga.h"
#include "term/term.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/scanvideo.h"
#include "pico/scanvideo/composable_scanline.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include <string.h>

static mutex_t vga_mutex;
static volatile vga_display_t vga_display_current;
static vga_display_t vga_display_selected;
static volatile vga_resolution_t vga_resolution_current;
static vga_resolution_t vga_resolution_selected;
static volatile bool vga_terminal_current;
static bool vga_terminal_selected;
static volatile scanvideo_mode_t const *vga_mode_current;
static scanvideo_mode_t const *vga_mode_selected;
static volatile bool vga_mode_switch_triggered;

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

static const scanvideo_mode_t vga_mode_640x512_sxga = {
    .default_timing = &vga_timing_1280x1024_tall_60_dmt,
    .pio_program = &video_24mhz_composable,
    .width = 640,
    .height = 512,
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

// PETSCII with some reordering.
// 0-31 and 96-127 swapped so letters match ASCII.
static const uint8_t __in_flash("haxscii") haxscii[1024] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
    0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x33, 0x33, 0xCC, 0xCC, 0x33, 0x33, 0xCC, 0xCC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF,
    0x30, 0x30, 0xC0, 0xC0, 0x30, 0x30, 0xC0, 0xC0, 0x33, 0x99, 0xCC, 0x66, 0x33, 0x99, 0xCC, 0x66,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x18, 0x18, 0x18,
    0x00, 0x00, 0x00, 0x00, 0xF0, 0xF0, 0xF0, 0xF0, 0x00, 0x00, 0x00, 0x1F, 0x1F, 0x18, 0x18, 0x18,
    0x18, 0x18, 0x18, 0xF8, 0xF8, 0x00, 0x00, 0x00, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0,
    0x00, 0x00, 0x00, 0xF8, 0xF8, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1F, 0x1F, 0x18, 0x18, 0x18,
    0x18, 0x18, 0x18, 0xF8, 0xF8, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0xFF, 0xFF, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0xE0, 0xE0, 0xE0, 0xE0, 0xE0, 0xE0, 0xE0, 0xE0,
    0x00, 0x78, 0x78, 0x30, 0x18, 0x0C, 0x06, 0x03, 0xF0, 0xF0, 0xF0, 0xF0, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x0F, 0x0F, 0x0F, 0x0F, 0x18, 0x18, 0x18, 0x1F, 0x1F, 0x00, 0x00, 0x00,
    0x0F, 0x0F, 0x0F, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x0F, 0x0F, 0x0F, 0xF0, 0xF0, 0xF0, 0xF0,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4F, 0x4F, 0x00, 0x00, 0x00,
    0x00, 0x07, 0x07, 0x00, 0x00, 0x07, 0x07, 0x00, 0x14, 0x7F, 0x7F, 0x14, 0x14, 0x7F, 0x7F, 0x14,
    0x00, 0x24, 0x2E, 0x6B, 0x6B, 0x3A, 0x12, 0x00, 0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00,
    0x00, 0x32, 0x7F, 0x4D, 0x4D, 0x77, 0x72, 0x50, 0x00, 0x00, 0x00, 0x04, 0x06, 0x03, 0x01, 0x00,
    0x00, 0x00, 0x1C, 0x3E, 0x63, 0x41, 0x00, 0x00, 0x00, 0x00, 0x41, 0x63, 0x3E, 0x1C, 0x00, 0x00,
    0x08, 0x2A, 0x3E, 0x1C, 0x1C, 0x3E, 0x2A, 0x08, 0x00, 0x08, 0x08, 0x3E, 0x3E, 0x08, 0x08, 0x00,
    0x00, 0x00, 0x80, 0xE0, 0x60, 0x00, 0x00, 0x00, 0x00, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x00,
    0x00, 0x00, 0x00, 0x60, 0x60, 0x00, 0x00, 0x00, 0x00, 0x40, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x02,
    0x00, 0x3E, 0x7F, 0x49, 0x45, 0x7F, 0x3E, 0x00, 0x00, 0x40, 0x44, 0x7F, 0x7F, 0x40, 0x40, 0x00,
    0x00, 0x62, 0x73, 0x51, 0x49, 0x4F, 0x46, 0x00, 0x00, 0x22, 0x63, 0x49, 0x49, 0x7F, 0x36, 0x00,
    0x00, 0x18, 0x18, 0x14, 0x16, 0x7F, 0x7F, 0x10, 0x00, 0x27, 0x67, 0x45, 0x45, 0x7D, 0x39, 0x00,
    0x00, 0x3E, 0x7F, 0x49, 0x49, 0x7B, 0x32, 0x00, 0x00, 0x03, 0x03, 0x79, 0x7D, 0x07, 0x03, 0x00,
    0x00, 0x36, 0x7F, 0x49, 0x49, 0x7F, 0x36, 0x00, 0x00, 0x26, 0x6F, 0x49, 0x49, 0x7F, 0x3E, 0x00,
    0x00, 0x00, 0x00, 0x24, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xE4, 0x64, 0x00, 0x00, 0x00,
    0x00, 0x08, 0x1C, 0x36, 0x63, 0x41, 0x41, 0x00, 0x00, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x00,
    0x00, 0x41, 0x41, 0x63, 0x36, 0x1C, 0x08, 0x00, 0x00, 0x02, 0x03, 0x51, 0x59, 0x0F, 0x06, 0x00,
    0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x7C, 0x7E, 0x0B, 0x0B, 0x7E, 0x7C, 0x00,
    0x00, 0x7F, 0x7F, 0x49, 0x49, 0x7F, 0x36, 0x00, 0x00, 0x3E, 0x7F, 0x41, 0x41, 0x63, 0x22, 0x00,
    0x00, 0x7F, 0x7F, 0x41, 0x63, 0x3E, 0x1C, 0x00, 0x00, 0x7F, 0x7F, 0x49, 0x49, 0x41, 0x41, 0x00,
    0x00, 0x7F, 0x7F, 0x09, 0x09, 0x01, 0x01, 0x00, 0x00, 0x3E, 0x7F, 0x41, 0x49, 0x7B, 0x3A, 0x00,
    0x00, 0x7F, 0x7F, 0x08, 0x08, 0x7F, 0x7F, 0x00, 0x00, 0x00, 0x41, 0x7F, 0x7F, 0x41, 0x00, 0x00,
    0x00, 0x20, 0x60, 0x41, 0x7F, 0x3F, 0x01, 0x00, 0x00, 0x7F, 0x7F, 0x1C, 0x36, 0x63, 0x41, 0x00,
    0x00, 0x7F, 0x7F, 0x40, 0x40, 0x40, 0x40, 0x00, 0x00, 0x7F, 0x7F, 0x06, 0x0C, 0x06, 0x7F, 0x7F,
    0x00, 0x7F, 0x7F, 0x0E, 0x1C, 0x7F, 0x7F, 0x00, 0x00, 0x3E, 0x7F, 0x41, 0x41, 0x7F, 0x3E, 0x00,
    0x00, 0x7F, 0x7F, 0x09, 0x09, 0x0F, 0x06, 0x00, 0x00, 0x1E, 0x3F, 0x21, 0x61, 0x7F, 0x5E, 0x00,
    0x00, 0x7F, 0x7F, 0x19, 0x39, 0x6F, 0x46, 0x00, 0x00, 0x26, 0x6F, 0x49, 0x49, 0x7B, 0x32, 0x00,
    0x00, 0x01, 0x01, 0x7F, 0x7F, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x7F, 0x40, 0x40, 0x7F, 0x3F, 0x00,
    0x00, 0x1F, 0x3F, 0x60, 0x60, 0x3F, 0x1F, 0x00, 0x00, 0x7F, 0x7F, 0x30, 0x18, 0x30, 0x7F, 0x7F,
    0x00, 0x63, 0x77, 0x1C, 0x1C, 0x77, 0x63, 0x00, 0x00, 0x07, 0x0F, 0x78, 0x78, 0x0F, 0x07, 0x00,
    0x00, 0x61, 0x71, 0x59, 0x4D, 0x47, 0x43, 0x00, 0x18, 0x18, 0x18, 0xFF, 0xFF, 0x18, 0x18, 0x18,
    0x33, 0x33, 0xCC, 0xCC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00,
    0xCC, 0xCC, 0x33, 0x33, 0xCC, 0xCC, 0x33, 0x33, 0x66, 0xCC, 0x99, 0x33, 0x66, 0xCC, 0x99, 0x33,
    0x00, 0x3E, 0x7F, 0x41, 0x4D, 0x4F, 0x2E, 0x00, 0x00, 0x20, 0x74, 0x54, 0x54, 0x7C, 0x78, 0x00,
    0x00, 0x7E, 0x7E, 0x48, 0x48, 0x78, 0x30, 0x00, 0x00, 0x38, 0x7C, 0x44, 0x44, 0x44, 0x00, 0x00,
    0x00, 0x30, 0x78, 0x48, 0x48, 0x7E, 0x7E, 0x00, 0x00, 0x38, 0x7C, 0x54, 0x54, 0x5C, 0x18, 0x00,
    0x00, 0x00, 0x08, 0x7C, 0x7E, 0x0A, 0x0A, 0x00, 0x00, 0x98, 0xBC, 0xA4, 0xA4, 0xFC, 0x7C, 0x00,
    0x00, 0x7E, 0x7E, 0x08, 0x08, 0x78, 0x70, 0x00, 0x00, 0x00, 0x48, 0x7A, 0x7A, 0x40, 0x00, 0x00,
    0x00, 0x00, 0x80, 0x80, 0x80, 0xFA, 0x7A, 0x00, 0x00, 0x7E, 0x7E, 0x10, 0x38, 0x68, 0x40, 0x00,
    0x00, 0x00, 0x42, 0x7E, 0x7E, 0x40, 0x00, 0x00, 0x00, 0x7C, 0x7C, 0x18, 0x38, 0x1C, 0x7C, 0x78,
    0x00, 0x7C, 0x7C, 0x04, 0x04, 0x7C, 0x78, 0x00, 0x00, 0x38, 0x7C, 0x44, 0x44, 0x7C, 0x38, 0x00,
    0x00, 0xFC, 0xFC, 0x24, 0x24, 0x3C, 0x18, 0x00, 0x00, 0x18, 0x3C, 0x24, 0x24, 0xFC, 0xFC, 0x00,
    0x00, 0x7C, 0x7C, 0x04, 0x04, 0x0C, 0x08, 0x00, 0x00, 0x48, 0x5C, 0x54, 0x54, 0x74, 0x24, 0x00,
    0x00, 0x04, 0x04, 0x3E, 0x7E, 0x44, 0x44, 0x00, 0x00, 0x3C, 0x7C, 0x40, 0x40, 0x7C, 0x7C, 0x00,
    0x00, 0x1C, 0x3C, 0x60, 0x60, 0x3C, 0x1C, 0x00, 0x00, 0x1C, 0x7C, 0x70, 0x38, 0x70, 0x7C, 0x1C,
    0x00, 0x44, 0x6C, 0x38, 0x38, 0x6C, 0x44, 0x00, 0x00, 0x9C, 0xBC, 0xA0, 0xE0, 0x7C, 0x3C, 0x00,
    0x00, 0x44, 0x64, 0x74, 0x5C, 0x4C, 0x44, 0x00, 0x00, 0x00, 0x7F, 0x7F, 0x41, 0x41, 0x00, 0x00,
    0x40, 0x68, 0x7C, 0x5E, 0x49, 0x49, 0x22, 0x00, 0x00, 0x00, 0x41, 0x41, 0x7F, 0x7F, 0x00, 0x00,
    0x00, 0x08, 0x0C, 0xFE, 0xFE, 0x0C, 0x08, 0x00, 0x00, 0x18, 0x3C, 0x7E, 0x18, 0x18, 0x18, 0x18};

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

// Temporary scaffolding
void vga_render_mono_haxscii(scanvideo_scanline_buffer_t *dest)
{
    static const uint32_t colors[] = {
        0x0A34 | (0x0A34 << 16),
        0x0A34 | (0x0D88 << 16),
        0x0D88 | (0x0A34 << 16),
        0x0D88 | (0x0D88 << 16)};

    int line = scanvideo_scanline_number(dest->scanline_id);
    const uint8_t *font_base = &xram[0x1000] + (line & 7);

    int columns = vga_mode_current->width / 8;

    uint8_t *term_ptr = xram + columns * (line / 8);
    uint32_t *buf = dest->data;
    for (int i = 0; i < columns; i++)
    {
        uint8_t bits = font_base[term_ptr[i] * 8];
        *++buf = colors[bits >> 6];
        *++buf = colors[bits >> 4 & 0x03];
        *++buf = colors[bits >> 2 & 0x03];
        *++buf = colors[bits & 0x03];
    }
    buf = (void *)dest->data;
    if (columns == 80)
    {
        buf[0] = COMPOSABLE_RAW_RUN | (buf[1] << 16);
        buf[1] = 637 | (buf[1] & 0xFFFF0000);
        buf[321] = COMPOSABLE_RAW_1P | 0;
        buf[322] = COMPOSABLE_EOL_SKIP_ALIGN;
        dest->data_used = 323;
        dest->status = SCANLINE_OK;
        return;
    }
    assert(columns == 40);
    buf[0] = COMPOSABLE_RAW_RUN | (buf[1] << 16);
    buf[1] = 317 | (buf[1] & 0xFFFF0000);
    buf[161] = COMPOSABLE_RAW_1P | 0;
    buf[162] = COMPOSABLE_EOL_SKIP_ALIGN;
    dest->data_used = 163;
    dest->status = SCANLINE_OK;
}

static void vga_render_terminal(void)
{
    struct scanvideo_scanline_buffer *scanline_buffer;
    for (int i = 0; i < 480; i++)
    {
        scanline_buffer = scanvideo_begin_scanline_generation(true);
        term_render(scanline_buffer, vga_mode_current->height);
        scanvideo_end_scanline_generation(scanline_buffer);
    }
}

static void vga_render_4bpp(struct scanvideo_scanline_buffer *dest)
{
    static const uint32_t colors[] = {
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 0, 0),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(205, 0, 0),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 205, 0),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(205, 205, 0),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 0, 205),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(205, 0, 205),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 205, 205),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(229, 229, 229),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(127, 127, 127),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 0, 0),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 255, 0),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 255, 0),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 0, 255),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 0, 255),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 255, 255),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 255, 255),
    };
    int line = scanvideo_scanline_number(dest->scanline_id);
    uint8_t *data = xram + line * 160;
    uint16_t *pbuf = (void *)dest->data;
    ++pbuf;
    for (int i = 0; i < 160;)
    {
        *++pbuf = colors[(*data) & 0xF];
        *++pbuf = colors[(*data) >> 4];
        ++data;
        ++i;
    }
    uint32_t *buf = (void *)dest->data;
    buf[0] = COMPOSABLE_RAW_RUN | (buf[1] << 16);
    buf[1] = 317 | (buf[1] & 0xFFFF0000);
    buf[161] = COMPOSABLE_RAW_1P | 0;
    buf[162] = COMPOSABLE_EOL_SKIP_ALIGN;
    dest->data_used = 163;
    dest->status = SCANLINE_OK;
}

static void vga_render_320_240(void)
{
    struct scanvideo_scanline_buffer *scanline_buffer;
    for (int i = 0; i < 240; i++)
    {
        scanline_buffer = scanvideo_begin_scanline_generation(true);
        vga_render_4bpp(scanline_buffer);
        scanvideo_end_scanline_generation(scanline_buffer);
    }
}

static void vga_render_640_480(void)
{
    struct scanvideo_scanline_buffer *scanline_buffer;
    for (int i = 0; i < 480; i++)
    {
        scanline_buffer = scanvideo_begin_scanline_generation(true);
        vga_render_color_bar(scanline_buffer);
        scanvideo_end_scanline_generation(scanline_buffer);
    }
}

static void vga_render_320_180(void)
{
    struct scanvideo_scanline_buffer *scanline_buffer;
    for (int i = 0; i < 180; i++)
    {
        scanline_buffer = scanvideo_begin_scanline_generation(true);
        vga_render_4bpp(scanline_buffer);
        scanvideo_end_scanline_generation(scanline_buffer);
    }
}

static void vga_render_640_360(void)
{
    struct scanvideo_scanline_buffer *scanline_buffer;
    for (int i = 0; i < 360; i++)
    {
        scanline_buffer = scanvideo_begin_scanline_generation(true);
        vga_render_color_bar(scanline_buffer);
        scanvideo_end_scanline_generation(scanline_buffer);
    }
}

static void vga_render_loop(void)
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

static void vga_find_mode(void)
{
    // terminal_selected mode goes first
    if (vga_terminal_selected)
    {
        if (vga_display_selected == vga_sxga)
            vga_mode_selected = &vga_mode_640x512_sxga;
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
    else if (vga_resolution_selected == vga_640_480)
    {
        if (vga_display_selected == vga_sxga)
            vga_mode_selected = &vga_mode_640x480_sxga;
        else
            vga_mode_selected = &vga_mode_640x480;
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
    else if (vga_resolution_selected == vga_640_360)
    {
        if (vga_display_selected == vga_sxga)
            vga_mode_selected = &vga_mode_640x360_sxga;
        else if (vga_display_selected == vga_hd)
            vga_mode_selected = &vga_mode_640x360_hd;
        else
            vga_mode_selected = &vga_mode_640x360;
    }
    // trigger only if change detected
    if ((vga_mode_selected != vga_mode_current) ||
        (vga_terminal_selected != vga_terminal_current))
        vga_mode_switch_triggered = true;
}

static void vga_set(void)
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
        main_reclock();
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

void vga_task(void)
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

static void vga_memory_init(void)
{
    // Thought provoking scaffolding
    strcpy(&xram[45], "**** PICOCOMPUTER 6502 V1 ****");
    strcpy(&xram[121], "64K RAM SYSTEM  53248 BASIC BYTES FREE");
    strcpy(&xram[200], "READY");
    xram[240] = 32 + 128;
    // Rotate font as we copy it into video memory
    for (int i = 0; i < 1024; i += 8)
        for (int x = 0; x < 8; x++)
            for (int y = 0; y < 8; y++)
                xram[0x1000 + i + y] =
                    (xram[0x1000 + i + y] << 1) | ((haxscii[i + x] & (0x01 << y)) >> y);
    // PETSCII 128-255 are inverse
    for (int i = 0; i < 1024; i += 1)
        xram[0x1400 + i] = ~xram[0x1000 + i];
}

void vga_init(void)
{
    // safety check for compiler alignment
    assert(!((uintptr_t)xram & 0xFFFF));

    vga_memory_init();
    mutex_init(&vga_mutex);
    vga_display(vga_sd);
    vga_resolution(vga_320_240);
    vga_terminal(true);
    vga_set();
    vga_mode_switch_triggered = false;
    multicore_launch_core1(vga_render_loop);
}
