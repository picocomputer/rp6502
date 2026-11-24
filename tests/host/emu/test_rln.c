/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/api/api.h"
#include "core/api/xreg.h"
#include "core/com/com.h"
#include "core/com/manifold.h"
#include "core/hid/typed.h"
#include "core/com/tio.h"
#include "core/ria/regs.h"
#include "core/com/rln.h"
#include "core/sys/sst.h"
#include "core/sys/sys.h"
#include "core/com/console.h"
#include "core/term/draw.h"
#include "core/term/grid.h"
#include "core/term/vterm.h"
#include "tb_screen.h"
#include "core/sys/timer.h"
#include "core/vga/vga_emu.h"
#include "utest.h"
#include <stdlib.h>
#include <string.h>

static int lines;
static char last[256];

/* The history these reads are handed, as the monitor hands its own, with
 * memory that has room for a number of lines the case sets. */
#define HELD_MAX 32
static void *held[HELD_MAX];
static int holding;
static int room;

static void *history_alloc(size_t size)
{
    if (holding >= room)
        return NULL;
    for (int i = 0; i < HELD_MAX; i++)
        if (!held[i])
        {
            holding++;
            return held[i] = malloc(size);
        }
    return NULL;
}

static void history_free(void *ptr)
{
    for (int i = 0; i < HELD_MAX; i++)
        if (held[i] == ptr)
        {
            held[i] = NULL;
            holding--;
            free(ptr);
            return;
        }
}

static rln_history_t history;
static rln_history_t *reading_with = &history;

/* The kill buffer these reads are handed, as a caller hands its own. */
static rln_kill_t kills;
static rln_kill_t *killing_with = &kills;

/* The reader takes its line out of the history again, as the monitor does
 * with a line that has a password in it. */
static bool forgetting;

static void got_line(bool timeout, const char *buf)
{
    (void)timeout;
    lines++;
    if (forgetting)
        rln_history_forget(&history);
    forgetting = false;
    strncpy(last, buf, sizeof last - 1);
    last[sizeof last - 1] = 0;
    rln_read_line(got_line, reading_with, killing_with);
}

static void tasks(void)
{
    for (int i = 0; i < 64; i++)
        rln_task();
}

static void wire(const char *s)
{
    manifold_uart_push(s, strlen(s));
    tasks();
}

/* rln_init keeps every source's line end, so they are cleared with
 * rln_forget_source too, or a case would start with what the previous case
 * left. tio starts over too. The console's terminal starts over with its
 * cursor at the top left, and the line editor reads its prompt column and
 * size from it. */
static void fresh(void)
{
    /* No frames run, so the console's line keeps no pace. */
    manifold_set_paced(false);
    for (int i = 0; i < HELD_MAX; i++)
        if (held[i])
            history_free(held[i]);
    history = (rln_history_t){.alloc = history_alloc, .free = history_free};
    kills.len = 0;
    killing_with = &kills;
    room = HELD_MAX;
    reading_with = &history;
    forgetting = false;
    draw_init();
    com_set_reply_default(true);
    rln_set_plain(false);
    com_rx_clear();
    rln_init();
    for (com_source_t s = COM_SOURCE_KEYBOARD; s < COM_SOURCE_COUNT; s++)
        rln_forget_source(s);
    tio_init();
}

static void reading(void)
{
    lines = 0;
    last[0] = 0;
    rln_read_line(got_line, reading_with, killing_with);
}

UTEST(rln, either_spelling_ends_a_line)
{
    fresh();
    reading();
    wire("a\r");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "a");
    reading();
    wire("b\n");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "b");
}

UTEST(rln, a_return_and_its_line_feed_are_one_line_end)
{
    fresh();
    reading();
    wire("a\r\n");
    ASSERT_EQ(lines, 1);
    reading();
    wire("b\n\r");
    ASSERT_EQ(lines, 1);
}

UTEST(rln, the_memory_of_a_line_end_lasts_one_character)
{
    fresh();
    reading();
    wire("\r\r");
    ASSERT_EQ(lines, 2);
    reading();
    wire("a\r\n\r\nb\r\n");
    ASSERT_EQ(lines, 3);
    ASSERT_STREQ(last, "b");
}

UTEST(rln, a_line_end_outlives_the_machine_it_was_typed_at)
{
    fresh();
    reading();
    wire("a\r");
    ASSERT_EQ(lines, 1);
    rln_stop();
    reading();
    wire("\n");
    ASSERT_EQ(lines, 0);
    wire("b\r");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "b");
}

UTEST(rln, a_source_that_goes_away_takes_its_half_line_end_with_it)
{
    fresh();
    reading();
    wire("a\r");
    ASSERT_EQ(lines, 1);
    rln_forget_source(COM_SOURCE_UART);
    reading();
    wire("\n");
    ASSERT_EQ(lines, 1);
}

/* Pulls the key and the action byte, as a program does. */
static int lastkey_len(void)
{
    rln_api_lastkey();
    int len = (int16_t)API_AX;
    if (len > 0)
        xstack_ptr += (size_t)len + 1;
    return len;
}

UTEST(rln, lastkey_and_peek_require_an_empty_xstack)
{
    fresh();
    reading();
    xstack_ptr = XSTACK_SIZE - 1;
    rln_api_lastkey();
    ASSERT_EQ((int16_t)API_AX, -1);
    xstack_ptr = XSTACK_SIZE - 1;
    rln_api_peek();
    ASSERT_EQ((int16_t)API_AX, -1);
    ASSERT_EQ(lastkey_len(), 0);
}

static void editing(void)
{
    fresh();
    reading();
}

UTEST(rln, a_key_longer_than_32_bytes_is_not_returned)
{
    editing();
    wire("\33[C");
    ASSERT_EQ(lastkey_len(), 3);
    char key[64] = "\33[";
    for (int i = 0; i < 16; i++)
        strcat(key, "1;");
    strcat(key, "C");
    wire(key);
    ASSERT_EQ(lastkey_len(), 0);
}

/* The up arrow after the Enter is the next read's key. */
UTEST(rln, a_key_typed_right_after_enter_goes_to_the_next_read)
{
    editing();
    wire("x\r");
    ASSERT_EQ(lines, 1);
    wire("a\r\33[A");
    ASSERT_EQ(lines, 2);
    ASSERT_STREQ(last, "a");
    wire("\r");
    ASSERT_EQ(lines, 3);
    ASSERT_STREQ(last, "a");
}

static char shown[4096];
static size_t shown_len;

static void show(const char *buf, int len)
{
    if (shown_len + (size_t)len < sizeof shown)
    {
        memcpy(shown + shown_len, buf, (size_t)len);
        shown_len += (size_t)len;
        shown[shown_len] = 0;
    }
}

static void capture(void)
{
    shown_len = 0;
    shown[0] = 0;
    manifold_set_tx_tap(show);
}

/* The terminal the line editor lays out on: a program's own size while it
 * has pinned one, otherwise the screen. */
static term_grid_t *rln_view(void)
{
    return vterm.grid_count > TERM_SCREEN_GRIDS ? vterm.grids[TERM_SCREEN_GRIDS]
                                                     : vterm_get_shown(&vterm);
}

/* Its cursor. */
static const term_pos_t *pos(void)
{
    return term_pos(rln_view());
}

/* The monitor and console.c start the next read after the callback returns. */
static bool need_next;

static void got_line_later(bool timeout, const char *buf)
{
    (void)timeout;
    lines++;
    strncpy(last, buf, sizeof last - 1);
    last[sizeof last - 1] = 0;
    need_next = true;
}

static void next(void)
{
    if (need_next)
    {
        need_next = false;
        rln_read_line(got_line_later, reading_with, killing_with);
    }
}

/* The prompt is printed before the read starts, and the line starts after it.
 * The fields are ten characters, so the cursor moves by column, with CR and
 * CUF. */
UTEST(rln, the_prompt_column_comes_from_the_printed_prompt)
{
    fresh();
    rln_set_max_length(10);
    com_printf("> ");
    reading();
    wire("ab");
    capture();
    wire("\33[H");
    manifold_set_tx_tap(NULL);
    ASSERT_STREQ(shown, "\r\33[2C");
}

/* What is typed before the read waits in its source, and is drawn at this
 * read's prompt. */
UTEST(rln, type_ahead_is_drawn_at_this_reads_prompt)
{
    fresh();
    rln_set_max_length(10);
    manifold_uart_push("ab", 2);
    tasks();
    com_printf("12345");
    capture();
    reading();
    tasks();
    manifold_set_tx_tap(NULL);
    ASSERT_TRUE(strstr(shown, "\r\33[5Ca") != NULL);
}

UTEST(rln, a_preload_is_drawn_at_this_reads_prompt)
{
    fresh();
    rln_set_max_length(10);
    com_printf("abc");
    reading();
    capture();
    rln_poke("xy");
    manifold_set_tx_tap(NULL);
    ASSERT_TRUE(strstr(shown, "\r\33[3Cx") != NULL);
}

/* A read with history asks for bracketed paste too, and one without does
 * not. */
UTEST(rln, a_read_start_sends_the_modes_and_asks_nothing)
{
    fresh();
    capture();
    reading();
    tasks();
    manifold_set_tx_tap(NULL);
    ASSERT_STREQ(shown, "\33[4l\33[?2004h");
    fresh();
    reading_with = NULL;
    capture();
    reading();
    tasks();
    manifold_set_tx_tap(NULL);
    ASSERT_STREQ(shown, "\33[4l");
}

/* A prompt that fills the row leaves the terminal in a pending wrap, so the
 * line starts on the next row. */
UTEST(rln, a_pending_wrap_gives_a_cr_lf_then_column_1)
{
    fresh();
    for (int i = 0; i < 80; i++)
        manifold_putchar('p');
    capture();
    reading();
    manifold_set_tx_tap(NULL);
    ASSERT_STREQ(shown, "\r\n\33[4l\33[?2004h");
    const term_pos_t *p = term_pos(rln_view());
    ASSERT_EQ(p->x, 0);
    ASSERT_EQ(p->y, 1);
    ASSERT_FALSE(p->wrap_pending);
}

UTEST(rln, a_flush_hands_over_what_was_typed)
{
    fresh();
    reading();
    wire("bb");
    ASSERT_EQ(lines, 0);
    ASSERT_TRUE(rln_read_flush());
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "bb");
    ASSERT_FALSE(rln_read_flush());
}

UTEST(rln, a_flush_hands_over_a_preload)
{
    fresh();
    lines = 0;
    need_next = true;
    next();
    rln_poke("x");
    ASSERT_TRUE(rln_read_flush());
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "x");
    ASSERT_FALSE(rln_read_flush());
}

UTEST(rln, a_poked_enter_takes_what_was_typed)
{
    fresh();
    reading();
    wire("ab");
    rln_poke("\r");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "ab");
}

/* A 320-wide canvas shows the 40-column grid. No terminal is trusted to
 * have reflowed the line, so it starts over in column 1 of a clean row. */
UTEST(rln, a_new_width_draws_the_line_again)
{
    fresh();
    reading();
    wire("abc");
    capture();
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_320_240));
    tasks();
    manifold_set_tx_tap(NULL);
    ASSERT_STREQ(shown, "\r\n\33[4l\33[?2004habc");
    ASSERT_EQ(rln_view()->width, 40);
    ASSERT_EQ(pos()->x, 3);
    shown_len = 0;
    shown[0] = 0;
    capture();
    tasks();
    manifold_set_tx_tap(NULL);
    ASSERT_EQ(shown_len, (size_t)0);
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
}

/* ----- Layout: rln breaks its own rows ----- */

/* Nothing that asks a terminal anything, and nothing that turns autowrap
 * on, ever goes out. */
static bool probed(void)
{
    static const char *const never[] = {"\33[6n", "\33[>c", "\33[999;999H", "\33[?7h"};
    for (size_t i = 0; i < sizeof never / sizeof *never; i++)
        if (strstr(shown, never[i]))
            return true;
    return false;
}

static void type_n(int n, char ch)
{
    char s[256];
    memset(s, ch, (size_t)n);
    s[n] = 0;
    wire(s);
}

/* The row breaks as the 80th character is typed, with autowrap off. */
UTEST(rln, a_row_breaks_with_cr_lf_right_after_column_80)
{
    fresh();
    com_printf("\33[?7l");
    capture();
    reading();
    type_n(80, 'a');
    ASSERT_TRUE(strstr(shown, "a\r\n") != NULL);
    ASSERT_EQ(pos()->x, 0);
    ASSERT_EQ(pos()->y, 1);
    wire("b");
    manifold_set_tx_tap(NULL);
    ASSERT_FALSE(probed());
    ASSERT_FALSE(vterm.autowrap);
    ASSERT_EQ(tb_screen_row(0)[79].glyph, 'a');
    ASSERT_EQ(tb_screen_row(1)[0].glyph, 'b');
    ASSERT_EQ(pos()->x, 1);
    ASSERT_EQ(pos()->y, 1);
}

/* Left, Right and Home across a row break move by row, then by column. */
UTEST(rln, moves_across_a_row_go_up_or_down_then_to_the_column)
{
    editing();
    type_n(80, 'a');
    capture();
    wire("\33[D");
    ASSERT_STREQ(shown, "\33[1A\r\33[79C");
    ASSERT_EQ(pos()->x, 79);
    ASSERT_EQ(pos()->y, 0);
    shown_len = 0;
    wire("\33[C");
    ASSERT_STREQ(shown, "\33[1B\r");
    ASSERT_EQ(pos()->x, 0);
    ASSERT_EQ(pos()->y, 1);
    shown_len = 0;
    wire("\33[H");
    manifold_set_tx_tap(NULL);
    ASSERT_STREQ(shown, "\33[1A\r");
    ASSERT_EQ(pos()->x, 0);
    ASSERT_EQ(pos()->y, 0);
}

/* A line that gets shorter erases its tail with EL and clears the rows it
 * no longer reaches. */
UTEST(rln, a_shrink_erases_the_tail_and_the_rows_below)
{
    editing();
    type_n(85, 'a');
    wire("\33[H");
    capture();
    wire("\v");
    manifold_set_tx_tap(NULL);
    ASSERT_STREQ(shown, "\33[K\33[B\r\33[K\33[1A");
    ASSERT_EQ(tb_screen_row(0)[0].glyph, ' ');
    ASSERT_EQ(tb_screen_row(1)[0].glyph, ' ');
    ASSERT_EQ(pos()->x, 0);
    ASSERT_EQ(pos()->y, 0);
}

/* A full line that ends on the last column keeps the cursor on its last
 * cell, out of the wrap, so an overwrite there stays on the row. */
UTEST(rln, a_full_line_keeps_the_cursor_on_its_last_cell)
{
    editing();
    rln_set_max_length(160);
    type_n(160, 'a');
    ASSERT_EQ(pos()->x, 79);
    ASSERT_EQ(pos()->y, 1);
    ASSERT_FALSE(pos()->wrap_pending);
    wire("b\33[2~z");
    ASSERT_EQ(tb_screen_row(1)[79].glyph, 'z');
    ASSERT_EQ(pos()->x, 79);
    ASSERT_EQ(pos()->y, 1);
    wire("\r");
    ASSERT_EQ(lines, 1);
    ASSERT_EQ(strlen(last), (size_t)160);
    ASSERT_EQ(last[159], 'z');
    ASSERT_EQ(pos()->x, 0);
    ASSERT_EQ(pos()->y, 2);
}

/* ----- The end of the line ----- */

/* The row break after the 79th character is the line's end: Enter or a stop
 * adds no blank row. */
UTEST(rln, a_line_that_ends_on_the_last_column_leaves_no_blank_row)
{
    fresh();
    com_printf("]");
    reading();
    type_n(79, 'a');
    ASSERT_EQ(pos()->x, 0);
    ASSERT_EQ(pos()->y, 1);
    wire("\r");
    ASSERT_EQ(lines, 1);
    manifold_flush();
    ASSERT_EQ(pos()->x, 0);
    ASSERT_EQ(pos()->y, 1);
    com_printf("]");
    reading();
    type_n(79, 'b');
    rln_stop();
    manifold_flush();
    ASSERT_EQ(pos()->x, 0);
    ASSERT_EQ(pos()->y, 2);
}

/* A "^C" whose '^' is on the last column ends the line on the row of its
 * 'C', so the next prompt goes on the row below it. */
UTEST(rln, a_caret_across_a_row_break_stays_whole)
{
    fresh();
    com_printf("]");
    reading();
    type_n(78, 'a');
    rln_poke("\3");
    ASSERT_EQ(lines, 1);
    com_printf("]");
    manifold_flush();
    ASSERT_EQ(tb_screen_row(0)[79].glyph, '^');
    ASSERT_EQ(tb_screen_row(1)[0].glyph, 'C');
    ASSERT_EQ(tb_screen_row(2)[0].glyph, ']');
}

/* An 80-column VT100 with autowrap off, fed what goes out the UART. It keeps
 * the columns a narrower screen grid has not got. */
#define VT_ROWS 30
static char vt[VT_ROWS][81];
static int vt_x, vt_y, vt_state;
static unsigned vt_param;

static void vt_csi(char final)
{
    int n = vt_param ? (int)vt_param : 1;
    if (final == 'A')
        vt_y = vt_y > n ? vt_y - n : 0;
    else if (final == 'B')
        vt_y = vt_y + n < VT_ROWS ? vt_y + n : VT_ROWS - 1;
    else if (final == 'C')
        vt_x = vt_x + n < 80 ? vt_x + n : 79;
    else if (final == 'D')
        vt_x = vt_x > n ? vt_x - n : 0;
    else if (final == 'K' && !vt_param)
        memset(&vt[vt_y][vt_x], ' ', (size_t)(80 - vt_x));
}

static void vt_out(const char *buf, int len)
{
    for (int i = 0; i < len; i++)
    {
        char c = buf[i];
        if (vt_state == 1)
        {
            vt_state = c == '[' ? 2 : 0;
            vt_param = 0;
        }
        else if (vt_state == 2)
        {
            if (c >= '0' && c <= '9')
                vt_param = vt_param * 10 + (unsigned)(c - '0');
            else if (c >= 0x40 && c <= 0x7E)
            {
                vt_csi(c);
                vt_state = 0;
            }
        }
        else if (c == '\33')
            vt_state = 1;
        else if (c == '\r')
            vt_x = 0;
        else if (c == '\n')
            vt_y = vt_y + 1 < VT_ROWS ? vt_y + 1 : VT_ROWS - 1;
        else if (c >= ' ')
        {
            vt[vt_y][vt_x] = c;
            if (vt_x < 79)
                vt_x++;
        }
    }
}

static void vt_start(void)
{
    for (int y = 0; y < VT_ROWS; y++)
    {
        memset(vt[y], ' ', 80);
        vt[y][80] = 0;
    }
    vt_x = vt_y = vt_state = 0;
    manifold_set_tx_tap(vt_out);
}

/* A row of the VT100 holds n of ch, then blanks. */
static bool vt_row_is(int y, int n, char ch)
{
    for (int x = 0; x < 80; x++)
        if (vt[y][x] != (x < n ? ch : ' '))
            return false;
    return true;
}

/* With the cursor at Home, a pinned width of 40 draws the line again on the
 * rows below the old one, which stays whole: nothing new lands in its
 * columns 41 to 80. */
UTEST(rln, a_new_width_draws_the_line_below_the_old_one)
{
    fresh();
    vt_start();
    reading();
    type_n(200, 'a');
    wire("\33[H");
    rln_set_term_width(40);
    tasks();
    wire("\r");
    manifold_set_tx_tap(NULL);
    ASSERT_EQ(lines, 1);
    ASSERT_EQ(strlen(last), (size_t)200);
    ASSERT_TRUE(vt_row_is(0, 80, 'a'));
    ASSERT_TRUE(vt_row_is(1, 80, 'a'));
    ASSERT_TRUE(vt_row_is(2, 40, 'a'));
    for (int y = 3; y < 8; y++)
        ASSERT_TRUE(vt_row_is(y, 40, 'a'));
    ASSERT_TRUE(vt_row_is(8, 0, ' '));
    ASSERT_EQ(vt_x, 0);
    ASSERT_EQ(vt_y, 8);
    rln_stop();
}

/* A pinned width of 20 holds 10 rows of 20, so a 230-character line is cut
 * to 200, and Home still climbs to its first row. */
UTEST(rln, a_new_width_cuts_the_line_to_what_it_holds)
{
    fresh();
    vt_start();
    reading();
    type_n(230, 'a');
    rln_set_term_width(20);
    tasks();
    wire("\33[H");
    ASSERT_EQ(vt_x, 0);
    ASSERT_EQ(vt_y, 3);
    wire("\r");
    manifold_set_tx_tap(NULL);
    ASSERT_EQ(lines, 1);
    ASSERT_EQ(strlen(last), (size_t)200);
    for (int y = 3; y < 13; y++)
        ASSERT_TRUE(vt_row_is(y, 20, 'a'));
    ASSERT_TRUE(vt_row_is(13, 0, ' '));
    ASSERT_EQ(vt_x, 0);
    ASSERT_EQ(vt_y, 13);
    rln_stop();
}

/* A double-width prompt row has 40 columns. */
UTEST(rln, a_double_width_prompt_row_breaks_at_40)
{
    fresh();
    com_printf("\33#6> ");
    capture();
    reading();
    type_n(38, 'a');
    manifold_set_tx_tap(NULL);
    ASSERT_FALSE(probed());
    ASSERT_TRUE(strstr(shown, "a\r\n") != NULL);
    ASSERT_EQ(pos()->x, 0);
    ASSERT_EQ(pos()->y, 1);
}

/* A program's own width of 40 breaks the row at 40 on the 80-column screen. */
UTEST(rln, a_pinned_width_of_40_breaks_at_40)
{
    fresh();
    rln_set_term_width(40);
    capture();
    reading();
    type_n(40, 'a');
    wire("b");
    manifold_set_tx_tap(NULL);
    ASSERT_FALSE(probed());
    ASSERT_TRUE(strstr(shown, "a\r\nb") != NULL);
    ASSERT_EQ(tb_screen_row(1)[0].glyph, 'b');
    ASSERT_EQ(pos()->x, 1);
    ASSERT_EQ(pos()->y, 1);
    rln_stop();
}

/* Pinning one axis fixes the other at the screen's, and a stop goes back to
 * the screen's size. */
UTEST(rln, the_size_is_the_pinned_one_until_a_stop)
{
    fresh();
    uint16_t w = rln_get_term_width(), h = rln_get_term_height();
    rln_set_term_width(50);
    ASSERT_EQ(rln_get_term_width(), 50);
    ASSERT_EQ(rln_get_term_height(), h);
    rln_set_term_height(20);
    ASSERT_EQ(rln_get_term_height(), 20);
    rln_stop();
    ASSERT_EQ(rln_get_term_width(), w);
    ASSERT_EQ(rln_get_term_height(), h);
}

/* The line can be longer than the row, so it is drawn on the wrap path. */
UTEST(rln, typing_at_the_end_of_the_line_echoes_only_the_char)
{
    editing();
    wire("ab");
    capture();
    wire("c");
    manifold_set_tx_tap(NULL);
    ASSERT_STREQ(shown, "c");
}

UTEST(rln, deleting_at_the_end_of_the_line_erases_the_tail)
{
    editing();
    wire("abc");
    capture();
    wire("\177");
    manifold_set_tx_tap(NULL);
    ASSERT_TRUE(strstr(shown, "\33[K") != NULL);
}

static char screen[4096];

/* What is sent to term.c is kept, and term.c still follows it. */
static int screen_out(const char *buf, int len, bool force)
{
    (void)force;
    strncat(screen, buf, (size_t)len);
    for (int i = 0; i < len; i++)
        term_write(&vterm, (uint8_t)buf[i]);
    draw_publish();
    return len;
}

static void screen_reading(void)
{
    fresh();
    screen[0] = 0;
    manifold_set_term_out(screen_out);
    capture();
    lines = 0;
    need_next = true;
    next();
}

static void screen_done(void)
{
    draw_init();
    manifold_set_tx_tap(NULL);
}

UTEST(rln, no_terminal_is_asked_and_an_enter_ends_the_line_at_once)
{
    screen_reading();
    wire("a\r");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "a");
    static const char *const queries[] = {"\33[6n", "\33[>c", "\33[999;999H"};
    for (size_t i = 0; i < sizeof queries / sizeof *queries; i++)
    {
        ASSERT_TRUE(strstr(screen, queries[i]) == NULL);
        ASSERT_TRUE(strstr(shown, queries[i]) == NULL);
    }
    screen_done();
}

/* What is typed after the Enter waits in its source for the next read. */
UTEST(rln, a_key_typed_after_an_enter_goes_to_the_next_read)
{
    screen_reading();
    wire("a\rb");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "a");
    next();
    wire("\r");
    ASSERT_EQ(lines, 2);
    ASSERT_STREQ(last, "b");
    screen_done();
}

/* A terminal on the UART may print a shape, so the shapes reach term.c alone.
 * The default comes back when the read ends. */
UTEST(rln, shapes_reach_the_screen_alone)
{
    screen_reading();
    ASSERT_TRUE(strstr(screen, "\33[5 q") != NULL);
    wire("\33[2~");
    ASSERT_TRUE(strstr(screen, "\33[1 q") != NULL);
    ASSERT_TRUE(strstr(screen, "\33[0 q") == NULL);
    wire("\r");
    ASSERT_TRUE(strstr(screen, "\33[1 q\33[0 q") != NULL);
    ASSERT_TRUE(strstr(shown, " q") == NULL);
    screen_done();
}

/* A program starting from a read cut short gets the default shape. */
UTEST(rln, a_stop_puts_the_default_shape_back)
{
    screen_reading();
    ASSERT_TRUE(strstr(screen, "\33[0 q") == NULL);
    rln_stop();
    ASSERT_TRUE(strstr(screen, "\33[0 q") != NULL);
    screen_done();
}

/* Plain lines lay nothing out and send no escape sequence: no prompt column,
 * no modes, no shape. BS moves left, the characters already there are printed
 * to move right, spaces erase, and the editing keys still edit. */
UTEST(rln, plain_lines_edit_with_no_escape_sequences)
{
    fresh();
    rln_set_plain(true);
    screen[0] = 0;
    manifold_set_term_out(screen_out);
    com_printf("> ");
    capture();
    reading();
    ASSERT_STREQ(shown, "");
    wire("abc\b");
    ASSERT_STREQ(shown, "abc\b \b");
    capture();
    wire("\33[Dx");
    ASSERT_STREQ(shown, "\bxb\b");
    capture();
    wire("\33[H");
    ASSERT_STREQ(shown, "\b\b");
    capture();
    wire("\33[F");
    ASSERT_STREQ(shown, "axb");
    capture();
    wire("\25");
    ASSERT_STREQ(shown, "\b\b\b   \b\b\b");
    capture();
    wire("axb\33[D\33[D\33[3~");
    ASSERT_STREQ(shown, "axb\b\bb \b\b");
    ASSERT_TRUE(strchr(screen, '\33') == NULL);
    wire("\r");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "ab");
    screen_done();
}

/* A terminal in VT52 mode takes no CSI. A read there is laid out all the
 * same, with a VT52's own moves and erase, each without a count. */
UTEST(rln, a_read_in_vt52_mode_is_laid_out_in_vt52_commands)
{
    fresh();
    com_printf("> \33[?2l");
    capture();
    reading();
    const uint8_t top = pos()->y;
    type_n(100, 'a');
    /* 78 fit on the prompt's row and the rest are on the next. */
    ASSERT_EQ(pos()->y, top + 1);
    ASSERT_EQ(pos()->x, 22);
    ASSERT_TRUE(strstr(shown, "\r\n") != NULL);
    /* Left twice, two typed, then Home, which is up a row and right to the
     * prompt's column, and End, which is down a row and right again. */
    wire("\33D\33Dxy");
    shown_len = 0;
    wire("\33H");
    ASSERT_STREQ(shown, "\33A\r\33C\33C");
    ASSERT_EQ(pos()->y, top);
    ASSERT_EQ(pos()->x, 2);
    wire("\33F");
    ASSERT_EQ(pos()->y, top + 1);
    ASSERT_EQ(pos()->x, 24);
    /* A backspace at the end erases with a VT52's erase. */
    shown_len = 0;
    wire("\b");
    manifold_set_tx_tap(NULL);
    ASSERT_TRUE(strstr(shown, "\33K") != NULL || strchr(shown, ' ') != NULL);
    ASSERT_TRUE(strstr(shown, "\33[") == NULL);
    ASSERT_TRUE(vterm.vt52);
    wire("\r");
    ASSERT_EQ(lines, 1);
    ASSERT_EQ(strlen(last), (size_t)101);
    ASSERT_STREQ(last + 97, "axya");
    /* The next read starts in ANSI mode, and is sent that mode's commands. */
    com_printf("\33<");
    capture();
    reading();
    manifold_set_tx_tap(NULL);
    ASSERT_STREQ(shown, "\33[4l\33[?2004h");
}

/* Nothing of a whole read in VT52 mode is a CSI: the history, a line that
 * shrinks over a row, and the cursor's shape among it. */
UTEST(rln, a_read_in_vt52_mode_sends_no_csi)
{
    fresh();
    com_printf("> \33[?2l");
    capture();
    reading();
    type_n(100, 'b');
    wire("\r");
    type_n(10, 'c');
    wire("\33A\33B\33A\33H\33C\33C\33F");
    for (int i = 0; i < 30; i++)
        wire("\b");
    wire("\r");
    manifold_set_tx_tap(NULL);
    ASSERT_TRUE(strstr(shown, "\33[") == NULL);
    ASSERT_TRUE(strstr(shown, "\33K") != NULL);
    ASSERT_EQ(lines, 2);
    ASSERT_EQ(strlen(last), (size_t)70);
    com_printf("\33<");
}

/* Between left and right margins a row is the columns between them: the
 * line breaks at the right margin, a CR lands on the left one, and nothing
 * either side of the margins is touched. */
UTEST(rln, a_line_between_left_and_right_margins_stays_between_them)
{
    fresh();
    /* Margins at columns 11 and 30, a mark either side of them on two rows,
     * and a prompt at the left margin of row 3. */
    com_printf("\33[2J\33[3;5HX\33[3;40HY\33[4;5HX\33[4;40HY");
    com_printf("\33[?69h\33[11;30s\33[3;11H> ");
    reading();
    const uint8_t top = pos()->y;
    ASSERT_EQ(top, 2);
    ASSERT_EQ(pos()->x, 12);
    /* 18 fit after the prompt, and the next 7 start at the left margin. */
    type_n(25, 'a');
    ASSERT_EQ(pos()->y, top + 1);
    ASSERT_EQ(pos()->x, 17);
    ASSERT_EQ(tb_screen_row(top)[12].glyph, 'a');
    ASSERT_EQ(tb_screen_row(top)[29].glyph, 'a');
    ASSERT_EQ(tb_screen_row(top)[30].glyph, ' ');
    ASSERT_EQ(tb_screen_row(top + 1)[10].glyph, 'a');
    ASSERT_EQ(tb_screen_row(top + 1)[16].glyph, 'a');
    ASSERT_EQ(tb_screen_row(top + 1)[17].glyph, ' ');
    ASSERT_EQ(tb_screen_row(top + 1)[9].glyph, ' ');
    /* Home and End find the ends of it. */
    wire("\33[H");
    ASSERT_EQ(pos()->y, top);
    ASSERT_EQ(pos()->x, 12);
    wire("b");
    ASSERT_EQ(tb_screen_row(top)[12].glyph, 'b');
    ASSERT_EQ(tb_screen_row(top + 1)[10].glyph, 'a');
    ASSERT_EQ(tb_screen_row(top + 1)[17].glyph, 'a');
    wire("\33[F");
    ASSERT_EQ(pos()->y, top + 1);
    ASSERT_EQ(pos()->x, 18);
    /* A line that shrinks back onto one row erases what it had below. */
    for (int i = 0; i < 12; i++)
        wire("\b");
    ASSERT_EQ(pos()->y, top);
    ASSERT_EQ(tb_screen_row(top + 1)[10].glyph, ' ');
    for (uint8_t y = top; y <= top + 1; y++)
    {
        ASSERT_EQ(tb_screen_row(y)[4].glyph, 'X');
        ASSERT_EQ(tb_screen_row(y)[39].glyph, 'Y');
    }
    wire("\r");
    ASSERT_EQ(lines, 1);
    ASSERT_EQ(strlen(last), (size_t)14);
    ASSERT_EQ(last[0], 'b');
    com_printf("\33[?69l");
}

/* A line has no more rows than the scroll region its cursor is in. */
UTEST(rln, a_line_has_no_more_rows_than_its_scroll_region)
{
    fresh();
    com_printf("\33[2J\33[5;6r\33[5;1H> ");
    reading();
    type_n(250, 'a');
    wire("\r");
    ASSERT_EQ(lines, 1);
    ASSERT_TRUE(strlen(last) <= 2 * 80 - 2);
    ASSERT_TRUE(strlen(last) >= 2 * 80 - 4);
    com_printf("\33[r");
}

/* A VT52's arrows, Home and End edit the line while the console's terminal
 * is in VT52 mode, when they are what the keyboard sends. */
UTEST(rln, vt52_arrows_home_and_end_edit_the_line)
{
    fresh();
    com_printf("\33[?2l");
    reading();
    wire("one\r");
    wire("abc\33D\33Dx\33Hy\33Fz\33H\33C\33C!\r");
    ASSERT_EQ(lines, 2);
    ASSERT_STREQ(last, "ya!xbcz");
    wire("\33A\33A\r");
    ASSERT_EQ(lines, 3);
    ASSERT_STREQ(last, "one");
    wire("\33A\33A\33B\r");
    ASSERT_EQ(lines, 4);
    ASSERT_STREQ(last, "one");
    com_printf("\33<");
}

/* Outside VT52 mode ESC and a letter is Alt with that letter: Alt-B and
 * Alt-F move by words and Alt-D kills one, in upper case too, as the
 * keyboard sends them with Shift held or Caps Lock on. Alt-A, Alt-C and
 * Alt-H, a VT52's Up, Right and Home, do nothing. */
UTEST(rln, alt_with_a_letter_is_the_same_in_upper_case)
{
    editing();
    wire("one\r");
    wire("ab cd ef\33B\33Bx\33F\33Fy\33b\33b\33b\33D\r");
    ASSERT_EQ(lines, 2);
    ASSERT_STREQ(last, " xcd efy");
    wire("ab\33Hx\r");
    ASSERT_STREQ(last, "abx");
    wire("ab\33[H\33Cx\r");
    ASSERT_STREQ(last, "xab");
    wire("\33Ay\r");
    ASSERT_STREQ(last, "y");
}

/* Alt and Ctrl together with B, F, D and Backspace go by whole arguments,
 * which only a space parts, where Alt alone goes by words: a path is one
 * argument and several words. What they kill is Ctrl-Y's too, and with a
 * selection they kill it. While Backspace sends BS, Ctrl makes DEL of it,
 * and ESC DEL is the one that goes by arguments. CSI b and CSI f are no
 * key of the line editor's. */
UTEST(rln, alt_and_ctrl_together_go_by_arguments)
{
    editing();
    wire("load USB0:/a/b.c x\33\2\33\2!\33[H\33\6\33\6?\r");
    ASSERT_STREQ(last, "load !USB0:/a/b.c? x");
    wire("load USB0:/a/b.c\33\10\r");
    ASSERT_STREQ(last, "load ");
    wire("load USB0:/a/b.c\33\177\r");
    ASSERT_STREQ(last, "load USB0:/a/");
    wire("load USB0:/a/b.c x\33[H\33\6\33\4\33[F\31\r");
    ASSERT_STREQ(last, "load x USB0:/a/b.c");
    com_printf("\33[?67h");
    wire("load USB0:/a/b.c\33\177\r");
    ASSERT_STREQ(last, "load ");
    wire("load USB0:/a/b.c\33\10\r");
    ASSERT_STREQ(last, "load USB0:/a/");
    com_printf("\33[?67l");
    wire("ab cd\33[bx\33[H\33[fy\r");
    ASSERT_STREQ(last, "yab cdx");
}

/* A word is what a space or a path's separators part, a colon, a slash or
 * a backslash, so a file's name is one word with its extension. */
UTEST(rln, a_word_ends_at_a_space_or_a_path_separator)
{
    editing();
    wire("load USB0:/games/chess.rp6502\27\r");
    ASSERT_STREQ(last, "load USB0:/games/");
    wire("load USB0:/games/chess.rp6502\27\27\r");
    ASSERT_STREQ(last, "load USB0:/");
    wire("load USB0:/games/chess.rp6502\27\27\27\r");
    ASSERT_STREQ(last, "load ");
    wire("dir C:\\x\\my_file-2.txt\27\r");
    ASSERT_STREQ(last, "dir C:\\x\\");
    wire("a=b.c d\33[H\33[1;5Cx\r");
    ASSERT_STREQ(last, "a=b.cx d");
}

/* Ctrl-T swaps the character before the cursor with the one at it and
 * steps past them, and at the line's end swaps the last two. At the line's
 * start, and in a line of one character, it does nothing. */
UTEST(rln, ctrl_t_swaps_two_characters)
{
    editing();
    wire("laod\33[D\33[D\24x\r");
    ASSERT_STREQ(last, "loaxd");
    wire("ab\24\24\r");
    ASSERT_STREQ(last, "ab");
    wire("abc\24\r");
    ASSERT_STREQ(last, "acb");
    wire("ab\33[H\24x\r");
    ASSERT_STREQ(last, "xab");
    wire("a\24\r");
    ASSERT_STREQ(last, "a");
}

/* Ctrl-Delete kills the word after the cursor, and Delete with any other
 * modifier does nothing. */
UTEST(rln, ctrl_delete_kills_the_word_after_the_cursor)
{
    editing();
    wire("ab cd ef\33[H\33[3;5~\33[3;3~\33[3;2~\33[3;6~\33[3;7~\r");
    ASSERT_STREQ(last, " cd ef");
    wire("ab cd ef\33[H\33[3;5~\33[3;5~\33[F\31\r");
    ASSERT_STREQ(last, " efab cd");
}

/* A modifier that means nothing on a key makes the key do nothing: Up,
 * Down, Insert, Home and End with Ctrl or Alt, Up, Down and Insert with
 * Shift too, and Left and Right with Ctrl and Alt together. Up alone
 * still brings a line back after them all. */
UTEST(rln, a_modifier_with_no_meaning_makes_a_key_do_nothing)
{
    editing();
    wire("one\r");
    wire("x\33[1;5A\33[1;3A\33[1;2A\33[1;5B\33[1;2B\r");
    ASSERT_STREQ(last, "x");
    static const char *const inserts[] = {"\33[2;2~", "\33[2;3~", "\33[2;5~", "\33[2;6~"};
    for (unsigned i = 0; i < sizeof inserts / sizeof *inserts; i++)
    {
        wire("ab\33[H");
        wire(inserts[i]);
        wire("x\r");
        ASSERT_STREQ(last, "xab");
    }
    wire("ab\33[1;5H\33[1;3H\33[1;6H\33[1;5~\33[7;3~x\r");
    ASSERT_STREQ(last, "abx");
    wire("ab\33[H\33[1;5F\33[1;3F\33[1;6F\33[4;5~\33[8;3~x\r");
    ASSERT_STREQ(last, "xab");
    wire("ab cd\33[1;7D\33[1;8Dx\33[H\33[1;7Cy\r");
    ASSERT_STREQ(last, "yab cdx");
    wire("\33[A\33[A\33[A\33[A\33[A\33[A\r");
    ASSERT_STREQ(last, "one");
}

/* Ctrl-Y puts back what the kill keys took. Kills one after another are
 * one piece, in the order they had in the line: here three words killed
 * back from the line's end. What was killed stays for the next line. */
UTEST(rln, kills_in_a_row_are_put_back_as_one)
{
    editing();
    wire("load one two three\27\27\27\31\r");
    ASSERT_STREQ(last, "load one two three");
    wire("\31\r");
    ASSERT_STREQ(last, "one two three");
    wire("ab cd ef\33[H\33d\33d\33[F\31\r");
    ASSERT_STREQ(last, " efab cd");
    wire("abcdef\33[D\33[D\33[D\13\25\31\r");
    ASSERT_STREQ(last, "abcdef");
    wire("ab cd ef\33[D\33[D\33[D\33\177\33d\31\r");
    ASSERT_STREQ(last, "ab cd ef");
}

/* Any other key between two kills starts the piece again: a character,
 * a move, and a step through the history. What a line killed is kept past
 * its Enter, until the next line's kill takes its place. A kill with
 * nothing to take changes nothing, and leaves a run it is in going. */
UTEST(rln, a_key_between_two_kills_starts_again)
{
    editing();
    wire("one two\27x\27\31\r");
    ASSERT_STREQ(last, "one x");
    wire("ab\27\r");
    ASSERT_STREQ(last, "");
    wire("cd\27\31\r");
    ASSERT_STREQ(last, "cd");
    wire("ef\27\33[D\27\27\13\31\r");
    ASSERT_STREQ(last, "ef");
    wire("gh\27i\13\25\13\31\31\r");
    ASSERT_STREQ(last, "ii");
    wire("ab cd\27\13\27\31\r");
    ASSERT_STREQ(last, "ab cd");
    wire("one\r");
    wire("ab cd\27\33[A\27\31\r");
    ASSERT_STREQ(last, "one");
}

/* Ctrl-Y inserts in overwrite mode too, where typing would overwrite. */
UTEST(rln, ctrl_y_inserts_in_overwrite_mode)
{
    editing();
    wire("abcd\25xyz\33[H\33[2~\31\33[2~\r");
    ASSERT_STREQ(last, "abcdxyz");
}

/* Left and Right with a count move that far, as a program's poke of CUF
 * and CUB does. */
UTEST(rln, an_arrow_with_a_count_moves_that_far)
{
    editing();
    wire("abcdef\33[H\33[3Cx\33[2Dy\r");
    ASSERT_STREQ(last, "abycxdef");
}

/* A program that shortens the longest line while a longer one is being
 * read stops the line growing: a character and Ctrl-Y add nothing to it. */
UTEST(rln, a_line_longer_than_it_may_be_now_takes_no_more)
{
    editing();
    wire("abcdefghijkl\25abcdefghijklmnopqrst");
    rln_set_max_length(10);
    wire("X\31\r");
    ASSERT_STREQ(last, "abcdefghijklmnopqrst");
}

/* What Ctrl-Y puts back is as much as the line has room for. */
UTEST(rln, ctrl_y_fills_the_line_and_no_more)
{
    editing();
    rln_set_max_length(4);
    wire("abcd\25xy\31\r");
    ASSERT_STREQ(last, "xyab");
    wire("xyz\33[D\31\r");
    ASSERT_STREQ(last, "xyaz");
}

/* A caller empties its kill buffer by its length, as the monitor does with
 * a line that held a password, and a read handed none kills nothing for
 * Ctrl-Y to put back. */
UTEST(rln, a_kill_buffer_is_its_caller_s)
{
    editing();
    wire("secret\25\r");
    kills.len = 0;
    wire("\31\r");
    ASSERT_STREQ(last, "");
    wire("abc\25");
    killing_with = NULL;
    wire("\r");
    wire("def\27\31");
    killing_with = &kills;
    wire("\r");
    ASSERT_STREQ(last, "");
    wire("\31\r");
    ASSERT_STREQ(last, "abc");
}

/* The terminal wraps a plain line. */
UTEST(rln, a_plain_line_has_no_row_breaks)
{
    fresh();
    rln_set_plain(true);
    capture();
    reading();
    type_n(100, 'a');
    manifold_set_tx_tap(NULL);
    ASSERT_EQ(strspn(shown, "a"), (size_t)100);
    ASSERT_EQ(shown_len, (size_t)100);
}

static com_source_t line_src;

static void got_script_line(bool timeout, const char *buf)
{
    (void)timeout;
    lines++;
    strncpy(last, buf, sizeof last - 1);
    last[sizeof last - 1] = 0;
    line_src = rln_line_source();
}

UTEST(rln, a_script_read_sends_nothing_but_the_echo)
{
    fresh();
    capture();
    screen[0] = 0;
    manifold_set_term_out(screen_out);
    lines = 0;
    rln_read_line_script(got_script_line, 1000);
    wire("$400 $CB981D36\r");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "$400 $CB981D36");
    ASSERT_STREQ(shown, "$400 $CB981D36\r\n");
    ASSERT_STREQ(screen, "$400 $CB981D36\r\n");
    screen_done();
}

UTEST(rln, the_line_source_is_the_source_of_the_enter)
{
    fresh();
    lines = 0;
    rln_read_line_script(got_script_line, 1000);
    typed_push("ab", 2);
    tasks();
    wire("\r");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "ab");
    ASSERT_EQ(line_src, COM_SOURCE_UART);
    rln_read_line_script(got_script_line, 1000);
    typed_push("cd\r", 3);
    tasks();
    ASSERT_EQ(lines, 2);
    ASSERT_EQ(line_src, COM_SOURCE_KEYBOARD);
    rln_read_line_script(got_script_line, 1000);
    rln_poke("ef\r");
    ASSERT_EQ(lines, 3);
    ASSERT_STREQ(last, "ef");
    ASSERT_EQ(line_src, COM_SOURCE_ANY);
}

UTEST(rln, a_poke_of_an_empty_string_ends_the_line)
{
    editing();
    rln_poke("ab");
    ASSERT_EQ(lines, 0);
    rln_poke("");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "ab");
}

/* ----- Keys and sequences ----- */

/* Time passes with vga_task, and the tasks run after. */
static void quiet_ms(uint32_t ms)
{
    timer_mach_t d = timer_mach_in_ms(ms);
    while (!timer_mach_passed(d))
        vga_task();
    tasks();
}

UTEST(rln, an_esc_inside_a_sequence_starts_it_over)
{
    editing();
    wire("ab\33\33[D\33[1\33[Dx\r");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "xab");
}

UTEST(rln, a_control_inside_a_sequence_acts)
{
    editing();
    wire("abc\33[1\177\33[\r");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "ab");
}

UTEST(rln, ctrl_x_is_a_key)
{
    editing();
    wire("\30");
    ASSERT_EQ(lastkey_len(), 1);
}

UTEST(rln, a_lone_esc_is_the_escape_key)
{
    editing();
    wire("\33");
    ASSERT_EQ(lastkey_len(), 0);
    quiet_ms(TIO_ESC_MS);
    ASSERT_EQ(lastkey_len(), 1);
}

/* Alt+[ is dropped and takes no key with it. */
UTEST(rln, an_unfinished_sequence_does_not_take_the_next_key)
{
    editing();
    wire("ab\33[");
    quiet_ms(TIO_ESC_MS);
    wire("D\r");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "abD");
}

UTEST(rln, a_utf8_character_in_a_sequence_comes_through_whole)
{
    editing();
    wire("a\33[1\303\251\r");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "a\303\251");
}

UTEST(rln, a_mode_report_with_an_intermediate_types_nothing)
{
    editing();
    wire("a\33[?2004;1$yb\33[4;2$y");
    ASSERT_EQ(lastkey_len(), 1);
    wire("c\r");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "abc");
}

/* rxvt's Shift-Home and Shift-End select as xterm's do: Shift-Home takes
 * "bc", which the 'a' replaces, and Shift-End at the end selects nothing,
 * and from the start takes the line. Its End, and the VT220's, move there.
 * Its shifted Insert and Delete are no key of the line editor's. */
UTEST(rln, rxvt_shifted_keys_end_on_a_dollar)
{
    editing();
    wire("bc\33[7$a\33[8$d\r");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "ad");
    wire("bc\33[H\33[8$a\r");
    ASSERT_STREQ(last, "a");
    wire("bc\33[H\33[8~a\33[H\33[4~d\r");
    ASSERT_STREQ(last, "bcad");
    wire("bc\33[H\33[2$\33[3$a\r");
    ASSERT_STREQ(last, "abc");
}

UTEST(rln, a_linux_console_function_key_is_one_key)
{
    editing();
    wire("a\33[[A");
    ASSERT_EQ(lastkey_len(), 4);
    wire("b\r");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "ab");
}

UTEST(rln, an_x10_mouse_report_types_nothing)
{
    editing();
    wire("a\33[M !!b\r");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "ab");
}

/* The arrows, Home and End of application cursor mode edit the line. The
 * keypad's keys of application keypad mode, its Enter too, are the
 * program's that asked for them: they do nothing to the line, and are the
 * last key for the program to read. */
UTEST(rln, ss3_arrows_move_and_the_keypad_is_the_program_s)
{
    editing();
    wire("ab\33OD\33ODx\33OC\33OC\33OHy\33OFz");
    wire("\33Oq\33Ok\33OX\33O5P\33OM");
    ASSERT_EQ(lines, 0);
    ASSERT_EQ(lastkey_len(), 3);
    wire("\r");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "yxabz");
}

/* Shift-F3 is CSI 1;2R, which looks like a cursor position report. */
UTEST(rln, a_cpr_shaped_key_is_a_key)
{
    editing();
    wire("\33[1;2R");
    ASSERT_EQ(lastkey_len(), 6);
}

/* Shift-F1 is CSI 1;2P, which looks like DCH. */
UTEST(rln, typed_editing_commands_do_nothing)
{
    editing();
    wire("abc\33[D\33[D\33[1;2P\33[@\33[X\r");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "abc");
}

UTEST(rln, poked_editing_commands_edit)
{
    editing();
    wire("abc\33[D\33[D");
    rln_poke("\33[P");
    wire("\r");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "ac");
}

UTEST(rln, a_poked_can_cancels_a_sequence_and_ends_the_line_outside_one)
{
    editing();
    rln_poke("a\33[1\30b");
    ASSERT_EQ(lines, 0);
    rln_poke("\30");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "ab");
}

/* The LF of a CR LF pair comes after the next read has started, and is
 * absorbed there. The field is ten characters, so the cursor moves by
 * column. */
UTEST(rln, the_lf_of_a_crlf_pair_keeps_the_next_prompt_column)
{
    fresh();
    rln_set_max_length(10);
    lines = 0;
    need_next = true;
    next();
    wire("a\r");
    ASSERT_EQ(lines, 1);
    com_printf("12345678");
    next();
    wire("\n");
    ASSERT_EQ(lines, 1);
    wire("b");
    capture();
    wire("\33[D");
    manifold_set_tx_tap(NULL);
    ASSERT_STREQ(shown, "\r\33[8C");
}

#define UP "\33[A"
#define DOWN "\33[B"

/* A history is its caller's, with as many lines as its caller's memory has
 * room for: three here, and the oldest goes for the fourth. */
UTEST(rln, a_history_holds_the_lines_its_caller_has_room_for)
{
    fresh();
    room = 3;
    reading();
    wire("one\rtwo\rthree\rfour\r");
    ASSERT_EQ(lines, 4);
    ASSERT_EQ(holding, 3);
    wire(UP UP UP UP "\r");
    ASSERT_EQ(lines, 5);
    ASSERT_STREQ(last, "two");
    /* The same line again is not entered twice. */
    wire("two\r" UP UP "\r");
    ASSERT_STREQ(last, "four");
    ASSERT_EQ(holding, 3);
}

/* The line being typed is there again below the newest line entered, and
 * there is nothing below it or above the oldest. */
UTEST(rln, the_line_being_typed_comes_back)
{
    fresh();
    reading();
    wire("a\rb\r");
    wire("xy" UP UP UP DOWN DOWN DOWN "z\r");
    ASSERT_STREQ(last, "xyz");
    wire(UP UP DOWN "\r");
    ASSERT_STREQ(last, "xyz");
}

/* Lines brought back and changed stay changed while the read looks at
 * others, and take no line of the history. The one entered is added, and
 * every line it was made from is as it was entered. */
UTEST(rln, a_changed_line_is_kept_until_the_read_ends)
{
    fresh();
    reading();
    wire("a\rb\rc\r");
    wire("xy" UP "X" UP "Y" DOWN);
    ASSERT_EQ(holding, 6);
    wire("\r");
    ASSERT_STREQ(last, "cX");
    ASSERT_EQ(holding, 4);
    wire(UP "\r");
    ASSERT_STREQ(last, "cX");
    wire(UP UP "\r");
    ASSERT_STREQ(last, "c");
    wire(UP UP UP UP "\r");
    ASSERT_STREQ(last, "b");
    wire(UP UP UP UP UP UP UP UP UP "\r");
    ASSERT_STREQ(last, "a");
}

/* A change taken back leaves the line as it was entered, kept nowhere
 * else. */
UTEST(rln, a_line_changed_back_is_not_kept)
{
    fresh();
    reading();
    wire("a\rb\r");
    wire(UP "X" UP DOWN "\b" UP DOWN DOWN);
    ASSERT_EQ(holding, 2);
    wire(UP "\r");
    ASSERT_STREQ(last, "b");
}

/* A read that ends with no line entered forgets its changes too. */
UTEST(rln, a_cancelled_read_changes_no_line)
{
    fresh();
    reading();
    wire("a\rb\r");
    wire("xy" UP "X" UP "Y" DOWN);
    rln_poke("\3");
    ASSERT_STREQ(last, "bX");
    ASSERT_EQ(holding, 2);
    wire(UP "\r");
    ASSERT_STREQ(last, "b");
    wire(UP UP "\r");
    ASSERT_STREQ(last, "a");
}

/* A line brought back and emptied enters nothing, and is still there. */
UTEST(rln, an_emptied_line_enters_nothing)
{
    fresh();
    reading();
    wire("a\rb\r");
    wire(UP "\25\r");
    ASSERT_STREQ(last, "");
    ASSERT_EQ(holding, 2);
    wire(UP "\r");
    ASSERT_STREQ(last, "b");
}

/* The line a read entered can be taken out again, and a read that entered
 * none, its line the newest again or empty, takes none out. */
UTEST(rln, the_line_entered_can_be_forgotten)
{
    fresh();
    reading();
    wire("a\r");
    forgetting = true;
    wire("secret\r");
    ASSERT_STREQ(last, "secret");
    ASSERT_EQ(holding, 1);
    forgetting = true;
    wire(UP "\r");
    ASSERT_STREQ(last, "a");
    ASSERT_EQ(holding, 1);
    forgetting = true;
    wire("\r");
    ASSERT_EQ(holding, 1);
    /* One made from a line brought back leaves that line. */
    forgetting = true;
    wire(UP "b\r");
    ASSERT_STREQ(last, "ab");
    ASSERT_EQ(holding, 1);
    wire(UP UP "\r");
    ASSERT_STREQ(last, "a");
    /* The only line. */
    fresh();
    reading();
    forgetting = true;
    wire("only\r");
    ASSERT_EQ(holding, 0);
    wire(UP "\r");
    ASSERT_STREQ(last, "");
}

/* A changed line is kept at the cost of the oldest lines, but never of the
 * one showing: with no room, the change is not kept. */
UTEST(rln, a_changed_line_costs_the_oldest_lines)
{
    fresh();
    room = 2;
    reading();
    wire("a\rb\r");
    /* Leaving b changed costs a, so there is no line to go up to. */
    wire(UP "X" UP "Y\r");
    ASSERT_STREQ(last, "bXY");
    ASSERT_EQ(holding, 2);
    wire(UP UP UP "\r");
    ASSERT_STREQ(last, "b");
    /* The line being typed is kept at that cost too. */
    wire("typed" UP "\r");
    ASSERT_STREQ(last, "b");
    ASSERT_EQ(holding, 1);
    wire(UP UP "\r");
    ASSERT_STREQ(last, "b");
    /* The line showing is the oldest, and stays. */
    fresh();
    room = 1;
    reading();
    wire("a\r");
    wire(UP "X" DOWN UP "\r");
    ASSERT_STREQ(last, "a");
    ASSERT_EQ(holding, 1);
}

UTEST(rln, a_read_handed_no_history_keeps_and_recalls_nothing)
{
    fresh();
    reading();
    wire("kept\r");
    reading_with = NULL;
    reading();
    wire("secret\r");
    wire("\33[A\r");
    ASSERT_STREQ(last, "");
    reading_with = &history;
    reading();
    wire("\33[A\r");
    ASSERT_STREQ(last, "kept");
    ASSERT_EQ(holding, 1);
}

/* ----- Where the line is (step 24) ----- */

/* The line's indexes on the screen: the prompt is outside it, and the
 * place after its last character is in it. */
UTEST(rln, the_line_knows_its_place_after_a_prompt)
{
    fresh();
    com_printf("> ");
    reading();
    wire("abc");
    ASSERT_EQ(rln_index_at(0, 1), -1);
    ASSERT_EQ(rln_index_at(0, 2), 0);
    ASSERT_EQ(rln_index_at(0, 4), 2);
    ASSERT_EQ(rln_index_at(0, 5), 3);
    ASSERT_EQ(rln_index_at(0, 6), -1);
    ASSERT_EQ(rln_index_at(1, 2), -1);
}

UTEST(rln, a_wrapped_line_knows_its_rows)
{
    fresh();
    com_printf("> ");
    reading();
    type_n(85, 'a');
    ASSERT_EQ(rln_index_at(0, 79), 77);
    ASSERT_EQ(rln_index_at(1, 0), 78);
    ASSERT_EQ(rln_index_at(1, 7), 85);
    ASSERT_EQ(rln_index_at(1, 8), -1);
}

/* A row break on the bottom row scrolls the line up a row, at the screen's
 * bottom and at a region's. */
UTEST(rln, a_line_scrolled_at_the_bottom_margin_follows_its_rows)
{
    fresh();
    const unsigned h = rln_view()->height;
    char at[16];
    snprintf(at, sizeof at, "\33[%u;1H> ", h);
    com_printf("%s", at);
    reading();
    type_n(85, 'a');
    ASSERT_EQ(rln_index_at((uint8_t)(h - 2), 2), 0);
    ASSERT_EQ(rln_index_at((uint8_t)(h - 1), 0), 78);
    fresh();
    com_printf("\33[5;10r\33[10;1H> ");
    reading();
    type_n(85, 'a');
    ASSERT_EQ(rln_index_at(8, 2), 0);
    ASSERT_EQ(rln_index_at(9, 0), 78);
    ASSERT_EQ(rln_index_at(10, 0), -1);
}

/* A row of double width has half the columns, and the line counts them. */
UTEST(rln, a_line_on_a_double_width_row_counts_its_own_columns)
{
    fresh();
    com_printf("\33#6> ");
    reading();
    wire("abc");
    ASSERT_EQ(rln_index_at(0, 2), 0);
    ASSERT_EQ(rln_index_at(0, 5), 3);
    type_n(40, 'b');
    ASSERT_EQ(rln_index_at(0, 39), 37);
    ASSERT_EQ(rln_index_at(0, 40), -1);
    ASSERT_EQ(rln_index_at(1, 0), 38);
}

/* Output written while a line is read is not followed: the line stays
 * where it was laid out, which the next read lays out again. */
UTEST(rln, output_during_a_read_leaves_the_line_where_it_was_laid_out)
{
    fresh();
    com_printf("> ");
    lines = 0;
    need_next = true;
    next();
    wire("ab");
    com_printf("\r\n\r\nout");
    ASSERT_EQ(rln_index_at(0, 2), 0);
    ASSERT_EQ(rln_index_at(2, 0), -1);
    wire("\r");
    ASSERT_EQ(lines, 1);
    com_printf("\r\n> ");
    next();
    tasks();
    const uint8_t y = pos()->y;
    ASSERT_EQ(rln_index_at(y, 2), 0);
    ASSERT_EQ(rln_index_at(0, 2), -1);
}

/* A plain line and a line on a program's own size have no place. */
UTEST(rln, plain_and_pinned_lines_have_no_place)
{
    fresh();
    rln_set_plain(true);
    reading();
    wire("ab");
    ASSERT_EQ(rln_index_at(0, 0), -1);
    rln_set_plain(false);
    fresh();
    rln_set_term_width(60);
    reading();
    wire("ab");
    ASSERT_EQ(rln_index_at(0, 0), -1);
    rln_set_term_width(0);
}

/* ----- Shift selects, and the terminal's selection shows it ----- */

static bool rev(int y, int x)
{
    return (tb_screen_row((uint8_t)y)[x].attributes & MODE0_ATTR_REVERSE) != 0;
}

/* Whether the highlight, the console's terminal's selection, takes column
 * x of row y of the screen, as mode 0's row word has it. */
static bool hl(int y, int x)
{
    draw_publish();
    const uint32_t w = mode0_regs.row[y];
    return x >= MODE0_ROW_FROM(w) && x < MODE0_ROW_TO(w);
}

/* The selection, from row y0, column x0 of the screen showing up to row
 * y1, column x1. */
static bool selected(int y0, int x0, int y1, int x1)
{
    const term_grid_t *g = vterm_get_shown(&vterm);
    const screen_selection_t *v = &vterm_selection;
    return v->shows && v->screen == g->shown &&
           v->start_row == screen_mem(g->shown, (uint8_t)y0) && v->start_col == x0 &&
           v->end_row == screen_mem(g->shown, (uint8_t)y1) && v->end_col == x1;
}

/* Reading on the screen after what pre prints, keeping what goes to it. */
static void screen_reading_after(const char *pre)
{
    fresh();
    com_printf("%s", pre);
    screen[0] = 0;
    manifold_set_term_out(screen_out);
    capture();
    lines = 0;
    need_next = true;
    next();
}

/* Shift-Left moves and selects the cell it passed: the reply is the move
 * alone, and the highlight is the terminal's selection, which nothing
 * sent draws. */
UTEST(rln, shift_with_the_arrows_selects)
{
    screen_reading_after("");
    wire("abcd");
    screen[0] = 0;
    wire("\33[1;2D");
    ASSERT_STREQ(screen, "\r\33[3C");
    ASSERT_TRUE(selected(0, 3, 0, 4));
    ASSERT_TRUE(hl(0, 3));
    ASSERT_FALSE(hl(0, 2));
    screen[0] = 0;
    wire("\33[1;2D");
    ASSERT_STREQ(screen, "\r\33[2C");
    ASSERT_TRUE(selected(0, 2, 0, 4));
    wire("\33[1;2C");
    ASSERT_FALSE(hl(0, 2));
    ASSERT_TRUE(hl(0, 3));
    wire("\33[C");
    ASSERT_FALSE(vterm_selection.shows);
    ASSERT_TRUE(strstr(shown, "$t") == NULL);
    for (int x = 0; x < 4; x++)
        ASSERT_FALSE(rev(0, x));
    wire("\r");
    ASSERT_STREQ(last, "abcd");
    screen_done();
}

/* Ctrl and Alt still move by words, and select nothing. */
UTEST(rln, ctrl_with_the_arrows_still_moves_by_word)
{
    screen_reading_after("");
    wire("ab cd");
    screen[0] = 0;
    wire("\33[1;5D\33[1;3D");
    ASSERT_FALSE(vterm_selection.shows);
    wire("x\r");
    ASSERT_STREQ(last, "xab cd");
    screen_done();
}

/* Shift with Home and End, in xterm's and the VT220's forms, selects, a
 * character typed replaces the selection, and Delete deletes it. */
UTEST(rln, shift_with_home_and_end_selects)
{
    screen_reading_after("");
    wire("abcd\33[1;2H");
    ASSERT_TRUE(selected(0, 0, 0, 4));
    wire("x");
    ASSERT_FALSE(vterm_selection.shows);
    wire("yz\33[H\33[1;2F\33[3~q\r");
    ASSERT_STREQ(last, "q");
    next();
    const int y = pos()->y;
    wire("abcd\33[1;2~\33[C\33[4;2~");
    ASSERT_TRUE(selected(y, 1, y, 4));
    wire("\33[3~\r");
    ASSERT_STREQ(last, "a");
    screen_done();
}

UTEST(rln, backspace_deletes_the_selection)
{
    screen_reading_after("");
    wire("abcd\33[1;2D\33[1;2D");
    ASSERT_TRUE(selected(0, 2, 0, 4));
    wire("\b");
    ASSERT_FALSE(vterm_selection.shows);
    wire("\r");
    ASSERT_STREQ(last, "ab");
    screen_done();
}

/* A poke ends the selection and edits as a poke does. */
UTEST(rln, a_poke_ends_the_selection)
{
    screen_reading_after("");
    wire("abcd\33[1;2D");
    ASSERT_TRUE(hl(0, 3));
    rln_poke("x");
    ASSERT_FALSE(vterm_selection.shows);
    rln_poke("\r");
    ASSERT_STREQ(last, "abcx");
    screen_done();
}

/* A RIS of the console's terminal at the same width, as a change of code
 * page makes, erases the line and its highlight: the line has no place
 * after it, and the next key that selects shows nothing over the erased
 * cells. */
UTEST(rln, a_ris_erases_the_line_and_its_highlight)
{
    screen_reading_after("");
    wire("abcd\33[1;2D");
    ASSERT_TRUE(hl(0, 3));
    vterm_RIS();
    ASSERT_FALSE(vterm_selection.shows);
    ASSERT_EQ(rln_index_at(0, 2), -1);
    wire("\33[1;2D");
    ASSERT_FALSE(vterm_selection.shows);
    wire("\r");
    ASSERT_STREQ(last, "abcd");
    screen_done();
}

/* A new canvas of the same width takes the highlight away, and the
 * selection ends with it: a typed character goes in at the cursor and
 * replaces nothing, as nothing shows selected. */
UTEST(rln, a_new_canvas_ends_the_selection_with_its_highlight)
{
    screen_reading_after("");
    wire("abcd\33[1;2D");
    ASSERT_TRUE(hl(0, 3));
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_640_480));
    tasks();
    ASSERT_FALSE(vterm_selection.shows);
    wire("x\r");
    ASSERT_STREQ(last, "abcxd");
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
    screen_done();
}

/* After a RIS the line's selection is gone too: a typed character
 * replaces nothing. */
UTEST(rln, a_ris_ends_the_selection)
{
    screen_reading_after("");
    wire("abcd\33[1;2D");
    ASSERT_TRUE(hl(0, 3));
    vterm_RIS();
    tasks();
    wire("x\r");
    ASSERT_STREQ(last, "abcxd");
    screen_done();
}

/* The highlight is off when the read ends, by Enter, a stop or a cancel. */
UTEST(rln, a_read_that_ends_leaves_no_highlight)
{
    screen_reading_after("");
    wire("abcd\33[1;2H");
    ASSERT_TRUE(hl(0, 0));
    wire("\r");
    ASSERT_EQ(lines, 1);
    ASSERT_FALSE(vterm_selection.shows);
    next();
    wire("efgh\33[1;2H");
    ASSERT_TRUE(hl(1, 0));
    rln_stop();
    ASSERT_FALSE(vterm_selection.shows);
    need_next = true;
    next();
    tasks();
    wire("ijkl\33[1;2H");
    ASSERT_TRUE(hl(pos()->y, 0));
    rln_read_cancel();
    ASSERT_FALSE(vterm_selection.shows);
    screen_done();
}

/* The highlight is drawn over the cells, which it never changes: a cell
 * drawn in reverse keeps its reverse, and shows as it is not reversed
 * while it is selected, as mode 0 draws the selection. */
UTEST(rln, a_cell_drawn_reversed_stays_reversed)
{
    screen_reading_after("\33[7m");
    wire("ab");
    ASSERT_TRUE(rev(0, 1));
    wire("\33[1;2D");
    ASSERT_TRUE(rev(0, 1));
    ASSERT_TRUE(hl(0, 1));
    wire("\33[C");
    ASSERT_TRUE(rev(0, 1));
    ASSERT_FALSE(hl(0, 1));
    screen_done();
}

/* The selection is in the screen's rows and columns, whatever DECOM: the
 * line in a scrolling region and between left and right margins. */
UTEST(rln, the_highlight_is_where_the_line_is_under_decom)
{
    screen_reading_after("\33[5;20r\33[?6h\33[H");
    wire("ab\33[1;2D");
    ASSERT_TRUE(selected(4, 1, 4, 2));
    ASSERT_TRUE(hl(4, 1));
    screen_done();
    screen_reading_after("\33[?69h\33[11;30s\33[?6h\33[H");
    wire("ab\33[1;2D");
    ASSERT_TRUE(selected(0, 11, 0, 12));
    ASSERT_TRUE(hl(0, 11));
    ASSERT_FALSE(hl(0, 10));
    screen_done();
}

/* A row of double width is selected in its own columns. */
UTEST(rln, a_double_width_row_is_selected_in_its_own_columns)
{
    screen_reading_after("\33#6");
    wire("abcd\33[1;2D");
    ASSERT_TRUE(selected(0, 3, 0, 4));
    ASSERT_TRUE(hl(0, 3));
    ASSERT_FALSE(hl(0, 2));
    screen_done();
}

/* A line of ten rows between margins 20 columns apart: a selection of all
 * of it is one selection across the rows it wraps onto, from its first
 * character to its last, between the margins: no column outside them is
 * highlighted. It goes when it ends. */
UTEST(rln, ten_rows_are_one_selection)
{
    for (int left = 0; left <= 10; left += 10)
    {
        char margins[32];
        snprintf(margins, sizeof margins, "\33[?69h\33[%d;%ds\33[1;%dH", left + 1, left + 20,
                 left + 1);
        screen_reading_after(margins);
        type_n(199, 'a');
        wire("\33[1;2H");
        ASSERT_TRUE(selected(0, left, 9, left + 19));
        ASSERT_EQ(vterm_selection.left, left);
        ASSERT_EQ(vterm_selection.right, left + 20);
        for (int y = 0; y < 10; y++)
        {
            for (int x = left; x < left + (y < 9 ? 20 : 19); x++)
                ASSERT_TRUE(hl(y, x));
            ASSERT_FALSE(hl(y, left + 20));
            if (left)
                ASSERT_FALSE(hl(y, left - 1));
        }
        ASSERT_FALSE(hl(9, left + 19));
        wire("\33[1;2F");
        ASSERT_FALSE(vterm_selection.shows);
        for (int y = 0; y < 10; y++)
            ASSERT_FALSE(hl(y, left));
        screen_done();
    }
}

/* A terminal in VT52 mode has no shifted arrows. */
UTEST(rln, vt52_has_no_shift_selection)
{
    screen_reading_after("\33[?2l");
    wire("abcd\33D\33D");
    ASSERT_FALSE(vterm_selection.shows);
    wire("x\r");
    ASSERT_STREQ(last, "abxcd");
    com_printf("\33<");
    screen_done();
}

/* The highlight waits for the echo: output over a selected cell drops the
 * terminal's selection, so a selection made while the line's echo is still
 * going out at a slow speed (DECSCS) shows only once the echo has reached
 * the terminal, and is not dropped by it. */
UTEST(rln, the_highlight_waits_for_the_echo)
{
    screen_reading_after("");
    manifold_set_paced(true);
    com_printf("\33[0;1*r");
    tasks();
    wire("abcd\33[1;2D");
    ASSERT_TRUE(rln_replying() || !manifold_drained());
    ASSERT_FALSE(vterm_selection.shows);
    for (int i = 0; i < 200 && (rln_replying() || !manifold_drained()); i++)
    {
        timer_mach_t d = timer_mach_in_ms(10);
        while (!timer_mach_passed(d))
            vga_task();
        manifold_task();
        tasks();
    }
    tasks();
    ASSERT_TRUE(selected(0, 3, 0, 4));
    manifold_set_paced(false);
    com_printf("\33[0;0*r");
    screen_done();
}

/* ----- Bracketed paste (step 28) ----- */

/* A read with history asks for bracketed paste, and turns it off when it
 * ends, after the line's newline, so a terminal sees the echo and its line
 * end unbroken, as a monitor command's output follows them (build.py). */
UTEST(rln, a_history_read_brackets_its_pastes_while_it_reads)
{
    fresh();
    capture();
    lines = 0;
    need_next = true;
    next();
    tasks();
    ASSERT_TRUE(vterm.bracketed_paste);
    wire("a\r");
    manifold_set_tx_tap(NULL);
    ASSERT_EQ(lines, 1);
    ASSERT_TRUE(strstr(shown, "a\r\n\33[?2004l") != NULL);
    ASSERT_FALSE(vterm.bracketed_paste);
}

/* A bracketed paste is taken up to its first line end, and the rest of it
 * is dropped, the next read's included; without brackets it is typing. */
UTEST(rln, a_bracketed_paste_takes_its_first_line)
{
    editing();
    wire("\33[200~one\rtwo\rthree\33[201~four\r");
    ASSERT_EQ(lines, 2);
    ASSERT_STREQ(last, "four");
    wire("one\rtwo\r");
    ASSERT_EQ(lines, 4);
    ASSERT_STREQ(last, "two");
}

/* A paste whose closing bracket never comes is let go after 64 KiB. */
UTEST(rln, a_missing_closing_bracket_is_bounded)
{
    editing();
    wire("\33[200~a\r");
    ASSERT_EQ(lines, 1);
    char chunk[128];
    memset(chunk, 'x', sizeof chunk);
    for (unsigned n = 0; n < 65535 / sizeof chunk; n++)
    {
        manifold_uart_push(chunk, sizeof chunk);
        tasks();
    }
    type_n(65535 % sizeof chunk, 'x');
    ASSERT_EQ(lines, 1);
    wire("b\r");
    ASSERT_EQ(lines, 2);
    ASSERT_STREQ(last, "b");
}

/* A stop ends a source's paste, and so does its terminal going away. */
UTEST(rln, a_stop_ends_a_paste_being_dropped)
{
    editing();
    wire("\33[200~a\r");
    rln_stop();
    reading();
    wire("b\r");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "b");
    wire("\33[200~c\r");
    rln_forget_source(COM_SOURCE_UART);
    wire("d\r");
    ASSERT_STREQ(last, "d");
}

/* A load in the middle of a read with history keeps its asking, and the
 * read still turns bracketed paste off at its end. */
UTEST(rln, a_load_keeps_a_history_read_s_bracketed_paste)
{
    sys_init();
    fresh();
    static uint8_t blob[1 << 20];
    rln_read_line(console_rln_reader(), &history, &kills);
    tasks();
    ASSERT_TRUE(vterm.bracketed_paste);
    ASSERT_EQ(sst_save(blob, sizeof blob, 0), (const char *)NULL);
    rln_read_cancel();
    ASSERT_FALSE(vterm.bracketed_paste);
    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_TRUE(vterm.bracketed_paste);
    capture();
    wire("a\r");
    manifold_set_tx_tap(NULL);
    ASSERT_TRUE(strstr(shown, "\33[?2004l") != NULL);
    ASSERT_FALSE(vterm.bracketed_paste);
}

/* With the line's selection showing, the keys that kill a word kill the
 * selection, which is then the word, whichever end the cursor is at. */
UTEST(rln, the_word_kills_kill_the_selection)
{
    static const char *const keys[] = {"\27", "\33\177", "\33d", "\33[3;5~"};
    for (unsigned i = 0; i < sizeof keys / sizeof *keys; i++)
    {
        screen_reading_after("");
        wire("ab cd ef\33[D\33[1;2D\33[1;2D\33[1;2D");
        wire(keys[i]);
        wire("\33[H\31\r");
        ASSERT_STREQ(last, "d eab cf");
        next();
        kills.len = 0;
        wire("ab cd ef\33[D\33[D\33[D\33[D\33[1;2C\33[1;2C\33[1;2C");
        wire(keys[i]);
        wire("\33[H\31\r");
        ASSERT_STREQ(last, "d eab cf");
        screen_done();
    }
}

/* Alt and Ctrl together kill the selection too, where there is one. */
UTEST(rln, the_argument_kills_kill_the_selection)
{
    screen_reading_after("");
    wire("ab cd ef\33[1;2D\33[1;2D\33\4\33[H\31\r");
    ASSERT_STREQ(last, "efab cd ");
    next();
    kills.len = 0;
    wire("ab cd ef\33[1;2D\33[1;2D\33\10\33[H\31\r");
    ASSERT_STREQ(last, "efab cd ");
    screen_done();
}

/* Ctrl-K and Ctrl-U kill from the cursor to the line's end and its start,
 * whatever is selected: the selection only ends. */
UTEST(rln, the_line_kills_kill_from_the_cursor)
{
    screen_reading_after("");
    wire("abcdef\33[D\33[1;2D\33[1;2D\13\r");
    ASSERT_STREQ(last, "abc");
    next();
    wire("abcdef\33[D\33[1;2D\33[1;2D\25\r");
    ASSERT_STREQ(last, "def");
    next();
    wire("abcdef\33[D\33[D\33[D\33[1;2C\33[1;2C\13\r");
    ASSERT_STREQ(last, "abcde");
    next();
    wire("abcdef\33[D\33[D\33[D\33[1;2C\33[1;2C\25\r");
    ASSERT_STREQ(last, "f");
    screen_done();
}

/* Ctrl-Y puts what was killed in the selection's place, and with nothing
 * killed leaves the selection's text alone. */
UTEST(rln, ctrl_y_replaces_the_selection)
{
    screen_reading_after("");
    wire("xyz\33[1;2D\33[1;2D\31\r");
    ASSERT_STREQ(last, "xyz");
    next();
    wire("abc\27xyz\33[1;2D\33[1;2D\31!\r");
    ASSERT_STREQ(last, "xabc!");
    screen_done();
}

/* A program that set modifyOtherKeys has the keyboard send keys as
 * sequences, ESC [ 27 ; mod ; code ~, or ESC [ code ; mod u. Each is the
 * key it stands for, at every level: text is typed, and Enter ends the
 * line with Shift or Ctrl as it does alone. */
UTEST(rln, a_modified_key_sequence_types_its_character)
{
    editing();
    wire("\33[27;1;104~\33[27;2;73~\33[27;1;233~\33[27;2;32~\33[27;5;49~\33[27;1;13~");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "hI\351 1");
    wire("\33[104;1u\33[73;2u\33[13;2u");
    ASSERT_EQ(lines, 2);
    ASSERT_STREQ(last, "hI");
    wire("a\33[27;5;13~");
    ASSERT_EQ(lines, 3);
    wire("b\33[27;3;13~");
    ASSERT_EQ(lines, 3);
    ASSERT_EQ(lastkey_len(), 10);
    wire("\r");
    ASSERT_STREQ(last, "b");
}

/* The Ctrl and Alt keys are the same keys as sequences: Ctrl-A and Ctrl-E,
 * Ctrl-Shift-A which is Ctrl-A, Ctrl-W twice and Ctrl-Y, Alt-B and Alt-D,
 * Alt-Backspace, Backspace and what Ctrl makes of it, and Alt and Ctrl
 * together going by arguments. */
UTEST(rln, a_modified_key_sequence_is_its_ctrl_or_alt_key)
{
    editing();
    wire("ab cd\33[27;5;97~x\33[27;5;101~y\r");
    ASSERT_STREQ(last, "xab cdy");
    wire("ab\33[27;6;65~x\33[101;5uy\r");
    ASSERT_STREQ(last, "xaby");
    wire("ab cd\33[27;5;119~\33[27;5;119~\33[27;5;121~\r");
    ASSERT_STREQ(last, "ab cd");
    wire("ab cd\33[27;3;98~\33[27;3;100~x\r");
    ASSERT_STREQ(last, "ab x");
    wire("ab cd\33[27;3;127~\33[27;4;66~x\r");
    ASSERT_STREQ(last, "xab ");
    wire("abcd\33[27;1;127~\33[27;1;8~\33[27;2;127~\r");
    ASSERT_STREQ(last, "a");
    wire("a:b c:d\33[27;7;98~x\r");
    ASSERT_STREQ(last, "a:b xc:d");
    wire("ab cd\33[98;3ux\33[100;7u\r");
    ASSERT_STREQ(last, "ab x");
    /* Alt with a character past ASCII types it, as ESC before its byte
     * does. */
    wire("a\33[27;3;233~\33[233;3ub\r");
    ASSERT_STREQ(last, "a\351\351b");
    /* Ctrl-J after Enter is its line end's other half. */
    lines = 0;
    wire("a\33[27;1;13~\33[27;5;106~");
    ASSERT_EQ(lines, 1);
}

/* A sequence for a key that does nothing does nothing: Tab, Esc, Alt with
 * a letter that is no key, Alt-[ and Alt-O, which open no sequence, and
 * one with a modifier or a code no key has. Each is still the last key, as
 * its terminal sent it. */
UTEST(rln, a_modified_key_sequence_for_no_key_does_nothing)
{
    editing();
    wire("a\33[27;1;9~\33[27;2;9~\33[27;1;27~\33[27;5;32~\33[27;3;120~");
    wire("\33[27;3;91~\33[27;3;79~\33[27;9;98~\33[27;5~\33[27;1;300~\33[98;9u");
    ASSERT_EQ(lastkey_len(), 7);
    wire("b\r");
    ASSERT_STREQ(last, "ab");
}

/* A sequence acts on the selection as its key does, and is a line end's
 * other half as its byte is. A poked one is its key too. */
UTEST(rln, a_modified_key_sequence_is_its_key_everywhere)
{
    screen_reading_after("");
    wire("abc\33[1;2D\33[1;2D\33[27;1;120~\r");
    ASSERT_STREQ(last, "ax");
    next();
    wire("ab cd\33[1;2D\33[1;2D\33[27;5;119~\33[27;5;121~\r");
    ASSERT_STREQ(last, "ab cd");
    next();
    lines = 0;
    wire("a\33[27;1;13~");
    next();
    wire("\n");
    ASSERT_EQ(lines, 1);
    rln_poke("abc");
    rln_poke("\33[27;5;97~");
    rln_poke("x");
    wire("\r");
    ASSERT_STREQ(last, "xbc");
    screen_done();
}

/* Shift with Ctrl or Alt and Left or Right selects by words. */
UTEST(rln, shift_with_ctrl_or_alt_selects_by_words)
{
    screen_reading_after("");
    wire("ab cd ef\33[1;6D\33[1;4Dx\r");
    ASSERT_STREQ(last, "ab x");
    next();
    wire("ab cd ef\33[H\33[1;6C\33[1;4Cx\r");
    ASSERT_STREQ(last, "x ef");
    screen_done();
}

/* A load in the middle of a program's read gives the read the console's
 * kill buffer again, holding what the program had killed. The line the
 * kill emptied is saved with no selection, which a load would refuse were
 * it left past the line's end. */
UTEST(rln, a_load_keeps_what_a_program_killed)
{
    sys_init();
    fresh();
    static uint8_t blob[1 << 20];
    rln_read_line(console_rln_reader(), NULL, console_rln_kill());
    tasks();
    wire("abc\27");
    ASSERT_EQ(sst_save(blob, sizeof blob, 0), (const char *)NULL);
    rln_read_cancel();
    console_rln_kill()->len = 0;
    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    wire("\31!");
    ASSERT_EQ(strncmp(rln_line(), "abc!", 4), 0);
    wire("\r");
}

/* A load ends a run of kills, as a key between two kills does: the kill
 * after it starts what Ctrl-Y puts back again. */
UTEST(rln, a_load_ends_a_run_of_kills)
{
    sys_init();
    fresh();
    static uint8_t blob[1 << 20];
    rln_read_line(console_rln_reader(), NULL, console_rln_kill());
    tasks();
    wire("abc def\27");
    ASSERT_EQ(sst_save(blob, sizeof blob, 0), (const char *)NULL);
    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    wire("\27\31!");
    ASSERT_EQ(strncmp(rln_line(), "abc !", 5), 0);
    wire("\r");
}

/* A program starts with nothing killed: the stop of the one before it
 * empties the console's kill buffer. */
UTEST(rln, a_stop_empties_what_a_program_killed)
{
    sys_init();
    fresh();
    rln_read_line(console_rln_reader(), NULL, console_rln_kill());
    tasks();
    wire("abc\27");
    ASSERT_EQ(console_rln_kill()->len, 3);
    rln_stop();
    console_stop();
    ASSERT_EQ(console_rln_kill()->len, 0);
}

UTEST_MAIN();
