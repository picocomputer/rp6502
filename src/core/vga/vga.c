/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "core/mem/mem.h"
#include "core/pix.h"
#include "core/sys/main.h"
#include "core/ria/ria.h"
#include "core/vga/vga_emu.h"
#include "core/vga/prog.h"
#include "core/vga/mode0.h"
#include "core/term/term.h"
#include "core/term/font.h"
#include "core/vga/pixel_format.h"
#include <string.h>

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

bool vga_connected(void)
{
    return true;
}

uint8_t vga_get_display_type(void)
{
    /* 2 selects the 32-row text geometry in rln; the console is 30 rows. */
    return 1;
}


/* A software renderer keeps its programming in the scanline table, and that
 * is the whole of what it has to forget. Nothing else needs telling: what
 * draws next reads the canvas when it is asked to. */
void vga_canvas_reset(void)
{
    vga_prog_reset();
}

void vga_canvas_publish(vga_canvas_t canvas)
{
    (void)canvas;
}

/* Software renders from the plane it is handed, so there is nothing to
 * publish ahead of one. */
void vga_mode_begin(uint8_t mode, uint16_t attr)
{
    (void)mode;
    (void)attr;
}

void vga_set_code_page(uint16_t cp)
{
    font_set_code_page(cp);
}

void vga_init(void)
{
    vga_canvas_select(0); /* console = 640x480, installs the term program */
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
        /* The RIA-private control channel, which on that machine crosses
         * the bus. Here the VGA is the same binary, so it is the call the
         * message would have become. */
        main_xreg_1(0xF, 0x00, vga_get_display_type());
    }
}

int16_t vga_vsync_scanline(void)
{
    /* Mirror the firmware (vga_scanline_complete): vsync fires at the highest
     * scanline any program renders, clamped to / falling back to the canvas
     * height (the visible region) — not the full 525-line frame. */
    if (vga_prog_highest() > 0 && vga_prog_highest() <= vga_canvas_height())
        return vga_prog_highest();
    return vga_canvas_height();
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

uint32_t *vga_get_framebuffer(void)
{
    return g_framebuffer;
}

/* Render ONE scanline y of the canvas into fb at the canvas's native stride
 * (the canvas width). Each plane runs its fill and then its own sprites — slot k's
 * sprites belong to plane k, over a zeroed buffer when no fill ran. (The RIA
 * firmware paints sprites into the lowest filled buffer to skip the memset;
 * that is a bandwidth optimization whose artifacts are not modeled.) The
 * planes composite as scanvideo's PIO does — plane 0 is the unconditional
 * base, black when unfilled, and higher planes overlay where their pixel's
 * alpha bit is set, so e.g. a sprite layer shows through the transparent
 * background of a text layer above it. */
static void render_scanline(int y, uint32_t *fb)
{
    const int W = vga_canvas_width();
    uint16_t plane[SCANVIDEO_PLANE_COUNT][VGA_MAX_WIDTH];
    const vga_prog_t *p = vga_prog_row((int16_t)y);
    bool filled[SCANVIDEO_PLANE_COUNT] = {false, false, false};
    for (int i = 0; i < SCANVIDEO_PLANE_COUNT; i++)
    {
        if (p->fill_fn[i])
            filled[i] = p->fill_fn[i](i, (int16_t)y, (int16_t)W, plane[i], p->fill_config[i]);
        if (p->sprite_fn[i])
        {
            if (!filled[i])
            {
                memset(plane[i], 0, (size_t)W * sizeof(uint16_t));
                filled[i] = true;
            }
            p->sprite_fn[i]((int16_t)y, (int16_t)W, plane[i], p->sprite_config[i], p->sprite_length[i]);
        }
    }

    uint32_t *dst = fb + (size_t)y * W;
    for (int x = 0; x < W; x++)
    {
        uint16_t px = filled[0] ? plane[0][x] : 0;
        for (int i = 1; i < SCANVIDEO_PLANE_COUNT; i++)
            if (filled[i] && (plane[i][x] & SCANVIDEO_ALPHA_MASK))
                px = plane[i][x];
        dst[x] = rgb555_to_rgba8(px);
    }
}

/* Render scanline y of the current frame into the registered framebuffer,
 * interleaved with the CPU by sys_run_frame so mid-frame state changes land on
 * later lines (raster effects), matching the real per-scanline VGA scanout. */
void vga_render_scanline(int y)
{
    if (g_framebuffer)
        render_scanline(y, g_framebuffer);
}
