/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* term.c, the console's terminal, fed through the console and read through
 * its view. The console canvas holds the 80-column terminal, 30 rows. */

#include "core/com/com.h"
#include "core/com/manifold.h"
#include "core/com/rln.h"
#include "core/str/str.h"
#include "core/sys/sst.h"
#include "core/sys/sys.h"
#include "core/term/color.h"
#include "core/term/cells.h"
#include "tb_screen.h"
#include "core/vga/vga.h"
#include "core/vga/vga_emu.h"
#include "utest.h"
#include <string.h>

static char reply[256];

static void feed(const char *s)
{
    manifold_write_to(COM_TERM_SCREEN, s, strlen(s));
}

static void feedn(const char *s, size_t n)
{
    manifold_write_to(COM_TERM_SCREEN, s, n);
}

/* What the terminal answered since the last call, as a string. The console
 * holds each answer back until the one before it is read. */
static const char *answer(void)
{
    size_t n = 0, r;
    while ((r = com_read(COM_SOURCE_UART, reply + n, sizeof reply - 1 - n)) > 0)
        n += r;
    reply[n] = 0;
    return reply;
}

/* One query and its answer. */
static const char *ask(const char *q)
{
    feed(q);
    return answer();
}

/* A machine, and the terminal reset and drained. */
static void fresh(void)
{
    sys_init();
    feed("\30\33c");
    for (int i = 0; i < TB_SCREEN_SETTLE; i++)
        cells_task();
    answer();
}

static const mode0_cell_t *cell(int x, int y)
{
    for (int i = 0; i < TB_SCREEN_SETTLE; i++)
        cells_task();
    return tb_screen_row((uint8_t)y) + x;
}

static char ch(int x, int y)
{
    return (char)cell(x, y)->font_code;
}

/* The row's text with trailing spaces dropped. */
static const char *row(int y)
{
    static char text[96];
    tb_screen_t v;
    tb_screen(&v);
    int n = 0, last = 0;
    for (int x = 0; x < v.width; x++)
    {
        char c = ch(x, y);
        text[n++] = c;
        if (c != ' ')
            last = n;
    }
    text[last] = 0;
    return text;
}

static int cx(void)
{
    tb_screen_t v;
    tb_screen(&v);
    return v.cursor_x;
}

static int cy(void)
{
    tb_screen_t v;
    tb_screen(&v);
    return v.cursor_y;
}

static uint16_t rgb(unsigned r, unsigned g, unsigned b)
{
    return SCANVIDEO_ALPHA_MASK | SCANVIDEO_PIXEL_FROM_RGB8(r, g, b);
}

/* ---- controls ---- */

UTEST(term, form_feed_and_vertical_tab_are_line_feeds)
{
    fresh();
    feed("AB\fC\vD");
    ASSERT_STREQ(row(0), "AB");
    ASSERT_STREQ(row(1), "  C");
    ASSERT_STREQ(row(2), "   D");
}

UTEST(term, c0_without_an_action_is_drawn)
{
    fresh();
    feed("\x01\x05\x10\x19\x1c\x1f|");
    ASSERT_EQ(ch(0, 0), '\x01');
    ASSERT_EQ(ch(1, 0), '\x05');
    ASSERT_EQ(ch(2, 0), '\x10');
    ASSERT_EQ(ch(3, 0), '\x19');
    ASSERT_EQ(ch(4, 0), '\x1c');
    ASSERT_EQ(ch(5, 0), '\x1f');
    ASSERT_EQ(ch(6, 0), '|');
    ASSERT_STREQ(answer(), ""); /* ENQ has no answerback */
}

UTEST(term, sub_and_del_are_silent)
{
    fresh();
    feed("a\x1a\x7f" "b\x07" "c");
    ASSERT_STREQ(row(0), "abc");
}

UTEST(term, sgr_11_draws_the_controls_and_sgr_10_stops)
{
    fresh();
    feed("\33[11m\x07\x09\x7f\x1a\x18\33[10m\x07" "x");
    ASSERT_EQ(ch(0, 0), '\x07');
    ASSERT_EQ(ch(1, 0), '\x09');
    ASSERT_EQ(ch(2, 0), '\x7f');
    ASSERT_EQ(ch(3, 0), '\x1a');
    ASSERT_EQ(ch(4, 0), '\x18');
    ASSERT_EQ(ch(5, 0), 'x');
}

UTEST(term, sgr_11_still_runs_the_line_controls)
{
    fresh();
    feed("\33[11mab\r\nc\bd\33[0m\x07");
    ASSERT_STREQ(row(0), "ab");
    ASSERT_STREQ(row(1), "d\x07"); /* SGR 0 leaves SGR 11 */
}

UTEST(term, crm_is_sgr_11)
{
    fresh();
    feed("\33[3h\x07\33[3l\x07" "z");
    ASSERT_STREQ(row(0), "\x07z");
}

/* ---- the parser ---- */

UTEST(term, controls_run_inside_a_sequence)
{
    fresh();
    feed("abcde\33[2\bC" "X");
    ASSERT_STREQ(row(0), "abcde X");
    feed("\r\n\33[1\r\n;5HY");
    ASSERT_EQ(ch(4, 0), 'Y');
}

UTEST(term, can_and_sub_abort_a_sequence)
{
    fresh();
    feed("\33[1\x18m\33[\x1aq");
    ASSERT_STREQ(row(0), "mq");
}

UTEST(term, esc_starts_over)
{
    fresh();
    feed("\33\33[1mB\33]0;title\33[4mC");
    ASSERT_TRUE(cell(0, 0)->font_code == 'B');
    ASSERT_STREQ(row(0), "BC");
    ASSERT_TRUE(cell(1, 0)->attributes & TERM_ATTR_UNDERLINE);
}

UTEST(term, esc_intermediates_print_nothing)
{
    fresh();
    feed("\33%G\33 F\33*0\33+B\33(%5\33#3x");
    ASSERT_STREQ(row(0), "x");
}

UTEST(term, csi_intermediates_and_bad_orders_print_nothing)
{
    fresh();
    feed("\33[1\"q\33[#{\33[62;1\"p\33[ 2q\33[!5p\33[?25 l\33[25?l\33[?>c\33[1:2H.");
    ASSERT_STREQ(row(0), ".");
    ASSERT_STREQ(answer(), "");
    tb_screen_t v;
    tb_screen(&v);
    ASSERT_TRUE(v.cursor_enabled);
}

UTEST(term, numbers_stop_at_65535)
{
    fresh();
    feed("\33[65567Cx");
    ASSERT_EQ(ch(79, 0), 'x');
}

UTEST(term, separators_past_sixteen_are_dropped)
{
    fresh();
    char buf[400];
    memset(buf, ';', sizeof buf);
    buf[0] = '\33';
    buf[1] = '[';
    memcpy(&buf[sizeof buf - 4], "31m", 3);
    buf[sizeof buf - 1] = 'x';
    feedn(buf, sizeof buf);
    ASSERT_EQ(ch(0, 0), 'x');
    ASSERT_EQ(cell(0, 0)->fg_color, color_256[7]);
}

UTEST(term, strings_are_ignored_to_st)
{
    fresh();
    feed("\33Pq#0;1\33\\A\33_apc\33\\B\33^pm\33\\C\33Xsos\33\\D\33Pabc\x18" "E");
    ASSERT_STREQ(row(0), "ABCDE");
}

UTEST(term, single_shifts_let_the_next_byte_print)
{
    fresh();
    feed("\33Nx\33Oy");
    ASSERT_STREQ(row(0), "xy");
}

UTEST(term, high_bytes_end_a_sequence)
{
    fresh();
    feed("\33[1\xc3" "a");
    ASSERT_STREQ(row(0), "a");
}

/* ---- the cursor and the wrap ---- */

UTEST(term, the_last_column_leaves_a_wrap_pending)
{
    fresh();
    for (int i = 0; i < 80; i++)
        feed("x");
    ASSERT_EQ(cx(), 80);
    ASSERT_EQ(cy(), 0);
    feed("\b");
    ASSERT_EQ(cx(), 78);
    feed("\33[999C");
    ASSERT_EQ(cx(), 79);
    feed("Y");
    ASSERT_EQ(cx(), 80);
    feed("Z");
    ASSERT_EQ(cy(), 1);
    ASSERT_EQ(ch(0, 1), 'Z');
    ASSERT_EQ(ch(79, 0), 'Y');
}

UTEST(term, tab_keeps_a_pending_wrap)
{
    fresh();
    for (int i = 0; i < 80; i++)
        feed("x");
    feed("\t*");
    ASSERT_EQ(ch(79, 0), 'x');
    ASSERT_EQ(ch(0, 1), '*');
}

UTEST(term, moves_end_a_pending_wrap)
{
    fresh();
    for (int i = 0; i < 80; i++)
        feed("x");
    feed("\33[K");
    ASSERT_EQ(cx(), 79);
    feed("A");
    ASSERT_EQ(ch(79, 0), 'A');
    ASSERT_EQ(cy(), 0);
}

UTEST(term, decsc_keeps_the_wrap_and_not_autowrap)
{
    fresh();
    for (int i = 0; i < 80; i++)
        feed("x");
    feed("\0337\r\33[?7l\0338");
    ASSERT_EQ(cx(), 80);
    feed("A");
    ASSERT_EQ(ch(79, 0), 'A'); /* autowrap stayed off */
    ASSERT_EQ(cy(), 0);
}

UTEST(term, decrc_without_a_save_is_the_defaults)
{
    fresh();
    feed("\33[5;5H\33[31;1m\33(0\0338q");
    ASSERT_EQ(cx(), 1);
    ASSERT_EQ(cy(), 0);
    ASSERT_EQ(ch(0, 0), 'q');
    ASSERT_EQ(cell(0, 0)->attributes & TERM_ATTR_DEC, 0);
    ASSERT_EQ(cell(0, 0)->fg_color, color_256[7]);
}

UTEST(term, scp_and_rcp_are_decsc)
{
    fresh();
    feed("\33[3;4H\33[32m\33[s\33[H\33[0m\33[uA");
    ASSERT_EQ(ch(3, 2), 'A');
    ASSERT_EQ(cell(3, 2)->fg_color, color_256[2]);
}

UTEST(term, screens_keep_their_own_saved_cursor)
{
    fresh();
    feed("\33[2;2H\0337\33[?1049h\33[9;9H\0337\33[H\0338A\33[?1049l\0338B");
    ASSERT_EQ(ch(1, 1), 'B');
}

UTEST(term, large_line_counts_are_clamped)
{
    fresh();
    feed("a\r\nb\r\nc\33[H\33[256M");
    ASSERT_STREQ(row(0), "");
    ASSERT_STREQ(row(2), "");
    feed("x\33[260Ly");
    ASSERT_STREQ(row(0), "y");
}

/* ---- modes ---- */

UTEST(term, insert_mode_pushes_the_row_right)
{
    fresh();
    feed("abc\r\33[4hXY\33[4lZ");
    ASSERT_STREQ(row(0), "XYZbc");
}

/* A VT52 has no insert mode: the flag waits for ANSI mode, as in xterm. */
UTEST(term, insert_mode_is_suspended_in_vt52_mode)
{
    fresh();
    feed("abc\r\33[4h\33[?2lX");
    ASSERT_STREQ(row(0), "Xbc");
    ASSERT_TRUE(cells_term.insert_mode);
    feed("\33<X");
    ASSERT_STREQ(row(0), "XXbc");
    feed("\33[3b");
    ASSERT_STREQ(row(0), "XXXXXbc");
    feed("\33[?2l\33<\33[4l");
}

UTEST(term, newline_mode_returns_the_carriage)
{
    fresh();
    feed("ab\33[20h\nc\33[20l\nd");
    ASSERT_STREQ(row(1), "c");
    ASSERT_STREQ(row(2), " d");
}

UTEST(term, alt_screen_1047_clears_on_leaving)
{
    fresh();
    feed("main\33[?1047hALT\33[?1047l\33[?47h");
    ASSERT_STREQ(row(0), "");
    feed("\33[?47l");
    ASSERT_STREQ(row(0), "main");
}

UTEST(term, cursor_blink_mode_shares_the_style)
{
    fresh();
    tb_screen_t v;
    feed("\33[?12l");
    tb_screen(&v);
    ASSERT_EQ(v.cursor_style, 2);
    ASSERT_TRUE(v.cursor_enabled);
    feed("\33[4 q\33[?12h");
    tb_screen(&v);
    ASSERT_EQ(v.cursor_style, 3);
    feed("\33[7 q");
    tb_screen(&v);
    ASSERT_EQ(v.cursor_style, 3);
}

UTEST(term, decstr_leaves_the_cursor_and_its_style)
{
    fresh();
    feed("\33[5 q\33[4h\33[?33h\33[3;3H\33[!pab");
    tb_screen_t v;
    tb_screen(&v);
    ASSERT_EQ(v.cursor_style, 5);
    ASSERT_STREQ(row(2), "  ab");
    feed("\33[1;5Hc"); /* IRM was reset */
    ASSERT_STREQ(row(0), "    c");
    feed("\33[?33$p");
    ASSERT_STREQ(answer(), "\33[?33;1$y");
}

UTEST(term, ris_resets_the_world)
{
    fresh();
    feed("\33[11m\33[4h\33[20h\33[?33h\33[3 q\33]4;1;#123456\7\33]10;#010203\7\33c");
    tb_screen_t v;
    tb_screen(&v);
    ASSERT_EQ(v.cursor_style, 0);
    ASSERT_STREQ(ask("\33[3$p"), "\33[3;2$y");
    ASSERT_STREQ(ask("\33[4$p"), "\33[4;2$y");
    ASSERT_STREQ(ask("\33[20$p"), "\33[20;2$y");
    ASSERT_STREQ(ask("\33[?33$p"), "\33[?33;2$y");
    feed("\33[31mA\33[39mB");
    ASSERT_EQ(cell(0, 0)->fg_color, color_256[1]);
    ASSERT_EQ(cell(1, 0)->fg_color, color_256[7]);
}

UTEST(term, the_system_soft_reset_keeps_the_screen)
{
    fresh();
    feed("hello\33[?1049hALT\33]4;1;#123456\7\33[4h");
    cells_RIS_no_clear();
    ASSERT_STREQ(row(0), "hello");
    ASSERT_EQ(cx(), 5);
    feed("\33[31mX\33[4$p");
    ASSERT_STREQ(answer(), "\33[4;2$y");
    ASSERT_EQ(cell(5, 0)->fg_color, color_256[1]);
}

UTEST(term, the_reset_strings_leave_vt52_mode)
{
    fresh();
    feed("\33[?2l");
    feed(STR_TERM_SOFT_RESET);
    feed("\33[31mA");
    ASSERT_STREQ(row(0), "A");
    ASSERT_EQ(cell(0, 0)->fg_color, color_256[1]);
    feed("\33[?2l");
    feed(STR_TERM_HARD_RESET);
    ASSERT_STREQ(row(0), "");
}

UTEST(term, the_soft_reset_string_puts_the_keys_back)
{
    fresh();
    feed("\33[?1h\33=\33[?8l");
    ASSERT_TRUE(cells_term.app_cursor);
    ASSERT_TRUE(cells_term.app_keypad);
    ASSERT_FALSE(cells_term.autorepeat);
    feed(STR_TERM_SOFT_RESET);
    ASSERT_FALSE(cells_term.app_cursor);
    ASSERT_FALSE(cells_term.app_keypad);
    ASSERT_TRUE(cells_term.autorepeat);
}

/* Each item of an OSC 4 takes effect as it ends, so the CAN keeps entry 1's
 * green and drops entry 2's blue, and an ESC after a finished item keeps
 * that item. X follows entry 1 to its blue. */
UTEST(term, an_aborted_osc_keeps_the_pen_true)
{
    fresh();
    feed("\33[31m\33]4;1;#00ff00;2;#0000ff\30X");
    ASSERT_EQ(cell(0, 0)->fg_color, rgb(0, 255, 0));
    feed("\33]4;1;#0000ff;\33[0mY\33[31mZ");
    ASSERT_EQ(cell(0, 0)->fg_color, rgb(0, 0, 255));
    ASSERT_EQ(cell(1, 0)->fg_color, color_256[7]);
    ASSERT_EQ(cell(2, 0)->fg_color, rgb(0, 0, 255));
}

UTEST(term, alt_1049_off_restores_on_the_primary_too)
{
    fresh();
    feed("\33[3;3H\0337\33[10;10H\33[?1049lA");
    ASSERT_EQ(ch(2, 2), 'A');
}

UTEST(term, ed_3_forgets_the_scrollback)
{
    fresh();
    cells_set_height(80, 29);
    feed("one\r\n");
    for (int i = 0; i < 28; i++)
        feed("\r\n");
    cells_set_height(80, 30);
    ASSERT_STREQ(row(0), "one");

    fresh();
    cells_set_height(80, 29);
    feed("one\r\n");
    for (int i = 0; i < 28; i++)
        feed("\r\n");
    feed("\33[3J");
    cells_set_height(80, 30);
    ASSERT_STREQ(row(0), "");
    cells_set_height(80, 30);
}

/* ---- replies ---- */

UTEST(term, device_attributes)
{
    fresh();
    ASSERT_STREQ(ask("\33[c"), "\33[?6c");
    ASSERT_STREQ(ask("\33[0c"), "\33[?6c");
    ASSERT_STREQ(ask("\33[1c"), "");
    ASSERT_STREQ(ask("\33Z"), "\33[?6c");
    ASSERT_STREQ(ask("\33[>c"), "\33[>0;1;0c");
    ASSERT_STREQ(ask("\33[>1c"), "");
    ASSERT_STREQ(ask("\33[=c"), "\33P!|00000000\33\\");
}

UTEST(term, decreqtparm)
{
    fresh();
    ASSERT_STREQ(ask("\33[x"), "\33[2;1;1;112;112;1;0x");
    ASSERT_STREQ(ask("\33[1x"), "\33[3;1;1;112;112;1;0x");
    ASSERT_STREQ(ask("\33[2x"), "");
}

UTEST(term, window_reports)
{
    fresh();
    ASSERT_STREQ(ask("\33[18t"), "\33[8;30;80t");
    ASSERT_STREQ(ask("\33[19t"), "\33[9;30;80t");
    ASSERT_STREQ(ask("\33[14t"), "\33[4;480;640t");
    ASSERT_STREQ(ask("\33[16t"), "\33[6;16;8t");
    ASSERT_STREQ(ask("\33[11t"), "\33[1t");
    ASSERT_STREQ(ask("\33[13t\33[21t\33[22;0t"), "");
}

UTEST(term, decrqm)
{
    fresh();
    ASSERT_STREQ(ask("\33[?3$p"), "\33[?3;4$y");
    ASSERT_STREQ(ask("\33[?7$p"), "\33[?7;1$y");
    ASSERT_STREQ(ask("\33[?12$p"), "\33[?12;1$y");
    ASSERT_STREQ(ask("\33[?1048$p"), "\33[?1048;2$y");
    ASSERT_STREQ(ask("\0337\33[?1048$p"), "\33[?1048;1$y");
    ASSERT_STREQ(ask("\33[2$p"), "\33[2;4$y");
    ASSERT_STREQ(ask("\33[12$p"), "\33[12;3$y");
    ASSERT_STREQ(ask("\33[?4000$p"), "\33[?4000;0$y");
}

/* ---- the keyboard's modes: the terminal holds them, the keyboard reads --- */

UTEST(term, the_keyboard_modes_set_reset_and_report)
{
    fresh();
    ASSERT_STREQ(ask("\33[?1$p\33[?8$p\33[?66$p"),
                 "\33[?1;2$y\33[?8;1$y\33[?66;2$y");
    ASSERT_FALSE(cells_term.app_cursor);
    ASSERT_TRUE(cells_term.autorepeat);
    ASSERT_FALSE(cells_term.app_keypad);
    feed("\33[?1;66h\33[?8l");
    ASSERT_STREQ(ask("\33[?1$p\33[?8$p\33[?66$p"),
                 "\33[?1;1$y\33[?8;2$y\33[?66;1$y");
    ASSERT_TRUE(cells_term.app_cursor);
    ASSERT_FALSE(cells_term.autorepeat);
    ASSERT_TRUE(cells_term.app_keypad);
    feed("\33[?1;66l\33[?8h");
    ASSERT_STREQ(ask("\33[?1$p\33[?8$p\33[?66$p"),
                 "\33[?1;2$y\33[?8;1$y\33[?66;2$y");
}

UTEST(term, the_keypad_mode_escapes_work_in_both_modes)
{
    fresh();
    feed("\33=");
    ASSERT_STREQ(ask("\33[?66$p"), "\33[?66;1$y");
    feed("\33>");
    ASSERT_STREQ(ask("\33[?66$p"), "\33[?66;2$y");
    feed("\33[?2l\33=");
    ASSERT_TRUE(cells_term.app_keypad);
    feed("\33>");
    ASSERT_FALSE(cells_term.app_keypad);
    feed("\33=\33<x");
    ASSERT_TRUE(cells_term.app_keypad); /* leaving VT52 mode keeps it */
    ASSERT_STREQ(row(0), "x");
}

/* DECSTR puts back the cursor keys and the keypad; only a full reset, RIS
 * or the system's, also turns autorepeat on. */
UTEST(term, the_resets_and_the_keyboard_modes)
{
    fresh();
    feed("\33[?1;66h\33[?8l\33[!p");
    ASSERT_FALSE(cells_term.app_cursor);
    ASSERT_FALSE(cells_term.app_keypad);
    ASSERT_FALSE(cells_term.autorepeat);
    feed("\33[?1h\33=\33c");
    ASSERT_FALSE(cells_term.app_cursor);
    ASSERT_FALSE(cells_term.app_keypad);
    ASSERT_TRUE(cells_term.autorepeat);
    feed("\33[?1h\33=\33[?8l");
    cells_RIS_no_clear();
    ASSERT_FALSE(cells_term.app_cursor);
    ASSERT_FALSE(cells_term.app_keypad);
    ASSERT_TRUE(cells_term.autorepeat);
}

/* A saved cursor and the alternate screen carry none of them. */
UTEST(term, the_keyboard_modes_belong_to_no_screen)
{
    fresh();
    feed("\0337\33[?1h\33=\33[?8l\0338\33[?1049h");
    ASSERT_TRUE(cells_term.app_cursor);
    ASSERT_TRUE(cells_term.app_keypad);
    ASSERT_FALSE(cells_term.autorepeat);
    feed("\33[?1l\33[?1049l");
    ASSERT_FALSE(cells_term.app_cursor);
    ASSERT_TRUE(cells_term.app_keypad);
}

UTEST(term, a_savestate_carries_the_keyboard_modes)
{
    static uint8_t blob[CELLS_SST_SIZE];
    fresh();
    feed("\33[?1h\33=\33[?8l");
    sst_cursor_t c = {blob, blob + sizeof blob, false};
    cells_sst_save(&c, 0);
    ASSERT_TRUE(sst_ok(&c));
    feed("\33c");
    c = (sst_cursor_t){blob, blob + sizeof blob, false};
    ASSERT_TRUE(cells_sst_load(&c, 0));
    ASSERT_TRUE(sst_ok(&c));
    ASSERT_TRUE(cells_term.app_cursor);
    ASSERT_TRUE(cells_term.app_keypad);
    ASSERT_FALSE(cells_term.autorepeat);
    feed("\33[?1l\33[?8h");
    c = (sst_cursor_t){blob, blob + sizeof blob, false};
    cells_sst_save(&c, 0);
    feed("\33[?1h\33[?8l");
    c = (sst_cursor_t){blob, blob + sizeof blob, false};
    ASSERT_TRUE(cells_sst_load(&c, 0));
    ASSERT_FALSE(cells_term.app_cursor);
    ASSERT_TRUE(cells_term.app_keypad);
    ASSERT_TRUE(cells_term.autorepeat);
}

UTEST(term, rep_repeats_the_last_glyph)
{
    fresh();
    feed("a\33[3b\33[2b\r\n\33[2b");
    ASSERT_STREQ(row(0), "aaaa"); /* the second REP follows a control */
    ASSERT_STREQ(row(1), "");
}

/* A long REP ends as the whole count would: the screen full of the glyph,
 * with the cursor where the count leaves it. */
UTEST(term, a_long_rep_ends_where_the_whole_count_would)
{
    fresh();
    feed("\33[3;7Hz\33[65535b");
    ASSERT_EQ(cy(), 29);
    ASSERT_EQ(cx(), (6 + 65536) % 80);
    char full[81];
    memset(full, 'z', 80);
    full[80] = 0;
    ASSERT_STREQ(row(0), full);
}

/* REP after a string or a bad sequence repeats nothing. */
UTEST(term, rep_needs_a_glyph_just_before)
{
    fresh();
    feed("a\33]0;t\7\33[3b\33[1 2b\33[3bb");
    ASSERT_STREQ(row(0), "ab");
}

UTEST(term, dectst_power_up_is_a_reset)
{
    fresh();
    feed("abc\33[2;1y");
    ASSERT_STREQ(row(0), "");
    ASSERT_EQ(cx(), 0);
}

/* ---- colors ---- */

UTEST(term, erase_takes_the_background_and_no_attributes)
{
    fresh();
    feed("\33[4;7;5;41;32mx\33[2K");
    const mode0_cell_t *c = cell(0, 0);
    ASSERT_EQ(c->font_code, ' ');
    ASSERT_EQ(c->attributes, 0);
    ASSERT_EQ(c->bg_color, color_256[1]);
    ASSERT_EQ(c->fg_color, color_256[2]);
}

UTEST(term, defaults_are_not_palette_entries)
{
    fresh();
    feed("\33]4;7;#ff0000\7\33]4;0;#00ff00\7\33[37;40mA\33[39;49mB");
    ASSERT_EQ(cell(0, 0)->fg_color, rgb(255, 0, 0));
    ASSERT_EQ(cell(0, 0)->bg_color, rgb(0, 255, 0));
    ASSERT_EQ(cell(1, 0)->fg_color, color_256[7]);
    ASSERT_EQ(cell(1, 0)->bg_color, color_256[0]);
}

/* A palette entry reaches the pen, and the cells drawn with it follow it. */
UTEST(term, the_palette_reaches_the_pen)
{
    fresh();
    feed("\33[31m\33]4;1;rgb:00/80/ff\7A");
    ASSERT_EQ(cell(0, 0)->fg_color, rgb(0, 0x80, 0xff));
    feed("\33]104\7B");
    ASSERT_EQ(cell(0, 0)->fg_color, color_256[1]);
    ASSERT_EQ(cell(1, 0)->fg_color, color_256[1]);
}

UTEST(term, osc_takes_many_items)
{
    fresh();
    feed("\33]4;1;#010203;2;#040506\33\\\33[31mA\33[32mB");
    ASSERT_EQ(cell(0, 0)->fg_color, rgb(1, 2, 3));
    ASSERT_EQ(cell(1, 0)->fg_color, rgb(4, 5, 6));
    feed("\33]104;1\7\33[31mC\33[32mD");
    ASSERT_EQ(cell(2, 0)->fg_color, color_256[1]);
    ASSERT_EQ(cell(3, 0)->fg_color, rgb(4, 5, 6));
    feed("\33]10;#102030;#405060\7\33[0mE");
    ASSERT_EQ(cell(4, 0)->fg_color, rgb(0x10, 0x20, 0x30));
    ASSERT_EQ(cell(4, 0)->bg_color, rgb(0x40, 0x50, 0x60));
    feed("\33]110\7\33]111\7F");
    ASSERT_EQ(cell(5, 0)->fg_color, color_256[7]);
    ASSERT_EQ(cell(5, 0)->bg_color, color_256[0]);
}

UTEST(term, osc_ignores_line_feeds_inside)
{
    fresh();
    feed("\33]4;1;#0\n10203\7\33[31mA");
    ASSERT_STREQ(row(0), "A");
    ASSERT_EQ(cell(0, 0)->fg_color, rgb(1, 2, 3));
}

UTEST(term, linux_palette)
{
    fresh();
    feed("\33[31m\33]P1102030A");
    ASSERT_EQ(cell(0, 0)->fg_color, rgb(0x10, 0x20, 0x30));
    feed("\33]RB");
    ASSERT_EQ(cell(0, 0)->fg_color, color_256[1]);
    ASSERT_EQ(cell(1, 0)->fg_color, color_256[1]);
}

UTEST(term, ice_colors_survive_osc_11)
{
    fresh();
    feed("\33[?33h\33[5;44m\33]11;#102030\7A\33[49mB");
    ASSERT_EQ(cell(0, 0)->bg_color, color_256[12]);
    ASSERT_EQ(cell(1, 0)->bg_color, color_256[8]);
}

UTEST(term, extended_colors_check_their_numbers)
{
    fresh();
    feed("\33[32m\33[38;5;999mA\33[38;2;300;0;0mB\33[38;5mC");
    ASSERT_EQ(cell(0, 0)->fg_color, color_256[2]);
    ASSERT_EQ(cell(1, 0)->fg_color, color_256[2]);
    ASSERT_EQ(cell(2, 0)->fg_color, color_256[2]);
    ASSERT_EQ(cell(2, 0)->attributes & TERM_ATTR_ANY_BLINK, 0);
    feed("\33[38:2::1:2:3mD\33[38:2:4:5:6mE\33[48:5:3mF");
    ASSERT_EQ(cell(3, 0)->fg_color, rgb(1, 2, 3));
    ASSERT_EQ(cell(4, 0)->fg_color, rgb(4, 5, 6));
    ASSERT_EQ(cell(5, 0)->bg_color, color_256[3]);
}

UTEST(term, bold_brightens_from_the_base)
{
    fresh();
    feed("\33[1;31mA\33[22mB\33[1;39mC");
    ASSERT_EQ(cell(0, 0)->fg_color, color_256[9]);
    ASSERT_EQ(cell(1, 0)->fg_color, color_256[1]);
    ASSERT_EQ(cell(2, 0)->fg_color, color_256[15]);
}

UTEST(term, underline_styles)
{
    fresh();
    feed("\33[4:3mA\33[4:2mB\33[4:0mC\33[1:5mD");
    ASSERT_EQ(cell(0, 0)->attributes, TERM_ATTR_UNDERLINE);
    ASSERT_EQ(cell(1, 0)->attributes, TERM_ATTR_DBL_UL);
    ASSERT_EQ(cell(2, 0)->attributes, 0);
    ASSERT_EQ(cell(3, 0)->attributes, 0); /* 1:5 is bold, not blink */
    ASSERT_EQ(cell(3, 0)->fg_color, color_256[15]);
}

/* ---- character sets ---- */

UTEST(term, the_uk_set_has_the_pound)
{
    fresh();
    feed("\33(A#a\33(B#\33(U#\33)0\16q\17");
    ASSERT_EQ(cell(0, 0)->font_code, 0x7D);
    ASSERT_TRUE(cell(0, 0)->attributes & TERM_ATTR_DEC);
    ASSERT_EQ(ch(1, 0), 'a');
    ASSERT_EQ(ch(2, 0), '#');
    ASSERT_EQ(cell(3, 0)->attributes, 0);
    ASSERT_TRUE(cell(4, 0)->attributes & TERM_ATTR_DEC);
}

/* ---- VT52 ---- */

UTEST(term, vt52_mode)
{
    fresh();
    feed("\33[?2l\33Y%*Z\33Z\33[\33Fq\33Gq\33<\33[2Cx");
    ASSERT_EQ(ch(10, 5), 'Z');
    ASSERT_STREQ(answer(), "\33/Z");
    ASSERT_TRUE(cell(11, 5)->attributes & TERM_ATTR_DEC);
    ASSERT_EQ(cell(12, 5)->attributes, 0);
    ASSERT_EQ(ch(15, 5), 'x');
}

/* ---- line sizes, DECALN, SL/SR and the reverse screen ---- */

UTEST(term, decaln_fills_the_screen_with_e)
{
    fresh();
    feed("\33[5;10r\33[3;3H\33[31mx\33#8");
    ASSERT_EQ(cx(), 0);
    ASSERT_EQ(cy(), 0);
    char full[81];
    memset(full, 'E', 80);
    full[80] = 0;
    ASSERT_STREQ(row(0), full);
    ASSERT_STREQ(row(29), full);
    ASSERT_EQ(cell(2, 2)->fg_color, color_256[7]);
    feed("\33[30;1H\n"); /* the margins reset, so this scrolls */
    ASSERT_STREQ(row(29), "");
}

UTEST(term, a_double_width_line_has_forty_columns)
{
    fresh();
    for (int i = 0; i < 80; i++)
        feed(i < 40 ? "a" : "b");
    feed("\33#6");
    ASSERT_EQ(tb_screen_row_size(0), TERM_LINE_WIDE);
    ASSERT_EQ(ch(39, 0), 'a');
    ASSERT_EQ(ch(40, 0), ' '); /* the right half is lost */
    ASSERT_EQ(cx(), 39);
    feed("\33[1;70Hz");
    ASSERT_EQ(ch(39, 0), 'z');
    ASSERT_EQ(cx(), 40); /* a wrap is pending at the row's own edge */
    feed("y");
    ASSERT_EQ(ch(0, 1), 'y');
    feed("\33#5\33[1;1H\33#5");
    ASSERT_EQ(tb_screen_row_size(0), TERM_LINE_NORMAL);
    feed("\33[1;60Hq");
    ASSERT_EQ(ch(59, 0), 'q');
}

UTEST(term, line_sizes_move_with_their_lines)
{
    fresh();
    feed("\33#3top\r\n\33#4top\r\n");
    ASSERT_EQ(tb_screen_row_size(0), TERM_LINE_TOP);
    ASSERT_EQ(tb_screen_row_size(1), TERM_LINE_BOTTOM);
    feed("\33[H\33M"); /* a line comes in at the top */
    ASSERT_EQ(tb_screen_row_size(0), TERM_LINE_NORMAL);
    ASSERT_EQ(tb_screen_row_size(1), TERM_LINE_TOP);
    ASSERT_EQ(tb_screen_row_size(2), TERM_LINE_BOTTOM);
    feed("\33[2J");
    ASSERT_EQ(tb_screen_row_size(1), TERM_LINE_NORMAL);
}

UTEST(term, a_scroll_keeps_the_cursor_on_its_line)
{
    fresh();
    feed("\33[2H\33#6\33[1;61H\33[S"); /* the double-width line comes up */
    ASSERT_EQ(tb_screen_row_size(0), TERM_LINE_WIDE);
    ASSERT_EQ(cx(), 39);
    ASSERT_STREQ(ask("\33[6n"), "\33[1;40R");
    feed("\33[P\33[100X");
}

UTEST(term, a_quiet_terminal_answers_nothing)
{
    fresh();
    com_set_reply_quiet(true);
    ASSERT_STREQ(ask("\33[6n"), "");
    com_set_reply_quiet(false);
    ASSERT_STREQ(ask("\33[6n"), "\33[1;1R");
}

/* Only a mode 0 on a 320-wide canvas sizes the 40-column terminal, so the
 * system's soft reset puts it back, as the RIA's copy does. */
UTEST(term, the_soft_reset_sets_the_40_back_to_30)
{
    fresh();
    cells_set_height(40, 22);
    ASSERT_EQ(cells_term.grids[0]->height, 22);
    cells_RIS_no_clear();
    ASSERT_EQ(cells_term.grids[0]->height, TERM_STD_HEIGHT);
}

/* A program's own size is a third grid: the answers come from it, and the
 * screen's 80 columns go on as they were. */
UTEST(term, a_pinned_width_answers_from_its_own_grid)
{
    fresh();
    rln_set_term_width(132);
    ASSERT_EQ(cells_term.grid_count, 3);
    ASSERT_STREQ(ask("\33[18t"), "\33[8;30;132t");
    feed("\33[1;100H");
    ASSERT_STREQ(ask("\33[6n"), "\33[1;100R");
    ASSERT_EQ(cx(), 79); /* the screen's terminal stops at its edge */
    rln_set_term_width(0);
    ASSERT_EQ(cells_term.grid_count, 2);
    ASSERT_STREQ(ask("\33[6n"), "\33[1;80R");
}

/* Pinning one axis fixes the other at the screen's; the tab stops go on
 * past the old width; the end of a program drops the grid. */
UTEST(term, a_pinned_height_fixes_the_width_and_stops_with_the_program)
{
    fresh();
    rln_set_term_height(20);
    ASSERT_STREQ(ask("\33[18t"), "\33[8;20;80t");
    rln_set_term_width(132);
    feed("\33[1;81H\t");
    ASSERT_STREQ(ask("\33[6n"), "\33[1;89R");
    rln_stop();
    ASSERT_EQ(cells_term.grid_count, TERM_SCREEN_GRIDS);
}

UTEST(term, scroll_left_and_right)
{
    fresh();
    feed("abcdef\r\n123456\33[2;30r\33[2 @");
    ASSERT_STREQ(row(0), "abcdef"); /* outside the region */
    ASSERT_STREQ(row(1), "3456");
    feed("\33[3 A");
    ASSERT_STREQ(row(1), "   3456");
}

UTEST(term, the_reverse_screen_is_a_mode)
{
    fresh();
    tb_screen_t v;
    feed("\33[?5h");
    tb_screen(&v);
    ASSERT_TRUE(v.reverse);
    ASSERT_STREQ(ask("\33[?5$p"), "\33[?5;1$y");
    feed("\33c");
    tb_screen(&v);
    ASSERT_FALSE(v.reverse);
}

/* ---- recolor ------------------------------------------------------------ */

/* The default colors are their own sources: OSC 10 and 11 recolor the text
 * drawn in them, and bold's 15 follows entry 15, not the default. */
UTEST(term, the_defaults_recolor_what_was_drawn_in_them)
{
    fresh();
    feed("A\33[1mB");
    feed("\33]10;#102030;#405060\7");
    ASSERT_EQ(cell(0, 0)->fg_color, rgb(0x10, 0x20, 0x30));
    ASSERT_EQ(cell(0, 0)->bg_color, rgb(0x40, 0x50, 0x60));
    ASSERT_EQ(cell(1, 0)->fg_color, color_256[15]);
    ASSERT_EQ(cell(1, 0)->bg_color, rgb(0x40, 0x50, 0x60));
    feed("\33]4;15;#0000ff\7");
    ASSERT_EQ(cell(1, 0)->fg_color, rgb(0, 0, 255));
    feed("\33]110\7\33]111\7");
    ASSERT_EQ(cell(0, 0)->fg_color, color_256[7]);
    ASSERT_EQ(cell(0, 0)->bg_color, color_256[0]);
}

/* A cell follows the entries it was drawn from through SGR 7, SGR 8 and
 * faint, its underline follows its foreground unless SGR 58 gave it one, and
 * an RGB color, or one made transparent, follows nothing. */
UTEST(term, a_recolor_follows_each_cells_sources)
{
    fresh();
    feed("\33[7;31;42mR\33[0m\33[8;31;42mC\33[0m\33[2;31mF\33[0m"
         "\33[4;31mU\33[58;5;3mV\33[0m\33[38;2;1;2;3mX\33[31;38;1mT");
    feed("\33]4;1;#ff0000\7\33]4;2;#00ff00\7\33]4;3;#0000ff\7");
    ASSERT_EQ(cell(0, 0)->fg_color, rgb(0, 255, 0));
    ASSERT_EQ(cell(0, 0)->bg_color, rgb(255, 0, 0));
    ASSERT_EQ(cell(1, 0)->fg_color, rgb(0, 255, 0));
    ASSERT_EQ(cell(1, 0)->bg_color, rgb(0, 255, 0));
    ASSERT_EQ(cell(2, 0)->fg_color, SCANVIDEO_ALPHA_MASK | SCANVIDEO_PIXEL_FROM_RGB5(15, 0, 0));
    ASSERT_EQ(cell(3, 0)->ul_color, rgb(255, 0, 0));
    ASSERT_EQ(cell(4, 0)->ul_color, color_256[3]);
    ASSERT_EQ(cell(5, 0)->fg_color, rgb(1, 2, 3));
    ASSERT_FALSE(cell(6, 0)->fg_color & SCANVIDEO_ALPHA_MASK);
}

/* A lazy erase still owed takes a recolor too. */
UTEST(term, an_owed_erase_takes_a_recolor)
{
    fresh();
    feed("\33[44m\33[2J\33]4;4;#123456\7");
    ASSERT_EQ(cell(10, 10)->bg_color, rgb(0x12, 0x34, 0x56));
}

/* ---- protection ---------------------------------------------------------- */

/* DECSCA protects from DECSED and DECSEL, and from nothing else. */
UTEST(term, decsca_protects_from_the_selective_erases_alone)
{
    fresh();
    feed("\33[1\"qAB\33[0\"qCD\33[?2K");
    ASSERT_STREQ(row(0), "AB");
    feed("\33[2K");
    ASSERT_STREQ(row(0), "");
    feed("\33[1\"qAB\33[0\"qCD\33[2J");
    ASSERT_STREQ(row(0), "");
}

/* SPA and EPA guard cells from ED, EL and ECH as well, unless ERM is set. */
UTEST(term, spa_guards_from_every_erase_unless_erm)
{
    fresh();
    feed("\33VAB\33WCD\33[H\33[4X");
    ASSERT_STREQ(row(0), "AB");
    feed("\33[2K\33[J");
    ASSERT_STREQ(row(0), "AB");
    feed("\33[6h");
    ASSERT_STREQ(ask("\33[6$p"), "\33[6;1$y");
    feed("\33[2K");
    ASSERT_STREQ(row(0), "");
}

/* A selective erase owed to a row spares its protected cells when it lands,
 * and leaves the row's size. */
UTEST(term, a_selective_lazy_erase_spares_and_keeps_the_size)
{
    fresh();
    feed("\33#6\33[1\"qA\33[0\"qB\r\nC\33[?2J");
    ASSERT_STREQ(row(0), "A");
    ASSERT_STREQ(row(1), "");
    ASSERT_EQ(tb_screen_row_size(0), TERM_LINE_WIDE);
    feed("\33[2J");
    ASSERT_STREQ(row(0), "");
    ASSERT_EQ(tb_screen_row_size(0), TERM_LINE_NORMAL);
}

/* SR moves the protected cells of a row still owed a selective erase. */
UTEST(term, a_shift_moves_what_a_selective_erase_spares)
{
    fresh();
    feed("\33[6;1H\33[1\"qAB\33[0\"qCD\33[H\33[?2J\33[3 A");
    ASSERT_STREQ(row(5), "   AB");
}

/* Printing and DCH ignore protection, and DECSTR ends it. */
UTEST(term, protection_ends_with_a_reset)
{
    fresh();
    feed("\33[1\"qABC\33[H\33[P");
    ASSERT_STREQ(row(0), "BC");
    feed("x\33[?2K");
    ASSERT_STREQ(row(0), "xC");
    feed("\33[!p\33[?2K");
    ASSERT_STREQ(row(0), "");
}

/* DECSC saves the pen's protection and DECRC brings it back. */
UTEST(term, decsc_saves_the_protection)
{
    fresh();
    feed("\33[1\"q\0337\33[0\"qAA\0338\33[2CB\33[?2K");
    ASSERT_STREQ(row(0), "  B");
}

UTEST_MAIN();
