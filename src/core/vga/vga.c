/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "core/aud/mix.h"
#include "core/com/com.h"
#include "core/com/manifold.h"
#include "core/ria/regs.h"
#include "core/sys/pix.h"
#include "core/sys/driver.h"
#include "core/ria/ria.h"
#include "core/vga/vga_emu.h"
#include "core/vga/prog.h"
#include "core/term/font.h"
#include "core/wdc/bus.h"
#include "core/dap/dbg.h"
#include "core/vga/pixel_format.h"
#include "core/sys/sys.h"
#include "host/host.h"
#include <assert.h>
#include <string.h>

/* The line within the frame, the frame count and the clock are computed from
 * beam_n rather than accumulated alongside it, so none can drift away from
 * it. */
static uint64_t beam_n;
static bool vsynced;
static bool vga_needs_reset;
static bool vga_scanout = true;
static uint32_t *g_framebuffer;

/* The emulators render one line at a time, so one set of caches serves. A
 * row of text is forgotten before every line, which builds its pairs again:
 * a color then shows on the very next line, and a savestate, which carries
 * no cache, loads into the picture the machine that saved it would draw. */
static vga_fill_cache_t vga_fill_cache[SCANVIDEO_PLANE_COUNT];

#define VGA_SST(X)                                                     \
    X(U64, beam_n)                                                     \
    X(BOOL, vsynced)                                                   \
    X(BOOL, vga_needs_reset)                                           \
    X(SKIP, vga_scanout, "whether this host wants pixels at all")      \
    X(SKIP, g_framebuffer, "the host's; no row touches the pixels")    \
    X(SKIP, vga_fill_cache, "keyed, and a row of text is built again " \
                            "on every line")
SST_TABLE(vga, VGA_SST)

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

void vga_canvas_reset(void)
{
    vga_prog_reset();
}

void vga_canvas_publish(vga_canvas_t canvas)
{
    (void)canvas;
}

void vga_set_code_page(uint16_t cp)
{
    font_set_code_page(cp);
}

void vga_load_code_page(uint16_t cp)
{
    font_load_code_page(cp);
}

void vga_init(void)
{
    vga_canvas_select(0);
}

static void vga_render_scanline(int y);

void vga_stop(void)
{
    vga_needs_reset = true;
}

/* A program starting before the reset after the last stop takes it now,
 * after what was printed before the stop and the session's end, which COM's
 * RUN ended first, so the reset cannot land in the new program. */
void vga_run(void)
{
    if (!vga_needs_reset)
        return;
    manifold_flush();
    vga_needs_reset = false;
    vga_console_reset();
}

void vga_reset(void)
{
    /* CANVAS_DRIVER's reset, before this one, has put back a console size
     * a host gives. The console reset calls into the cells, whose RESET
     * runs after this one, so it waits for vga_task. That clears the table
     * and its rows before it draws one. */
    vga_needs_reset = true;
}

#define BEAM_US_NUM 2000ull
#define BEAM_US_DEN 63ull
static_assert(BEAM_US_NUM * ((uint64_t)VGA_HZ * VGA_SCANLINES) ==
                  BEAM_US_DEN * 1000000ull,
              "a scanline must be 2000/63 microseconds");

uint64_t host_clock_us(void)
{
    /* The division is exact on every 63rd line, and a second is 31500 lines,
     * so a second of frames comes out as exactly a second. */
    return beam_n * BEAM_US_NUM / BEAM_US_DEN;
}

void vga_set_scanout(bool on) { vga_scanout = on; }

uint64_t vga_beam_lines(void) { return beam_n; }

unsigned long vga_frame_count(void)
{
    return (unsigned long)(beam_n / VGA_SCANLINES);
}

bool vga_run_frame(void)
{
    const unsigned long want = vga_frame_count() + 1;
    bool whole = true;
    while (vga_frame_count() != want)
    {
        if (dbg_is_stopped() && sys_running())
        {
            whole = false;
            break;
        }
        sys_task();
        sys_commit();
    }
    aud_step();
    return whole;
}

void vga_task(void)
{
    /* The reset waits for everything printed before the stop to go out,
     * and for the session's end after it, as a Pico's does. */
    if (vga_needs_reset && manifold_drained() && !com_ending())
    {
        vga_needs_reset = false;
        vga_console_reset();
    }
    /* A debugger holding the 6502 holds the whole machine. Left running, the
     * beam would go on counting frames and latching $FFF0 bit 7 while a
     * program sat at a breakpoint, and stepping one instruction would resume
     * into an interrupt storm the program never lived through. After an exit
     * there is no 6502 to hold, so the stop presented at exit leaves the beam
     * running and the final screen is drawn. */
    if (dbg_is_stopped() && sys_running())
        return;
    /* The line is drawn from the machine as it stands before the 6502 cycles
     * for that scanline have run, because the 6502 catches up to the beam
     * afterwards, so a write it makes lands on a later line. */
    const int16_t line = (int16_t)(beam_n % VGA_SCANLINES);
    const int16_t y_scale = vga_canvas_y_scale();
    const int16_t row = line / y_scale;
    if (vga_scanout && line % y_scale == 0 && row < vga_canvas_height())
        vga_render_scanline(row);
    beam_n++;
    if (!vsynced && line + 1 >= vga_vsync_line())
    {
        REGS(0xFFE3) = (uint8_t)(REGS(0xFFE3) + 1); /* the VSYNC counter */
        ria_trigger_vsync();
        vsynced = true;
    }
    if (beam_n % VGA_SCANLINES == 0)
        vsynced = false;
}

void vga_set_framebuffer(uint32_t *fb)
{
    g_framebuffer = fb;
}

uint32_t *vga_get_framebuffer(void)
{
    return g_framebuffer;
}

bool vga_frame_crc(uint32_t *crc)
{
    if (!g_framebuffer)
        return false;
    int w, h;
    vga_canvas_size(&w, &h);
    *crc = host_crc32(0, g_framebuffer, (size_t)w * (size_t)h * sizeof *g_framebuffer);
    return true;
}

/* The planes composite the way scanvideo's PIO does: plane 0 is the base, black
 * where it is unfilled, and a higher plane replaces it only where that plane's
 * pixel has the alpha bit set, because the PIO drives an overlay state
 * machine's pins only while that bit is high.
 *
 * The VGA firmware diverges here: it paints a plane's sprites into the buffer
 * of the most recently filled plane at or below it, not into the plane's
 * own. */
static void render_scanline(int y, uint32_t *fb)
{
    const int W = vga_canvas_width();
    uint16_t plane[SCANVIDEO_PLANE_COUNT][VGA_MAX_WIDTH];
    const vga_prog_t *p = vga_prog_row((int16_t)y);
    bool filled[SCANVIDEO_PLANE_COUNT] = {false, false, false};
    for (int i = 0; i < SCANVIDEO_PLANE_COUNT; i++)
    {
        if (p->fill_fn[i])
        {
            vga_fill_cache_forget_row(&vga_fill_cache[i]);
            filled[i] = p->fill_fn[i](p->fill_options[i], (int16_t)y, (int16_t)W, plane[i],
                                      p->fill_config[i], &vga_fill_cache[i]);
        }
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

static void vga_render_scanline(int y)
{
    if (g_framebuffer)
        render_scanline(y, g_framebuffer);
}
