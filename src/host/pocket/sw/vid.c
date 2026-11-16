/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The console's canvas and code page, the frame count, and the canvas's
 * savestate row. mode0.sv draws the console from core/vga/mode/mode0.c's
 * cells and words, which cells.c writes and link.ld places on its
 * memories.
 */

#include "core/api/xreg.h"
#include "mmio.h"
#include "vga.h"
#include "vid.h"

#include "core/str/unicode.h"
#include "core/term/cells.h"
#include "core/term/font.h"

#include <stdint.h>

/* link.ld places font.c's tables on the font memories the fabric draws
 * from, so font_init writes them there. */
void vid_init(void)
{
    font_init();
    vga_canvas_select(0);
}

static bool vid_needs_restore;

void vid_task(void)
{
    if (vid_needs_restore)
    {
        vid_needs_restore = false;
        xreg1(0x0F, 0x01, 437);
        vga_console_reset();
    }
}

unsigned long vga_frame_count(void)
{
    return VID_FRAME;
}

/* The console's code page and canvas are put back when a program stops.
 * Both are deferred to vid_task so that they land after every other stop hook
 * has run, which a stop hook itself could ensure only from the first row of
 * the driver list, because stop hooks run in reverse order. */
void vid_stop(void)
{
    vid_needs_restore = true;
}

/* The fabric's registers here are written, not read, so the row carries
 * what was written: the canvas and vga.c's record of the scanline table.
 * The font memories cannot be read either, so it carries the code page and
 * the load writes its glyphs again. The cells come back in their own row,
 * before this one, and the load publishes them for the canvas it puts
 * back. */
void vid_sst_save(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    sst_put_u16(c, (uint16_t)vga_canvas_code());
    sst_put_u16(c, font_get_code_page());
    vga_table_save(c);
}

bool vid_sst_load(sst_cursor_t *c, unsigned flags)
{
    (void)flags;
    uint16_t canvas = sst_get_u16(c);
    uint16_t cp = sst_get_u16(c);
    if (!sst_ok(c))
        return false;
    if (!unicode_has_page(cp))
        return sst_refuse(c, "a code page this build lacks");
    if (!vga_canvas_load(canvas))
        return sst_refuse(c, "a canvas this build lacks");
    if (!vga_table_load(c))
        return sst_refuse(c, "the scanline table's notes are out of range");
    font_load_code_page(cp);
    cells_publish();
    vid_needs_restore = false;
    return true;
}
