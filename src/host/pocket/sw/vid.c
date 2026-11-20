/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The console's canvas and code page, and the frame count. mode0.sv draws
 * the console from core/vga/mode/mode0.c's cells and words, which cells.c
 * writes and link.ld places on its memories.
 */

#include "core/api/xreg.h"
#include "core/com/com.h"
#include "core/com/manifold.h"
#include "mmio.h"
#include "vga.h"
#include "vid.h"

#include <stdint.h>

/* FONT_DRIVER's INIT has written the glyphs by now: link.ld places
 * font.c's tables on the font memories the fabric draws from. */
void vid_init(void)
{
    vga_canvas_select(0);
}

/* A save taken between a stop and vid_task carries the restore still owed,
 * so the loading machine puts the console back as the saving one would. */
static bool vid_needs_restore;

#define VID_SST(X) X(BOOL, vid_needs_restore)
SST_TABLE(vid, VID_SST)

static void vid_restore(void)
{
    vid_needs_restore = false;
    xreg1(0x0F, 0x01, 437);
    vga_console_reset();
}

/* The restore waits for everything printed before the stop to go out at the
 * console line's pace, and for the session's end after it, as the
 * emulators' and a Pico's do. */
void vid_task(void)
{
    if (vid_needs_restore && manifold_drained() && !com_ending())
        vid_restore();
}

/* A program starting before the restore after the last stop takes it now,
 * after what was printed before the stop and the session's end, which COM's
 * RUN ended first. */
void vid_run(void)
{
    if (!vid_needs_restore)
        return;
    manifold_flush();
    vid_restore();
}

unsigned long vga_frame_count(void)
{
    return VID_FRAME;
}

/* The console's code page and canvas are put back when a program stops.
 * Both are deferred to vid_task so that they land after every other stop hook
 * has run, which a stop hook itself could ensure only from the first row of
 * the driver list, because stop hooks run in reverse order. A failed
 * savestate load resets through here too: vga_console_reset puts back the
 * canvas, clears the scanline table and the console's mask, and sets the
 * console's plane, so no program's booking meets the load's values, since
 * vid_task runs before the next program starts. */
void vid_stop(void)
{
    vid_needs_restore = true;
}
