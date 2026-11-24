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
bool vterm_focused(void) { return true; }
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
 * selection from row y0, column x0 up to row y1, column x1 of the screen,
 * between the margins left and right (vterm_select_words), as the VGA receives
 * them. */
static vterm_select_words_t margined(int y0, int x0, int y1, int x1, int left, int right)
{
    const screen_t *s = &vterm_80.grid.screen[0];
    return (vterm_select_words_t){
        (uint16_t)(left | right << 7),
        (uint16_t)(x0 | screen_mem(s, (uint8_t)y0) << 7 | 2u << 13),
        (uint16_t)(x1 | screen_mem(s, (uint8_t)y1) << 7 | 2u << 13),
    };
}

/* The same, out to the screen's edges, as the mouse's are. */
static vterm_select_words_t words(int y0, int x0, int y1, int x1)
{
    return margined(y0, x0, y1, x1, 0, SCREEN_SELECTION_EDGE);
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
    vterm_select_apply(words(2, 5, 4, 10));
    ASSERT_TRUE(vterm_selection.shows);
    draw_task();
    ASSERT_EQ(row_sel(2), 580u);
    ASSERT_EQ(row_sel(3), 80u);
    ASSERT_EQ(row_sel(4), 10u);
    vterm_select_apply((vterm_select_words_t){0, 0, 0});
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
    vterm_select_apply(words(2, 5, 4, 10));
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
    vterm_select_apply(words(2, 5, 4, 10));
    feed("\33[1;1H\33[J");
    ASSERT_TRUE(term_job_pending(&vterm));
    ASSERT_FALSE(vterm_selection.shows);
}

/* A selection between margins, as the line editor's is, takes on
 * each row only the columns between them, its first row from its start and
 * its last up to its end; output outside the margins keeps it, and output
 * between them drops it. */
UTEST(draw, a_selection_between_margins_takes_only_their_columns)
{
    fresh();
    ASSERT_TRUE(vterm_select_apply(margined(2, 25, 4, 15, 10, 30)));
    ASSERT_TRUE(vterm_selection.shows);
    draw_task();
    ASSERT_EQ(row_sel(2), 2530u);
    ASSERT_EQ(row_sel(3), 1030u);
    ASSERT_EQ(row_sel(4), 1015u);
    feed("\33[4;1Habcdefghij\33[4;31Hxyz");
    ASSERT_TRUE(vterm_selection.shows);
    feed("\33[4;11Hk");
    ASSERT_FALSE(vterm_selection.shows);
    /* Margins with no column between them select nothing. */
    ASSERT_FALSE(vterm_select_apply(margined(2, 25, 4, 15, 30, 30)));
    ASSERT_FALSE(vterm_selection.shows);
}

UTEST_MAIN()
