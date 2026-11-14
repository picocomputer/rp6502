/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/api/api.h"
#include "core/api/xreg.h"
#include "core/com/com.h"
#include "core/com/manifold.h"
#include "core/com/tio.h"
#include "core/ria/regs.h"
#include "core/com/rln.h"
#include "core/term/cells.h"
#include "core/sys/timer.h"
#include "core/vga/vga_emu.h"
#include "utest.h"
#include <string.h>

static int lines;
static char last[256];

static void got_line(bool timeout, const char *buf)
{
    (void)timeout;
    lines++;
    strncpy(last, buf, sizeof last - 1);
    last[sizeof last - 1] = 0;
    rln_read_line(got_line);
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

/* rln_init keeps each source's line_end, so every source is also cleared
 * with rln_forget_source, or a case would start with what the previous case
 * left. tio starts over too. The console's terminal starts over with its
 * cursor at the top left, and the line editor reads its prompt column and
 * size from it. */
static void fresh(void)
{
    cells_init();
    manifold_set_reply_default(true);
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
    rln_read_line(got_line);
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

/* The console's terminal's cursor. */
static const term_pos_t *pos(void)
{
    return term_pos(&cells_term, term_shown(&cells_term));
}

/* The monitor and std.c start the next read after the callback returns. */
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
        rln_read_line(got_line_later);
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

UTEST(rln, a_read_start_sends_the_modes_and_asks_nothing)
{
    fresh();
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
        com_putchar('p');
    capture();
    reading();
    manifold_set_tx_tap(NULL);
    ASSERT_STREQ(shown, "\33[4l\r\n");
    const term_pos_t *p = term_pos(&cells_term, term_shown(&cells_term));
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
    ASSERT_STREQ(shown, "\r\n\33[4labc");
    ASSERT_EQ(term_shown(&cells_term)->width, 40);
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
    ASSERT_FALSE(cells_term.autowrap);
    ASSERT_EQ(cells_view_row(0)[79].font_code, 'a');
    ASSERT_EQ(cells_view_row(1)[0].font_code, 'b');
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
    ASSERT_EQ(cells_view_row(0)[0].font_code, ' ');
    ASSERT_EQ(cells_view_row(1)[0].font_code, ' ');
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
    ASSERT_EQ(cells_view_row(1)[79].font_code, 'z');
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
    com_flush();
    ASSERT_EQ(pos()->x, 0);
    ASSERT_EQ(pos()->y, 1);
    com_printf("]");
    reading();
    type_n(79, 'b');
    rln_stop();
    com_flush();
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
    com_flush();
    ASSERT_EQ(cells_view_row(0)[79].font_code, '^');
    ASSERT_EQ(cells_view_row(1)[0].font_code, 'C');
    ASSERT_EQ(cells_view_row(2)[0].font_code, ']');
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
    tio_set_term_width(40);
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
    tio_stop();
}

/* A pinned width of 20 holds 10 rows of 20, so a 230-character line is cut
 * to 200, and Home still climbs to its first row. */
UTEST(rln, a_new_width_cuts_the_line_to_what_it_holds)
{
    fresh();
    vt_start();
    reading();
    type_n(230, 'a');
    tio_set_term_width(20);
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
    tio_stop();
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
    tio_set_term_width(40);
    capture();
    reading();
    type_n(40, 'a');
    wire("b");
    manifold_set_tx_tap(NULL);
    ASSERT_FALSE(probed());
    ASSERT_TRUE(strstr(shown, "a\r\nb") != NULL);
    ASSERT_EQ(cells_view_row(1)[0].font_code, 'b');
    ASSERT_EQ(pos()->x, 1);
    ASSERT_EQ(pos()->y, 1);
    tio_stop();
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

static char screen[256];

/* What is sent to term.c is kept, and term.c still follows it. */
static void screen_out(const char *buf, int len)
{
    strncat(screen, buf, (size_t)len);
    for (int i = 0; i < len; i++)
        term_write(&cells_term, (uint8_t)buf[i]);
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
    cells_init();
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

/* A terminal on the UART may print a shape, so the shapes reach term.c alone. */
UTEST(rln, shapes_reach_the_screen_alone)
{
    screen_reading();
    ASSERT_TRUE(strstr(screen, "\33[5 q") != NULL);
    wire("\33[2~");
    ASSERT_TRUE(strstr(screen, "\33[1 q") != NULL);
    ASSERT_TRUE(strstr(shown, " q") == NULL);
    screen_done();
}

/* Plain lines lay nothing out: no prompt column, no shape, plain moves, and
 * the editing keys still edit. */
UTEST(rln, plain_lines_echo_and_edit_with_plain_moves)
{
    fresh();
    rln_set_plain(true);
    screen[0] = 0;
    manifold_set_term_out(screen_out);
    com_printf("> ");
    capture();
    reading();
    wire("abc\b\33[Dx\33[H");
    ASSERT_TRUE(strstr(screen, " q") == NULL);
    ASSERT_TRUE(strstr(shown, "\r") == NULL);
    ASSERT_TRUE(strstr(shown, "\33[2D") != NULL);
    wire("\r");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "axb");
    screen_done();
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
    ASSERT_EQ(strncmp(shown, "\33[4l", 4), 0);
    ASSERT_EQ(strspn(shown + 4, "a"), (size_t)100);
    ASSERT_EQ(shown_len, (size_t)104);
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
    manifold_keyboard_push("ab", 2);
    tasks();
    wire("\r");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "ab");
    ASSERT_EQ(line_src, COM_SOURCE_UART);
    rln_read_line_script(got_script_line, 1000);
    manifold_keyboard_push("cd\r", 3);
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

/* rxvt's Shift-Home and Shift-End. */
UTEST(rln, rxvt_shifted_keys_end_on_a_dollar)
{
    editing();
    wire("bc\33[7$a\33[8$d\r");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "abcd");
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

UTEST(rln, ss3_keys_move_type_and_end_the_line)
{
    editing();
    wire("ab\33OD\33ODx\33OC\33OC\33Oq\33Ok\33OX\33O5P\33OM");
    ASSERT_EQ(lines, 1);
    ASSERT_STREQ(last, "xab1+=");
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

UTEST_MAIN();
