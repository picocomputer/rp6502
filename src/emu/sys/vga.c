/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/sys/mem.h"
#include "emu/sys/vga.h"
#include "emu/sys/pix.h"
#include "emu/chips/rp6502.h"
#include "emu/sys/vga.h"
#include "vga/term/term.h"
#include "vga/term/font.h"
#include "vga/scanvideo/pixel_format.h"
#include <string.h>

/* Current canvas geometry. The boot console is 640x480. */
static int16_t g_canvas_w = VGA_MAX_WIDTH;
static int16_t g_canvas_h = VGA_MAX_HEIGHT;
static vga_canvas_t g_canvas_code = vga_canvas_console;

/* Per-scanline render programming, indexed [scanline]. Each scanline holds up
 * to SCANVIDEO_PLANE_COUNT planes; a plane may carry a fill renderer, a sprite
 * renderer, or both (sprites draw over the fill). Mirrors firmware vga_prog. */
typedef bool (*fill_fn_t)(int16_t plane_id, int16_t scanline, int16_t width,
                          uint16_t *rgb, uint16_t config_ptr);
typedef void (*sprite_fn_t)(int16_t scanline, int16_t width, uint16_t *rgb,
                            uint16_t config_ptr, uint16_t length);

typedef struct
{
    fill_fn_t fill_fn[SCANVIDEO_PLANE_COUNT];
    uint16_t fill_config[SCANVIDEO_PLANE_COUNT];
    sprite_fn_t sprite_fn[SCANVIDEO_PLANE_COUNT];
    uint16_t sprite_config[SCANVIDEO_PLANE_COUNT];
    uint16_t sprite_length[SCANVIDEO_PLANE_COUNT];
} vga_prog_t;
static vga_prog_t g_prog[VGA_PROG_MAX];

/* Highest scanline any program renders; vsync fires here (firmware parity). */
static int16_t g_highest_scanline;

/* RGB555(+alpha bit) -> RGBA8 (0xAABBGGRR). Computed inline rather than through a
 * 256 KB value-indexed table: the shifts vectorize, and keeping the cache free
 * for the CPU core and framebuffer beats a table that thrashes on color-rich
 * content (and it's a large fraction of L2 on the ARM/WASM targets). */
static inline uint32_t rgb555_to_rgba8(uint16_t px)
{
    uint32_t r5 = SCANVIDEO_R5_FROM_PIXEL(px);
    uint32_t g5 = SCANVIDEO_G5_FROM_PIXEL(px);
    uint32_t b5 = SCANVIDEO_B5_FROM_PIXEL(px);
    uint32_t r = (r5 << 3) | (r5 >> 2);
    uint32_t g = (g5 << 3) | (g5 >> 2);
    uint32_t b = (b5 << 3) | (b5 >> 2);
    return r | (g << 8) | (b << 16) | 0xFF000000u;
}

int16_t vga_canvas_height(void)
{
    return g_canvas_h;
}

bool vga_connected(void)
{
    return true;
}

vga_canvas_t vga_get_canvas(void)
{
    return g_canvas_code;
}

uint8_t vga_get_display_type(void)
{
    /* 2 selects the 32-row text geometry in rln; the console is 30 rows. */
    return 1;
}

/* Validate a prog request exactly as firmware vga_prog_valid: a zero end means
 * "to the bottom of the canvas", then bound the plane and scanline window and
 * track the highest scanline rendered (for vsync). */
static bool vga_prog_valid(int16_t plane, int16_t scanline_begin, int16_t *scanline_end)
{
    if (!*scanline_end)
        *scanline_end = g_canvas_h;
    if (plane < 0 || plane >= SCANVIDEO_PLANE_COUNT ||
        scanline_begin < 0 || *scanline_end > g_canvas_h ||
        *scanline_end - scanline_begin < 1)
        return false;
    if (*scanline_end > g_highest_scanline)
        g_highest_scanline = *scanline_end;
    return true;
}

bool vga_prog_fill(int16_t plane, int16_t scanline_begin, int16_t scanline_end,
                   uint16_t config_ptr, fill_fn_t fill_fn)
{
    if (g_canvas_code == vga_canvas_console) /* graphics modes need a canvas */
        return false;
    if (!vga_prog_valid(plane, scanline_begin, &scanline_end))
        return false;
    for (int16_t i = scanline_begin; i < scanline_end; i++)
    {
        g_prog[i].fill_config[plane] = config_ptr;
        g_prog[i].fill_fn[plane] = fill_fn;
    }
    return true;
}

bool vga_prog_exclusive(int16_t plane, int16_t scanline_begin, int16_t scanline_end,
                        uint16_t config_ptr, fill_fn_t fill_fn)
{
    if (!vga_prog_valid(plane, scanline_begin, &scanline_end))
        return false;
    /* Remove every prior instance of this fill_fn (term re-programs on resize). */
    for (uint16_t i = 0; i < VGA_PROG_MAX; i++)
        for (uint16_t j = 0; j < SCANVIDEO_PLANE_COUNT; j++)
            if (g_prog[i].fill_fn[j] == fill_fn)
                g_prog[i].fill_fn[j] = NULL;
    for (int16_t i = scanline_begin; i < scanline_end; i++)
    {
        g_prog[i].fill_config[plane] = config_ptr;
        g_prog[i].fill_fn[plane] = fill_fn;
    }
    return true;
}

bool vga_prog_sprite(int16_t plane, int16_t scanline_begin, int16_t scanline_end,
                     uint16_t config_ptr, uint16_t length, sprite_fn_t sprite_fn)
{
    if (g_canvas_code == vga_canvas_console)
        return false;
    if (!vga_prog_valid(plane, scanline_begin, &scanline_end))
        return false;
    for (int16_t i = scanline_begin; i < scanline_end; i++)
    {
        g_prog[i].sprite_config[plane] = config_ptr;
        g_prog[i].sprite_length[plane] = length;
        g_prog[i].sprite_fn[plane] = sprite_fn;
    }
    return true;
}

/* Map a canvas code to its pixel geometry (see vga/sys/vga.h vga_canvas_t) and
 * clear all programming, mirroring firmware vga_xreg_canvas. The console canvas
 * reinstalls the terminal program so a return to it keeps rendering. An
 * out-of-range code is rejected (false) with no state change, as the firmware
 * NAKs it. */
bool vga_set_canvas(uint16_t canvas)
{
    switch (canvas)
    {
    case 1: /* vga_320_240 */
        g_canvas_w = 320, g_canvas_h = 240;
        break;
    case 2: /* vga_320_180 */
        g_canvas_w = 320, g_canvas_h = 180;
        break;
    case 4: /* vga_640_360 */
        g_canvas_w = 640, g_canvas_h = 360;
        break;
    case 0: /* vga_console */
    case 3: /* vga_640_480 */
        g_canvas_w = 640, g_canvas_h = 480;
        break;
    default:
        return false;
    }
    g_canvas_code = (vga_canvas_t)canvas;
    memset(g_prog, 0, sizeof(g_prog));
    g_highest_scanline = 0;
    if (canvas == vga_canvas_console)
    {
        uint16_t xregs[8] = {0};
        term_prog(xregs); /* console term across the whole canvas */
    }
    return true;
}

void vga_set_code_page(uint16_t cp)
{
    font_set_code_page(cp);
}

void vga_init(void)
{
    vga_set_canvas(0); /* console = 640x480, installs the term program */
}

static bool vga_needs_reset;

void vga_stop(void)
{
    /* Reset only on a real program stop (firmware vga_stop). ria_active() is
     * always false in the emu — no chunked fast-loads — so every main_stop is an
     * idle stop that arms, exactly as the firmware's exec/exit stop does. */
    if (!ria_active())
        vga_needs_reset = true;
}

void vga_task(void)
{
    if (vga_needs_reset)
    {
        vga_needs_reset = false;
        pix_send_blocking(PIX_DEVICE_VGA, 0xF, 0x00, vga_get_display_type());
    }
}

int vga_vsync_scanline(void)
{
    /* Mirror the firmware (vga_scanline_complete): vsync fires at the highest
     * scanline any program renders, clamped to / falling back to the canvas
     * height (the visible region) — not the full 525-line frame. */
    if (g_highest_scanline > 0 && g_highest_scanline <= g_canvas_h)
        return g_highest_scanline;
    return g_canvas_h;
}

/* Current canvas pixel size (≤ VGA_MAX_WIDTH x VGA_MAX_HEIGHT). The presentation
 * layer reads this to size its texture and scale the canvas to the display. */
void vga_canvas_size(int *w, int *h)
{
    *w = g_canvas_w;
    *h = g_canvas_h;
}

/* The app-owned framebuffer the scanlines render into (the window's texture
 * staging, main.c's screenshot buffer, a test's assertion buffer). The owner
 * registers storage for the largest canvas before running frames; sokol's
 * swapchain provides the display double-buffering, so one buffer suffices. */
static uint32_t *g_framebuffer;

void vga_set_framebuffer(uint32_t *fb)
{
    g_framebuffer = fb;
}

/* Render ONE scanline y of the canvas into fb at the canvas's native stride
 * (g_canvas_w). Mirrors firmware vga_render_scanline: run each plane's fill then
 * sprite, where sprites draw onto the current "foreground" (the most recently
 * filled plane, or their own zeroed buffer if none); the filled planes are then
 * composited bottom-to-top — the lowest is the opaque base and higher planes
 * overlay where their pixel's alpha bit is set, so e.g. a sprite layer shows
 * through the transparent background of a text layer above it. */
static void render_scanline(int y, uint32_t *fb)
{
    const int W = g_canvas_w;
    uint16_t plane[SCANVIDEO_PLANE_COUNT][VGA_MAX_WIDTH];
    const vga_prog_t *p = &g_prog[y];
    bool filled[SCANVIDEO_PLANE_COUNT] = {false, false, false};
    uint16_t *foreground = NULL;
    for (int i = 0; i < SCANVIDEO_PLANE_COUNT; i++)
    {
        if (p->fill_fn[i])
        {
            filled[i] = p->fill_fn[i](i, (int16_t)y, (int16_t)W, plane[i], p->fill_config[i]);
            if (filled[i])
                foreground = plane[i];
        }
        if (p->sprite_fn[i])
        {
            if (!foreground)
            {
                foreground = plane[i];
                memset(foreground, 0, (size_t)W * sizeof(uint16_t));
                filled[i] = true;
            }
            p->sprite_fn[i]((int16_t)y, (int16_t)W, foreground, p->sprite_config[i], p->sprite_length[i]);
        }
    }

    uint32_t *dst = fb + (size_t)y * W;
    int base = -1;
    for (int i = 0; i < SCANVIDEO_PLANE_COUNT; i++)
        if (filled[i])
        {
            base = i;
            break;
        }
    if (base < 0)
    {
        for (int x = 0; x < W; x++)
            dst[x] = 0xFF000000u;
        return;
    }
    for (int x = 0; x < W; x++)
    {
        uint16_t px = plane[base][x];
        for (int i = base + 1; i < SCANVIDEO_PLANE_COUNT; i++)
            if (filled[i] && (plane[i][x] & SCANVIDEO_ALPHA_MASK))
                px = plane[i][x];
        dst[x] = rgb555_to_rgba8(px);
    }
}

/* Render scanline y of the current frame into the registered framebuffer,
 * interleaved with the CPU by main_run_frame so mid-frame state changes land on
 * later lines (raster effects), matching the real per-scanline VGA scanout. */
void vga_render_scanline(int y)
{
    if (g_framebuffer)
        render_scanline(y, g_framebuffer);
}
