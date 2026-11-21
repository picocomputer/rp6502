/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The RIA's console terminal, which has no cells: which rows it says may
 * have something on them, for the clear line a program's end leaves. */

#include "core/aud/bel.h"
#include "core/com/com.h"
#include "core/com/manifold.h"
#include "core/term/cells.h"
#include "core/term/grid.h"
#include "ria/sys/vga.h"
#include "utest.h"
#include <string.h>

/* The console with no VGA, the 80-column terminal, unless a test connects
 * one (ria/sys/vga.c). Its canvas is 640 wide, so the 80 shows either way. */
static bool connected;
bool vga_connected(void) { return connected; }
uint32_t vga_frames(void) { return 0; }
void vga_canvas_size(int *w, int *h) { *w = 640, *h = 480; }
void manifold_set_term_out(int (*out)(const char *buf, int len, bool force)) { (void)out; }
void com_reply(const char *s, size_t n) { (void)s, (void)n; }

/* The bell's voice is the PSG's ninth, and register 6 is its gate
 * (core/aud/bel.c); the bell sounds while the gate is up. Its deadlines
 * never fall due, as the clock stands still. */
static uint8_t gate;
void aud_bel_put(unsigned i, uint8_t val)
{
    if (i == 6)
        gate = val;
}
uint64_t host_clock_us(void) { return 1; }

static void say(const char *s)
{
    while (*s)
        term_write(&cells_term, (uint8_t)*s++);
}

/* The rows in use, as a string of a character a row: # in use, . clear. */
static const char *rows(int count)
{
    static char out[40];
    term_grid_t *g = cells_shown(&cells_term);
    for (int y = 0; y < count; y++)
        out[y] = screen_row_used(g->shown, (uint8_t)y) ? '#' : '.';
    out[count] = 0;
    return out;
}

static void fresh(void)
{
    connected = false;
    cells_init();
    cells_RIS();
}

UTEST(cells, a_row_written_is_in_use)
{
    fresh();
    ASSERT_STREQ(rows(6), "......");
    say("one\r\n\r\nthree");
    ASSERT_STREQ(rows(6), "#.#...");
    /* Spaces in the plain pen put nothing on a row. */
    say("\33[5;1H     ");
    ASSERT_STREQ(rows(6), "#.#...");
    /* A space in reverse, or underlined, or on a color, does. */
    say("\33[4;1H\33[7m \33[0m\33[5;1H\33[4m \33[0m\33[6;1H\33[41m \33[0m");
    ASSERT_STREQ(rows(6), "#.####");
}

UTEST(cells, an_erase_of_a_whole_row_clears_it)
{
    fresh();
    say("one\r\ntwo\r\nthree\r\nfour");
    ASSERT_STREQ(rows(5), "####.");
    /* EL 2 on row 1, and ED 0 from the start of row 2. */
    say("\33[1;1H\33[2K\33[3;1H\33[J");
    ASSERT_STREQ(rows(5), ".#...");
    /* Part of a row erased leaves it in use, which is all that is known. */
    say("\33[2;2H\33[K\33[2;1H\33[1K");
    ASSERT_STREQ(rows(5), ".#...");
    /* An erase to a color puts the color on the row. */
    say("\33[4;1H\33[44m\33[2K\33[0m");
    ASSERT_STREQ(rows(5), ".#.#.");
    say("\33[2J");
    ASSERT_STREQ(rows(5), ".....");
}

UTEST(cells, the_rows_move_with_a_scroll)
{
    fresh();
    say("a\r\n\r\nc\r\n\r\ne");
    ASSERT_STREQ(rows(7), "#.#.#..");
    say("\33[1S");
    ASSERT_STREQ(rows(7), ".#.#...");
    say("\33[2T");
    ASSERT_STREQ(rows(7), "...#.#.");
    /* Inside a scroll region the rows outside it stay. */
    say("\33[4;6r\33[4;1H\33[1M\33[r");
    ASSERT_STREQ(rows(7), "....#..");
    /* A line feed on the bottom row moves everything up. */
    fresh();
    const int bottom = cells_shown(&cells_term)->height - 1;
    say("top\33[99;1Hbottom");
    ASSERT_TRUE(screen_row_used(cells_shown(&cells_term)->shown, 0));
    ASSERT_TRUE(screen_row_used(cells_shown(&cells_term)->shown, (uint8_t)bottom));
    say("\n");
    ASSERT_FALSE(screen_row_used(cells_shown(&cells_term)->shown, 0));
    ASSERT_TRUE(screen_row_used(cells_shown(&cells_term)->shown, (uint8_t)(bottom - 1)));
    ASSERT_FALSE(screen_row_used(cells_shown(&cells_term)->shown, (uint8_t)bottom));
}

UTEST(cells, each_screen_has_its_own_rows)
{
    fresh();
    say("main");
    say("\33[?47h\33[2J\33[3;1Halternate");
    ASSERT_STREQ(rows(4), "..#.");
    say("\33[?47l");
    ASSERT_STREQ(rows(4), "#...");
}

/* The bell is the VGA terminal's: with no VGA, the terminal the user sees
 * is another, which rings its own. */
UTEST(cells, a_bell_rings_only_with_a_vga)
{
    fresh();
    bel_reset();
    bel_init();
    say("\a");
    ASSERT_FALSE(gate);
    connected = true;
    say("\a");
    ASSERT_TRUE(gate);
    /* And none rings with the bell turned off. */
    bel_init();
    bel_set_term(false);
    say("\a");
    ASSERT_FALSE(gate);
    bel_reset();
    say("\a");
    ASSERT_TRUE(gate);
    bel_init();
}

UTEST_MAIN();
