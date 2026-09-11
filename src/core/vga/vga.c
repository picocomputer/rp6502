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
#include "core/vga/mode/mode1.h"
#include "core/vga/mode/mode2.h"
#include "core/vga/mode/mode3.h"
#include "core/vga/mode/mode4.h"
#include "core/vga/mode/mode5.h"
#include "core/term/term.h"
#include "core/term/font.h"
#include "core/wdc/bus.h"
#include "core/dap/dbg.h"
#include "core/vga/pixel_format.h"
#include "core/sys/sys.h"
#include "host/host.h"
#include <assert.h>
#include <string.h>

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
    return 1;
}

void vga_canvas_reset(void)
{
    vga_prog_reset();
}

void vga_canvas_publish(vga_canvas_t canvas)
{
    (void)canvas;
}

void vga_mode_begin(uint8_t mode, uint16_t attr)
{
    (void)mode;
    (void)attr;
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

static bool vga_needs_reset;

void vga_stop(void)
{
    /* ria_active() is a constant false here, so every stop resets. The test
     * is the RIA firmware's, where a stop that only closes a transfer must
     * not reset the console. */
    if (!ria_active())
        vga_needs_reset = true;
}

static void vga_render_scanline(int y);

/* The line within the frame and the clock are computed from beam_n rather than
 * accumulated alongside it, so neither can drift away from it. */
static uint64_t beam_n;
static unsigned long frame_n;
static bool vsynced;

static bool vga_scanout = true;

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

unsigned long vga_frame_count(void) { return frame_n; }

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

vga_fill_fn_t vga_mode_fill_fn(uint8_t mode, uint16_t attributes)
{
    switch (mode)
    {
    case 0: return mode0_fill_fn(attributes);
    case 1: return mode1_fill_fn(attributes);
    case 2: return mode2_fill_fn(attributes);
    case 3: return mode3_fill_fn(attributes);
    default: return NULL;
    }
}

bool vga_mode_fill_id(vga_fill_fn_t fn, int16_t scanline, int16_t plane,
                      uint8_t *mode, uint16_t *attributes)
{
    if (!fn)
    {
        *mode = VGA_MODE_NONE;
        *attributes = 0;
        return true;
    }
    if (mode0_fill_attr(fn, attributes)) { *mode = 0; return true; }
    if (mode1_fill_attr(fn, attributes)) { *mode = 1; return true; }
    if (mode3_fill_attr(fn, attributes)) { *mode = 3; return true; }
    if (fn == mode2_fill_fn(0) && mode2_fill_attr(scanline, plane, attributes))
    {
        *mode = 2;
        return true;
    }
    return false;
}

vga_sprite_fn_t vga_mode_sprite_fn(uint8_t mode, uint16_t attributes)
{
    switch (mode)
    {
    case 4: return mode4_sprite_fn(attributes);
    case 5: return mode5_sprite_fn(attributes);
    default: return NULL;
    }
}

bool vga_mode_sprite_id(vga_sprite_fn_t fn, uint8_t *mode, uint16_t *attributes)
{
    if (!fn)
    {
        *mode = VGA_MODE_NONE;
        *attributes = 0;
        return true;
    }
    if (mode4_sprite_attr(fn, attributes)) { *mode = 4; return true; }
    if (mode5_sprite_attr(fn, attributes)) { *mode = 5; return true; }
    return false;
}

void vga_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put_u64(c, beam_n);
    sst_put_bool(c, vsynced);
    sst_put_bool(c, vga_needs_reset);
    sst_put_u16(c, (uint16_t)vga_canvas_code());
    sst_put_u16(c, (uint16_t)vga_prog_highest());
    sst_put_u16(c, (uint16_t)mode0_begin());
    for (int16_t line = 0; line < VGA_SST_ROWS; line++)
    {
        const vga_prog_t *row = vga_prog_row(line);
        for (int16_t plane = 0; plane < SCANVIDEO_PLANE_COUNT; plane++)
        {
            uint8_t mode;
            uint16_t attr;
            if (!vga_mode_fill_id(row->fill_fn[plane], line, plane, &mode, &attr))
                mode = VGA_MODE_NONE, attr = 0;
            sst_put_u8(c, mode);
            sst_put_u16(c, attr);
            sst_put_u16(c, row->fill_config[plane]);
            if (!vga_mode_sprite_id(row->sprite_fn[plane], &mode, &attr))
                mode = VGA_MODE_NONE, attr = 0;
            sst_put_u8(c, mode);
            sst_put_u16(c, attr);
            sst_put_u16(c, row->sprite_config[plane]);
            sst_put_u16(c, row->sprite_length[plane]);
        }
    }
}

bool vga_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    uint64_t beam = sst_get_u64(c);
    bool synced = sst_get_bool(c);
    bool reset = sst_get_bool(c);
    uint16_t canvas = sst_get_u16(c);
    uint16_t highest = sst_get_u16(c);
    uint16_t term_begin = sst_get_u16(c);
    if (!sst_ok(c) || highest > VGA_SST_ROWS || term_begin > VGA_SST_ROWS)
        return false;

    /* Every renderer the table names is resolved before any row is installed,
     * so a blob naming a mode this build does not have refuses the load
     * before anything is mutated. That is why the cursor is saved here and
     * read a second time below. */
    const sst_cursor_t table = *c;
    for (int16_t line = 0; line < VGA_SST_ROWS; line++)
        for (int16_t plane = 0; plane < SCANVIDEO_PLANE_COUNT; plane++)
        {
            uint8_t mode = sst_get_u8(c);
            uint16_t attr = sst_get_u16(c);
            if (mode != VGA_MODE_NONE && !vga_mode_fill_fn(mode, attr))
                return false;
            sst_get_u16(c);
            mode = sst_get_u8(c);
            attr = sst_get_u16(c);
            if (mode != VGA_MODE_NONE && !vga_mode_sprite_fn(mode, attr))
                return false;
            sst_get_u16(c);
            sst_get_u16(c);
        }
    if (!sst_ok(c) || !vga_canvas_load(canvas))
        return false;

    beam_n = beam;
    frame_n = (unsigned long)(beam / VGA_SCANLINES);
    vsynced = synced;
    vga_needs_reset = reset;
    vga_prog_reset();
    *c = table;
    for (int16_t line = 0; line < VGA_SST_ROWS; line++)
    {
        vga_prog_t row;
        for (int16_t plane = 0; plane < SCANVIDEO_PLANE_COUNT; plane++)
        {
            uint8_t mode = sst_get_u8(c);
            uint16_t attr = sst_get_u16(c);
            row.fill_fn[plane] = mode == VGA_MODE_NONE ? NULL : vga_mode_fill_fn(mode, attr);
            row.fill_config[plane] = sst_get_u16(c);
            mode2_set_options(line, plane, mode == 2 ? attr : 0);
            mode = sst_get_u8(c);
            attr = sst_get_u16(c);
            row.sprite_fn[plane] = mode == VGA_MODE_NONE ? NULL : vga_mode_sprite_fn(mode, attr);
            row.sprite_config[plane] = sst_get_u16(c);
            row.sprite_length[plane] = sst_get_u16(c);
        }
        vga_prog_load_row(line, &row);
    }
    vga_prog_set_highest((int16_t)highest);
    mode0_set_begin((int16_t)term_begin);
    return true;
}

void vga_task(void)
{
    if (vga_needs_reset)
    {
        vga_needs_reset = false;
        /* Channel $F is the control channel the RIA reserves for itself, which
         * on a board crosses the PIX bus to the VGA. Here the VGA is the same
         * binary, so it is the call that message would have become. */
        xreg1(0xF, 0x00, vga_get_display_type());
    }
    /* A debugger holding the 6502 holds the whole machine. Left running, the
     * beam would go on counting frames and latching $FFF0 bit 7 while a
     * program sat at a breakpoint, and stepping one instruction would resume
     * into an interrupt storm the program never lived through. */
    if (dbg_is_stopped())
        return;
    /* The line is drawn from the machine as it stands before the cycles that
     * belong to it have run, because the 6502 catches up to the beam
     * afterwards, so a write it makes lands on a later line. */
    const int16_t line = (int16_t)(beam_n % VGA_SCANLINES);
    if (vga_scanout && line < vga_canvas_height())
        vga_render_scanline(line);
    beam_n++;
    if (!vsynced && line + 1 >= vga_vsync_line())
    {
        REGS(0xFFE3) = (uint8_t)(REGS(0xFFE3) + 1); /* the VSYNC counter */
        ria_trigger_vsync();
        vsynced = true;
    }
    if (beam_n % VGA_SCANLINES == 0)
    {
        vsynced = false;
        frame_n++;
    }
}


static uint32_t *g_framebuffer;

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

static void vga_render_scanline(int y)
{
    if (g_framebuffer)
        render_scanline(y, g_framebuffer);
}
