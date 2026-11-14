/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* term.c, the console's terminal, fed through the console and read through
 * its view. The console canvas holds the 80-column terminal, 30 rows. */

#include "core/com/com.h"
#include "core/com/manifold.h"
#include "core/str/str.h"
#include "core/sys/sys.h"
#include "core/term/color.h"
#include "core/term/cells.h"
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
    while ((r = manifold_uart_read(reply + n, sizeof reply - 1 - n)) > 0)
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
    for (int i = 0; i < 4 * TERM_MAX_HEIGHT; i++)
        cells_task();
    answer();
}

static const term_data_t *cell(int x, int y)
{
    for (int i = 0; i < 4 * TERM_MAX_HEIGHT; i++)
        cells_task();
    return cells_view_row((uint8_t)y) + x;
}

static char ch(int x, int y)
{
    return (char)cell(x, y)->font_code;
}

/* The row's text with trailing spaces dropped. */
static const char *row(int y)
{
    static char text[96];
    cells_view_t v;
    cells_view(&v);
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
    cells_view_t v;
    cells_view(&v);
    return v.cursor_x;
}

static int cy(void)
{
    cells_view_t v;
    cells_view(&v);
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
    cells_view_t v;
    cells_view(&v);
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
    cells_view_t v;
    feed("\33[?12l");
    cells_view(&v);
    ASSERT_EQ(v.cursor_style, 2);
    ASSERT_TRUE(v.cursor_enabled);
    feed("\33[4 q\33[?12h");
    cells_view(&v);
    ASSERT_EQ(v.cursor_style, 3);
    feed("\33[7 q");
    cells_view(&v);
    ASSERT_EQ(v.cursor_style, 3);
}

UTEST(term, decstr_leaves_the_cursor_and_its_style)
{
    fresh();
    feed("\33[5 q\33[4h\33[?33h\33[3;3H\33[!pab");
    cells_view_t v;
    cells_view(&v);
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
    cells_view_t v;
    cells_view(&v);
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

UTEST(term, an_aborted_osc_keeps_the_pen_true)
{
    fresh();
    feed("\33[31m\33]4;1;#00ff00;2;#0000ff\30X\33]4;1;#0000ff;\33[0mY\33[31mZ");
    ASSERT_EQ(cell(0, 0)->fg_color, rgb(0, 255, 0));
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
    ASSERT_STREQ(ask("\33[?1$p"), "\33[?1;0$y");
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
    const term_data_t *c = cell(0, 0);
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

UTEST(term, the_palette_reaches_the_pen)
{
    fresh();
    feed("\33[31m\33]4;1;rgb:00/80/ff\7A\33]104\7B");
    ASSERT_EQ(cell(0, 0)->fg_color, rgb(0, 0x80, 0xff));
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
    feed("\33[31m\33]P1102030A\33]RB");
    ASSERT_EQ(cell(0, 0)->fg_color, rgb(0x10, 0x20, 0x30));
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
    ASSERT_EQ(cells_view_row_size(0), TERM_LINE_WIDE);
    ASSERT_EQ(ch(39, 0), 'a');
    ASSERT_EQ(ch(40, 0), ' '); /* the right half is lost */
    ASSERT_EQ(cx(), 39);
    feed("\33[1;70Hz");
    ASSERT_EQ(ch(39, 0), 'z');
    ASSERT_EQ(cx(), 40); /* a wrap is pending at the row's own edge */
    feed("y");
    ASSERT_EQ(ch(0, 1), 'y');
    feed("\33#5\33[1;1H\33#5");
    ASSERT_EQ(cells_view_row_size(0), TERM_LINE_NORMAL);
    feed("\33[1;60Hq");
    ASSERT_EQ(ch(59, 0), 'q');
}

UTEST(term, line_sizes_move_with_their_lines)
{
    fresh();
    feed("\33#3top\r\n\33#4top\r\n");
    ASSERT_EQ(cells_view_row_size(0), TERM_LINE_TOP);
    ASSERT_EQ(cells_view_row_size(1), TERM_LINE_BOTTOM);
    feed("\33[H\33M"); /* a line comes in at the top */
    ASSERT_EQ(cells_view_row_size(0), TERM_LINE_NORMAL);
    ASSERT_EQ(cells_view_row_size(1), TERM_LINE_TOP);
    ASSERT_EQ(cells_view_row_size(2), TERM_LINE_BOTTOM);
    feed("\33[2J");
    ASSERT_EQ(cells_view_row_size(1), TERM_LINE_NORMAL);
}

UTEST(term, a_scroll_keeps_the_cursor_on_its_line)
{
    fresh();
    feed("\33[2H\33#6\33[1;61H\33[S"); /* the double-width line comes up */
    ASSERT_EQ(cells_view_row_size(0), TERM_LINE_WIDE);
    ASSERT_EQ(cx(), 39);
    ASSERT_STREQ(ask("\33[6n"), "\33[1;40R");
    feed("\33[P\33[100X");
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
    cells_view_t v;
    feed("\33[?5h");
    cells_view(&v);
    ASSERT_TRUE(v.reverse);
    ASSERT_STREQ(ask("\33[?5$p"), "\33[?5;1$y");
    feed("\33c");
    cells_view(&v);
    ASSERT_FALSE(v.reverse);
}

UTEST_MAIN();
