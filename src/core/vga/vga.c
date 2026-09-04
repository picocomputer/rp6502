/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "core/api/xreg.h"
#include "core/sys/ria.h"
#include "core/ria/regs.h"
#include "core/sys/pix.h"
#include "core/sys/driver.h"
#include "core/ria/ria.h"
#include "core/vga/vga_emu.h"
#include "core/vga/prog.h"
#include "core/vga/mode/mode0.h"
#include "core/term/term.h"
#include "core/term/font.h"
#include "core/wdc/bus.h"
#include "core/dap/dbg.h"
#include "core/vga/pixel_format.h"
#include "core/sys/sys.h"
#include <assert.h>
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
     * always false in the emu — no chunked fast-loads — so every sys_stop is an
     * idle stop that arms, exactly as the firmware's exec/exit stop does. */
    if (!ria_active())
        vga_needs_reset = true;
}

static int16_t vga_vsync_scanline(void);
static void vga_render_scanline(int y);

/* ---- the beam ------------------------------------------------------------
 *
 * Video leads and the 6502 follows: this advances the beam at most one
 * scanline per call, and cpu_task runs the machine up to wherever it got to.
 * On real hardware the two run at once; here they zip, one line at a time.
 */

/* Absolute scanline, never reset. Everything below is computed from it every
 * time rather than accumulated, so integer division introduces no drift. */
static uint64_t beam_n;
static unsigned long frame_n;
static bool vsynced;

/* The machine's clock is the beam, so this is host.h's contract: one
 * scanline is 1000000/(VGA_HZ*VGA_SCANLINES) microseconds, reduced to
 * 2000/63. Asserted against the constants it came from, because the two
 * silently disagreeing is the bug this prevents. Reduced also for range --
 * the unreduced form would overflow a uint64 in about nine days. */
#define BEAM_US_NUM 2000ull
#define BEAM_US_DEN 63ull
static_assert(BEAM_US_NUM * ((uint64_t)VGA_HZ * VGA_SCANLINES) ==
                  BEAM_US_DEN * 1000000ull,
              "a scanline must be 2000/63 microseconds");

uint64_t host_clock_us(void)
{
    /* Exact every 63 lines, so a second of frames is exactly a second. Do NOT
     * "fix" the inexact ratio with a per-line remainder: that double-corrects
     * and creates the drift it looks like it removes. */
    return beam_n * BEAM_US_NUM / BEAM_US_DEN;
}

uint64_t vga_beam_lines(void) { return beam_n; }

unsigned long vga_frame_count(void) { return frame_n; }

/* Run the machine until video says one frame went by. False when a
 * debugger holds it -- a held machine never will, and a caller must not
 * wait for it. */
bool vga_run_frame(void)
{
    const unsigned long want = frame_n + 1;
    while (frame_n != want)
    {
        if (dbg_is_stopped())
            return false;
        sys_task();
        sys_io_task();
        sys_commit();
    }
    return true;
}

void vga_task(void)
{
    if (vga_needs_reset)
    {
        vga_needs_reset = false;
        /* The RIA-private control channel, which on that machine crosses
         * the bus. Here the VGA is the same binary, so it is the call the
         * message would have become. */
        xreg1(0xF, 0x00, vga_get_display_type());
    }
    /* A debugger holding the 6502 holds the whole machine. Left running, the
     * beam would keep counting frames and firing vsync while a program sat at
     * a breakpoint, latching $FFF0 bit 7 each time -- so stepping one
     * instruction would resume into an IRQ storm the program never lived
     * through. The picture stays as it was; the window presents it again. */
    if (dbg_is_stopped())
        return;
    /* Draw the line from the machine state as it stands now, before the
     * cycles that belong to it have run -- the CPU catches up to the beam
     * afterwards, so a write lands on later lines. Real per-scanline scanout. */
    const int16_t line = (int16_t)(beam_n % VGA_SCANLINES);
    if (line < vga_canvas_height())
        vga_render_scanline(line);
    beam_n++;
    if (!vsynced && line + 1 >= vga_vsync_scanline())
    {
        REGS(0xFFE3) = (uint8_t)(REGS(0xFFE3) + 1); /* VSYNC counter, 8-bit wrap */
        ria_trigger_vsync(); /* latch $FFF0 bit7; IRQ only if the program enabled it */
        vsynced = true;
    }
    if (beam_n % VGA_SCANLINES == 0)
    {
        vsynced = false;
        frame_n++;
    }
}

static int16_t vga_vsync_scanline(void)
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
 * interleaved with the CPU a line at a time, so mid-frame state changes land on
 * later lines (raster effects), matching the real per-scanline VGA scanout. */
static void vga_render_scanline(int y)
{
    if (g_framebuffer)
        render_scanline(y, g_framebuffer);
}
