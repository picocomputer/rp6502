/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The Pico VGA's console screen (core/term/draw.c), built as its machine.h
 * builds it: a job left pending and done 11 rows a pass of the main loop
 * (TERM_JOB_ROWS), and mode 0's words written once a frame at most
 * (DRAW_PUBLISH_PER_FRAME). The selection, which reaches the VGA from the
 * RIA with no byte of the console (vterm_select_apply). */

#include "core/aud/bel.h"
#include "core/com/com.h"
#include "core/term/draw.h"
#include "core/term/vterm.h"
#include "core/term/vterm_mouse.h"
#include "core/vga/mode/mode0.h"
#include "core/vga/vga.h"
#include "utest.h"
#include <string.h>

_Static_assert(TERM_MAX_HEIGHT == 32 && TERM_ALT_SCREEN == 1 && TERM_JOB_ROWS == 11 &&
                   DRAW_PUBLISH_PER_FRAME == 1,
               "the Pico VGA's console");

/* ---- the VGA around draw.c --------------------------------------------- */

static uint32_t frames;
unsigned long vga_frame_count(void) { return frames; }
bool vga_connected(void) { return true; }
void com_reply(const char *s, size_t n) { (void)s, (void)n; }
/* The console's mouse is not in this test: what a program asks of it does
 * nothing. */
void vterm_mouse_track(int func, int col, int row, int first, int last)
{
    (void)func, (void)col, (void)row, (void)first, (void)last;
}
void vterm_mouse_locate(void) {}
void vterm_mouse_filter(void) {}
void bel_term(void) {}

mode0_cell_t mode0_cells[MODE0_CELLS];
mode0_colors_t mode0_colors[MODE0_CELLS];
uint16_t mode0_faint_palette[VTERM_PALETTE];
uint16_t mode0_faint_defaults[MODE0_DEFAULTS];
volatile mode0_regs_t mode0_regs;

/* The console's bytes go to draw.c's terminal through the output it takes
 * (manifold.c). */
static int (*term_out)(const char *buf, int length, bool force);
void manifold_set_term_out(int (*out)(const char *buf, int length, bool force))
{
    term_out = out;
}

static void feed(const char *s)
{
    term_out(s, (int)strlen(s), false);
}

/* The console as at power-up, rows 0 to 9 full of their digit, and a frame
 * on, so the next draw_task publishes. */
static void fresh(void)
{
    draw_init();
    for (int y = 0; y < 10; y++)
    {
        char line[96];
        memset(line, '0' + y, 80);
        line[80] = 0;
        feed(line);
        if (y < 9)
            feed("\r\n");
    }
    feed("\33[20;1H");
    term_job_finish(&vterm);
    frames++;
    draw_task();
    frames++;
}

/* The words of the 80-column terminal's primary, the RIA's encoding of a
 * selection from row y0, column x0 up to row y1, column x1 of the screen
 * (vterm_select_words), as the VGA receives them. */
static void words(int y0, int x0, int y1, int x1, uint16_t *start, uint16_t *end)
{
    const screen_t *s = &vterm_80.grid.screen[0];
    *start = (uint16_t)(x0 | screen_mem(s, (uint8_t)y0) << 7 | 2u << 13);
    *end = (uint16_t)(x1 | screen_mem(s, (uint8_t)y1) << 7 | 2u << 13);
}

static unsigned row_sel(int y)
{
    const uint32_t w = mode0_regs.row[y];
    return MODE0_ROW_FROM(w) * 100u + MODE0_ROW_TO(w);
}

/* A selection that comes with no byte of the console is published by
 * draw_task alone, once a frame at most like everything else. */
UTEST(draw, a_selection_alone_is_published_once_a_frame)
{
    fresh();
    uint16_t start, end;
    words(2, 5, 4, 10, &start, &end);
    vterm_select_apply(start, end);
    ASSERT_TRUE(vterm_selection.shows);
    draw_task();
    ASSERT_EQ(row_sel(2), 580u);
    ASSERT_EQ(row_sel(3), 80u);
    ASSERT_EQ(row_sel(4), 10u);
    vterm_select_apply(0, 0);
    ASSERT_FALSE(vterm_selection.shows);
    draw_task();
    ASSERT_EQ(row_sel(2), 580u);
    frames++;
    draw_task();
    ASSERT_EQ(row_sel(2), 0u);
    ASSERT_EQ(row_sel(4), 0u);
}

/* The words arrive while a job is still pending: the RIA did the job at
 * once and judged it as it started, before the user's selection, as the
 * VGA did, so the selection the words bring stays as the job goes on over
 * its rows. */
UTEST(draw, a_selection_applies_with_a_job_pending)
{
    fresh();
    feed("\33[1;1H\33[J");
    ASSERT_TRUE(term_job_pending(&vterm));
    uint16_t start, end;
    words(2, 5, 4, 10, &start, &end);
    vterm_select_apply(start, end);
    ASSERT_TRUE(term_job_pending(&vterm));
    while (term_job_pending(&vterm))
        draw_task();
    ASSERT_TRUE(vterm_selection.shows);
    frames++;
    draw_task();
    ASSERT_EQ(row_sel(3), 80u);
}

/* A job is judged as it starts, so one over the selection drops it at its
 * byte, before it has done a row. */
UTEST(draw, a_job_drops_it_as_it_starts)
{
    fresh();
    uint16_t start, end;
    words(2, 5, 4, 10, &start, &end);
    vterm_select_apply(start, end);
    feed("\33[1;1H\33[J");
    ASSERT_TRUE(term_job_pending(&vterm));
    ASSERT_FALSE(vterm_selection.shows);
}

UTEST_MAIN()
