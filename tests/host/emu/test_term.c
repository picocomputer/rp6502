/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* term.c, the console's terminal, fed through the console and read through
 * its view. The console canvas holds the 80-column terminal, 30 rows. */

#include "core/api/xreg.h"
#include "core/com/com.h"
#include "core/com/manifold.h"
#include "core/com/rln.h"
#include "core/com/console.h"
#include "core/str/str.h"
#include "core/sys/sst.h"
#include "core/sys/sys.h"
#include "core/sys/version.h"
#include "core/term/color.h"
#include "core/term/cells.h"
#include "tb_screen.h"
#include "core/vga/vga.h"
#include "core/vga/vga_emu.h"
#include "core/vga/mode/mode0.h"
#include "utest.h"
#include <stdio.h>
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
    /* No frames run, so the console's line keeps no pace. */
    manifold_set_paced(false);
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

/* The colors of a cell. */
static const mode0_colors_t *ink(int x, int y)
{
    return &mode0_colors[cell(x, y) - mode0_cells];
}

static char ch(int x, int y)
{
    return (char)mode0_glyph_code(cell(x, y)->glyph);
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

/* Whether the cursor has a wrap pending. */
static bool pending(void)
{
    tb_screen_t v;
    tb_screen(&v);
    return v.cursor_pending;
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

UTEST(term, crm_draws_the_controls)
{
    fresh();
    feed("\33[3h\x07\x09\x7f\x1a\x18\33[3l\x07" "x");
    ASSERT_EQ(ch(0, 0), '\x07');
    ASSERT_EQ(ch(1, 0), '\x09');
    ASSERT_EQ(ch(2, 0), '\x7f');
    ASSERT_EQ(ch(3, 0), '\x1a');
    ASSERT_EQ(ch(4, 0), '\x18');
    ASSERT_EQ(ch(5, 0), 'x');
}

UTEST(term, crm_still_runs_the_line_controls)
{
    fresh();
    feed("\33[3hab\r\nc\bd\33[0m\x07");
    ASSERT_STREQ(row(0), "ab");
    ASSERT_STREQ(row(1), "d\x07"); /* SGR 0 leaves CRM */
}

/* SGR 10 and 11, the Linux console's way to the same switch, are not taken. */
UTEST(term, sgr_10_and_11_do_nothing)
{
    fresh();
    feed("\33[11m\x07" "z");
    ASSERT_STREQ(row(0), "z");
    feed("\r\33[3h\33[10m\x07");
    ASSERT_STREQ(row(0), "\x07");
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
    ASSERT_TRUE(cell(0, 0)->glyph == 'B');
    ASSERT_STREQ(row(0), "BC");
    ASSERT_TRUE(cell(1, 0)->attributes & TERM_ATTR_UL_MASK);
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
    ASSERT_EQ(mode0_color(ink(0, 0)->fg_color), color_256[7]);
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
    ASSERT_EQ(cx(), 79);
    ASSERT_TRUE(pending());
    ASSERT_EQ(cy(), 0);
    feed("\b");
    ASSERT_EQ(cx(), 78);
    feed("\33[999C");
    ASSERT_EQ(cx(), 79);
    ASSERT_FALSE(pending());
    feed("Y");
    ASSERT_EQ(cx(), 79);
    ASSERT_TRUE(pending());
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
    ASSERT_EQ(cx(), 79);
    ASSERT_TRUE(pending());
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
    ASSERT_TRUE(cell(0, 0)->glyph < 0x100);
    ASSERT_EQ(mode0_color(ink(0, 0)->fg_color), color_256[7]);
}

UTEST(term, scp_and_rcp_are_decsc)
{
    fresh();
    feed("\33[3;4H\33[32m\33[s\33[H\33[0m\33[uA");
    ASSERT_EQ(ch(3, 2), 'A');
    ASSERT_EQ(mode0_color(ink(3, 2)->fg_color), color_256[2]);
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
    feed("\33[3h\33[4h\33[20h\33[?33h\33[3 q\33]4;1;#123456\7\33]10;#010203\7\33c");
    tb_screen_t v;
    tb_screen(&v);
    ASSERT_EQ(v.cursor_style, 0);
    ASSERT_STREQ(ask("\33[3$p"), "\33[3;2$y");
    ASSERT_STREQ(ask("\33[4$p"), "\33[4;2$y");
    ASSERT_STREQ(ask("\33[20$p"), "\33[20;2$y");
    ASSERT_STREQ(ask("\33[?33$p"), "\33[?33;2$y");
    feed("\33[31mA\33[39mB");
    ASSERT_EQ(mode0_color(ink(0, 0)->fg_color), color_256[1]);
    ASSERT_EQ(mode0_color(ink(1, 0)->fg_color), color_256[7]);
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
    ASSERT_EQ(mode0_color(ink(5, 0)->fg_color), color_256[1]);
}

UTEST(term, the_reset_strings_leave_vt52_mode)
{
    fresh();
    feed("\33[?2l");
    feed(STR_TERM_SOFT_RESET);
    feed("\33[31mA");
    ASSERT_STREQ(row(0), "A");
    ASSERT_EQ(mode0_color(ink(0, 0)->fg_color), color_256[1]);
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
    ASSERT_EQ(mode0_color(ink(0, 0)->fg_color), rgb(0, 255, 0));
    feed("\33]4;1;#0000ff;\33[0mY\33[31mZ");
    ASSERT_EQ(mode0_color(ink(0, 0)->fg_color), rgb(0, 0, 255));
    ASSERT_EQ(mode0_color(ink(1, 0)->fg_color), color_256[7]);
    ASSERT_EQ(mode0_color(ink(2, 0)->fg_color), rgb(0, 0, 255));
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

/* XTVERSION: the console's name and the firmware's version. */
UTEST(term, xtversion_names_the_console_and_its_version)
{
    fresh();
    char want[64];
    snprintf(want, sizeof want, "\33P>|RP6502(%s)\33\\", version_bare());
    ASSERT_STREQ(ask("\33[>q"), want);
    ASSERT_STREQ(ask("\33[>0q"), want);
    ASSERT_STREQ(ask("\33[>1q"), "");
    ASSERT_STREQ(ask("\33[q"), "");
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

/* DECCOLM is 80 columns or 132. The 40-column screen is neither. */
UTEST(term, decrqm_deccolm_on_the_40_column_screen)
{
    fresh();
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_320_240));
    ASSERT_STREQ(ask("\33[?3$p"), "\33[?3;0$y");
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
    ASSERT_STREQ(ask("\33[?3$p"), "\33[?3;4$y");
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
    static uint8_t blob[1 << 18];
    fresh();
    feed("\33[?1h\33=\33[?8l");
    ASSERT_LE((size_t)cells_sst.size, sizeof blob);
    sst_cursor_t c = {blob, blob + cells_sst.size, false, NULL};
    cells_sst.save(&c, 0);
    ASSERT_TRUE(sst_ok(&c));
    feed("\33c");
    c = (sst_cursor_t){blob, blob + cells_sst.size, false, NULL};
    ASSERT_TRUE(cells_sst.load(&c, 0));
    ASSERT_TRUE(sst_ok(&c));
    ASSERT_TRUE(cells_term.app_cursor);
    ASSERT_TRUE(cells_term.app_keypad);
    ASSERT_FALSE(cells_term.autorepeat);
    feed("\33[?1l\33[?8h");
    c = (sst_cursor_t){blob, blob + cells_sst.size, false, NULL};
    cells_sst.save(&c, 0);
    feed("\33[?1h\33[?8l");
    c = (sst_cursor_t){blob, blob + cells_sst.size, false, NULL};
    ASSERT_TRUE(cells_sst.load(&c, 0));
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
    ASSERT_EQ(c->glyph, ' ');
    ASSERT_EQ(c->attributes, 0);
    ASSERT_EQ(mode0_color(ink(0, 0)->bg_color), color_256[1]);
    ASSERT_EQ(mode0_color(ink(0, 0)->fg_color), color_256[2]);
}

UTEST(term, defaults_are_not_palette_entries)
{
    fresh();
    feed("\33]4;7;#ff0000\7\33]4;0;#00ff00\7\33[37;40mA\33[39;49mB");
    ASSERT_EQ(mode0_color(ink(0, 0)->fg_color), rgb(255, 0, 0));
    ASSERT_EQ(mode0_color(ink(0, 0)->bg_color), rgb(0, 255, 0));
    ASSERT_EQ(mode0_color(ink(1, 0)->fg_color), color_256[7]);
    ASSERT_EQ(mode0_color(ink(1, 0)->bg_color), color_256[0]);
}

/* A palette entry reaches the pen, and the cells drawn with it follow it. */
UTEST(term, the_palette_reaches_the_pen)
{
    fresh();
    feed("\33[31m\33]4;1;rgb:00/80/ff\7A");
    ASSERT_EQ(mode0_color(ink(0, 0)->fg_color), rgb(0, 0x80, 0xff));
    feed("\33]104\7B");
    ASSERT_EQ(mode0_color(ink(0, 0)->fg_color), color_256[1]);
    ASSERT_EQ(mode0_color(ink(1, 0)->fg_color), color_256[1]);
}

UTEST(term, osc_takes_many_items)
{
    fresh();
    feed("\33]4;1;#010203;2;#040506\33\\\33[31mA\33[32mB");
    ASSERT_EQ(mode0_color(ink(0, 0)->fg_color), rgb(1, 2, 3));
    ASSERT_EQ(mode0_color(ink(1, 0)->fg_color), rgb(4, 5, 6));
    feed("\33]104;1\7\33[31mC\33[32mD");
    ASSERT_EQ(mode0_color(ink(2, 0)->fg_color), color_256[1]);
    ASSERT_EQ(mode0_color(ink(3, 0)->fg_color), rgb(4, 5, 6));
    feed("\33]10;#102030;#405060\7\33[0mE");
    ASSERT_EQ(mode0_color(ink(4, 0)->fg_color), rgb(0x10, 0x20, 0x30));
    ASSERT_EQ(mode0_color(ink(4, 0)->bg_color), rgb(0x40, 0x50, 0x60));
    feed("\33]110\7\33]111\7F");
    ASSERT_EQ(mode0_color(ink(5, 0)->fg_color), color_256[7]);
    ASSERT_EQ(mode0_color(ink(5, 0)->bg_color), color_256[0]);
}

UTEST(term, osc_ignores_line_feeds_inside)
{
    fresh();
    feed("\33]4;1;#0\n10203\7\33[31mA");
    ASSERT_STREQ(row(0), "A");
    ASSERT_EQ(mode0_color(ink(0, 0)->fg_color), rgb(1, 2, 3));
}

UTEST(term, ice_colors_survive_osc_11)
{
    fresh();
    feed("\33[?33h\33[5;44m\33]11;#102030\7A\33[49mB");
    ASSERT_EQ(mode0_color(ink(0, 0)->bg_color), color_256[12]);
    ASSERT_EQ(mode0_color(ink(1, 0)->bg_color), color_256[8]);
}

UTEST(term, extended_colors_check_their_numbers)
{
    fresh();
    feed("\33[32m\33[38;5;999mA\33[38;2;300;0;0mB\33[38;5mC");
    ASSERT_EQ(mode0_color(ink(0, 0)->fg_color), color_256[2]);
    ASSERT_EQ(mode0_color(ink(1, 0)->fg_color), color_256[2]);
    ASSERT_EQ(mode0_color(ink(2, 0)->fg_color), color_256[2]);
    ASSERT_EQ(cell(2, 0)->attributes & TERM_ATTR_ANY_BLINK, 0);
    feed("\33[38:2::1:2:3mD\33[38:2:4:5:6mE\33[48:5:3mF");
    ASSERT_EQ(mode0_color(ink(3, 0)->fg_color), rgb(1, 2, 3));
    ASSERT_EQ(mode0_color(ink(4, 0)->fg_color), rgb(4, 5, 6));
    ASSERT_EQ(mode0_color(ink(5, 0)->bg_color), color_256[3]);
}

UTEST(term, bold_brightens_from_the_base)
{
    fresh();
    feed("\33[1;31mA\33[22mB\33[1;39mC");
    ASSERT_EQ(mode0_color(ink(0, 0)->fg_color), color_256[9]);
    ASSERT_EQ(mode0_color(ink(1, 0)->fg_color), color_256[1]);
    ASSERT_EQ(mode0_color(ink(2, 0)->fg_color), color_256[15]);
}

UTEST(term, underline_styles)
{
    fresh();
    feed("\33[4:3mA\33[4:2mB\33[4:0mC\33[1:5mD");
    ASSERT_EQ(cell(0, 0)->attributes, TERM_ATTR_UL(TERM_UL_CURLY));
    ASSERT_EQ(cell(1, 0)->attributes, TERM_ATTR_UL(TERM_UL_DOUBLE));
    ASSERT_EQ(cell(2, 0)->attributes, 0);
    /* 1:5 is bold, not blink */
    ASSERT_EQ(cell(3, 0)->attributes, MODE0_ATTR_BOLD | MODE0_ATTR_BOLD_DEFAULT);
    ASSERT_EQ(mode0_color(ink(3, 0)->fg_color), color_256[15]);
    /* Every style, the last one set standing; a style past the last is
     * none; SGR 21 is the double one and SGR 24 ends them all. */
    feed("\33[H\33[m\33[4mA\33[4:1mB\33[4:4mC\33[4:5mD\33[4:6mE\33[4;21mF\33[24mG");
    ASSERT_EQ(cell(0, 0)->attributes, TERM_ATTR_UL(TERM_UL_SINGLE));
    ASSERT_EQ(cell(1, 0)->attributes, TERM_ATTR_UL(TERM_UL_SINGLE));
    ASSERT_EQ(cell(2, 0)->attributes, TERM_ATTR_UL(TERM_UL_DOTTED));
    ASSERT_EQ(cell(3, 0)->attributes, TERM_ATTR_UL(TERM_UL_DASHED));
    ASSERT_EQ(cell(4, 0)->attributes, 0);
    ASSERT_EQ(cell(5, 0)->attributes, TERM_ATTR_UL(TERM_UL_DOUBLE));
    ASSERT_EQ(cell(6, 0)->attributes, 0);
}

/* SGR 58 colors the underline and SGR 59 gives it back to the foreground.
 * A cell says its underline has a color of its own only when it has an
 * underline, so the renderer reads that color for no other cell. */
UTEST(term, an_underline_says_when_its_color_is_its_own)
{
    fresh();
    feed("\33[58;2;1;2;3mA\33[4:3mB\33[59mC\33[58;5;2;4mD\33[mE");
    ASSERT_EQ(cell(0, 0)->attributes, 0);
    ASSERT_EQ(cell(1, 0)->attributes, TERM_ATTR_UL(TERM_UL_CURLY) | MODE0_ATTR_UL_COLOR);
    ASSERT_EQ(mode0_color(ink(1, 0)->ul_color), rgb(1, 2, 3));
    ASSERT_EQ(cell(2, 0)->attributes, TERM_ATTR_UL(TERM_UL_CURLY));
    ASSERT_EQ(ink(2, 0)->ul_color, ink(2, 0)->fg_color);
    ASSERT_EQ(cell(3, 0)->attributes, TERM_ATTR_UL(TERM_UL_SINGLE) | MODE0_ATTR_UL_COLOR);
    ASSERT_EQ(mode0_color(ink(3, 0)->ul_color), color_256[2]);
    ASSERT_EQ(cell(4, 0)->attributes, 0);
}

/* A cell holds where the font has its glyph. SGR 3 is the italics for
 * ASCII and the code page's glyph for anything else, DEC Special Graphics
 * wins over the italics, and neither is an attribute of the cell. */
UTEST(term, a_cell_names_its_glyph_in_the_font)
{
    fresh();
    feed("A\33[3mA\xC4\33(0qa\33(Bq\33[23mq");
    ASSERT_EQ(cell(0, 0)->glyph, 'A');
    ASSERT_EQ(cell(1, 0)->glyph, MODE0_GLYPH_ITALIC(16, 'A'));
    ASSERT_EQ(cell(2, 0)->glyph, 0xC4);
    ASSERT_EQ(cell(3, 0)->glyph, MODE0_GLYPH_DEC(16, 'q'));
    ASSERT_EQ(cell(4, 0)->glyph, MODE0_GLYPH_DEC(16, 'a'));
    ASSERT_EQ(cell(5, 0)->glyph, MODE0_GLYPH_ITALIC(16, 'q'));
    ASSERT_EQ(cell(6, 0)->glyph, 'q');
    for (int x = 0; x < 7; x++)
        ASSERT_EQ(cell(x, 0)->attributes, 0);
    ASSERT_STREQ(row(0), "AA\xC4qaqq");
}

/* ---- character sets ---- */

UTEST(term, the_uk_set_has_the_pound)
{
    fresh();
    feed("\33(A#a\33(B#\33(U#\33)0\16q\17");
    ASSERT_EQ(cell(0, 0)->glyph, MODE0_GLYPH_DEC(16, 0x7D));
    ASSERT_EQ(ch(1, 0), 'a');
    ASSERT_EQ(ch(2, 0), '#');
    ASSERT_EQ(cell(3, 0)->attributes, 0);
    ASSERT_TRUE(mode0_glyph_is_dec(cell(4, 0)->glyph));
}

/* ---- VT52 ---- */

UTEST(term, vt52_mode)
{
    fresh();
    feed("\33[?2l\33Y%*Z\33Z\33[\33Fq\33Gq\33<\33[2Cx");
    ASSERT_EQ(ch(10, 5), 'Z');
    ASSERT_STREQ(answer(), "\33/Z");
    ASSERT_TRUE(mode0_glyph_is_dec(cell(11, 5)->glyph));
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
    ASSERT_EQ(mode0_color(ink(2, 2)->fg_color), color_256[7]);
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
    ASSERT_EQ(cx(), 39);
    ASSERT_TRUE(pending()); /* a wrap is pending at the row's own edge */
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
    ASSERT_STREQ(row(1), "123456"); /* the cursor is outside the region, as on xterm */
    feed("\33[2;1H\33[2 @");
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
    ASSERT_EQ(mode0_color(ink(0, 0)->fg_color), rgb(0x10, 0x20, 0x30));
    ASSERT_EQ(mode0_color(ink(0, 0)->bg_color), rgb(0x40, 0x50, 0x60));
    ASSERT_EQ(mode0_color(ink(1, 0)->fg_color), color_256[15]);
    ASSERT_EQ(mode0_color(ink(1, 0)->bg_color), rgb(0x40, 0x50, 0x60));
    feed("\33]4;15;#0000ff\7");
    ASSERT_EQ(mode0_color(ink(1, 0)->fg_color), rgb(0, 0, 255));
    feed("\33]110\7\33]111\7");
    ASSERT_EQ(mode0_color(ink(0, 0)->fg_color), color_256[7]);
    ASSERT_EQ(mode0_color(ink(0, 0)->bg_color), color_256[0]);
}

/* A cell follows the entries it was drawn from through SGR 7, SGR 8 and
 * faint. Its underline follows its foreground, or the entry SGR 58 gave it.
 * An RGB color follows nothing. */
UTEST(term, a_recolor_follows_each_cells_sources)
{
    fresh();
    feed("\33[7;31;42mR\33[0m\33[8;31;42mC\33[0m\33[2;31mF\33[0m"
         "\33[4;31mU\33[58;5;3mV\33[0m\33[38;2;1;2;3mX");
    feed("\33]4;1;#ff0000\7\33]4;2;#00ff00\7\33]4;3;#0000ff\7");
    ASSERT_EQ(mode0_color(ink(0, 0)->fg_color), rgb(0, 255, 0));
    ASSERT_EQ(mode0_color(ink(0, 0)->bg_color), rgb(255, 0, 0));
    ASSERT_EQ(mode0_color(ink(1, 0)->fg_color), rgb(0, 255, 0));
    ASSERT_EQ(mode0_color(ink(1, 0)->bg_color), rgb(0, 255, 0));
    ASSERT_EQ(mode0_color(ink(2, 0)->fg_color), SCANVIDEO_ALPHA_MASK | SCANVIDEO_PIXEL_FROM_RGB5(15, 0, 0));
    ASSERT_EQ(mode0_color(ink(3, 0)->ul_color), rgb(255, 0, 0));
    ASSERT_EQ(mode0_color(ink(4, 0)->ul_color), rgb(0, 0, 255));
    ASSERT_EQ(mode0_color(ink(5, 0)->fg_color), rgb(1, 2, 3));
}

/* A cell holds the entry and not the color, so a palette change needs no
 * pass over the cells: it shows with the cells untouched. */
UTEST(term, a_palette_change_touches_no_cell)
{
    fresh();
    feed("\33[31mA\33[2;32mB\33[0m\33[38;2;9;8;7mC");
    const mode0_cell_t before[3] = {*cell(0, 0), *cell(1, 0), *cell(2, 0)};
    const mode0_colors_t inks[3] = {*ink(0, 0), *ink(1, 0), *ink(2, 0)};
    feed("\33]4;1;#102030\7\33]4;2;#204060\7");
    ASSERT_EQ(0, memcmp(before, cell(0, 0), sizeof before));
    ASSERT_EQ(0, memcmp(inks, ink(0, 0), sizeof inks));
    ASSERT_EQ(mode0_color(ink(0, 0)->fg_color), rgb(0x10, 0x20, 0x30));
    ASSERT_EQ(mode0_color(ink(1, 0)->fg_color),
              SCANVIDEO_ALPHA_MASK | SCANVIDEO_PIXEL_FROM_RGB5(0x20 >> 4, 0x40 >> 4, 0x60 >> 4));
    ASSERT_EQ(mode0_color(ink(2, 0)->fg_color), rgb(9, 8, 7));
    /* An RGB color is the pixel itself. */
    ASSERT_TRUE(ink(2, 0)->fg_color & SCANVIDEO_ALPHA_MASK);
    ASSERT_FALSE(ink(0, 0)->fg_color & SCANVIDEO_ALPHA_MASK);
}

/* SGR 38:1 and 48:1 were the console's own transparent color. They are
 * forms SGR does not know now: the colon form is skipped whole, and the
 * semicolon form takes its 1 with it, so it is not bold. */
UTEST(term, sgr_38_1_is_no_color)
{
    fresh();
    feed("\33[31;38:1mA\33[0m\33[32;48:1mB\33[0m\33[31;38;1mC");
    ASSERT_EQ(mode0_color(ink(0, 0)->fg_color), color_256[1]);
    ASSERT_EQ(mode0_color(ink(1, 0)->fg_color), color_256[2]);
    ASSERT_EQ(mode0_color(ink(1, 0)->bg_color), color_256[0]);
    ASSERT_EQ(mode0_color(ink(2, 0)->fg_color), color_256[1]);
}

/* A lazy erase still owed takes a recolor too. */
UTEST(term, an_owed_erase_takes_a_recolor)
{
    fresh();
    feed("\33[44m\33[2J\33]4;4;#123456\7");
    ASSERT_EQ(mode0_color(ink(10, 10)->bg_color), rgb(0x12, 0x34, 0x56));
}

/* Bracketed paste is a mode a program sets, and a reset clears. */
UTEST(term, bracketed_paste_is_a_mode)
{
    fresh();
    feed("\33[?2004h");
    ASSERT_STREQ(ask("\33[?2004$p"), "\33[?2004;1$y");
    feed("\33c");
    ASSERT_STREQ(ask("\33[?2004$p"), "\33[?2004;2$y");
}

/* Linux's palette sequences are not taken: ESC ] P begins an OSC like any
 * other, which runs to its terminator. */
UTEST(term, linux_palette_is_an_osc_like_any_other)
{
    fresh();
    feed("\33[31m\33]P1102030A\7B");
    ASSERT_STREQ(row(0), "B");
    ASSERT_EQ(mode0_color(ink(0, 0)->fg_color), color_256[1]);
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

/* A cell's guard goes where the cell goes: through ICH, DCH, SL and SR, the
 * selective erase after each spares the same two cells and nothing else. */
UTEST(term, a_guard_moves_with_its_cell)
{
    fresh();
    feed("\33[1\"qAB\33[0\"qCD\33[H\33[2@\33[?2K");
    ASSERT_STREQ(row(0), "  AB");
    feed("\33[0\"qxy\33[H\33[P\33[?2K");
    ASSERT_STREQ(row(0), " AB");
    feed("\33[1 @\33[?2K");
    ASSERT_STREQ(row(0), "AB");
    feed("\33[3 A\33[?2K");
    ASSERT_STREQ(row(0), "   AB");
    /* An erase that is not selective takes the guards with the cells: a
     * selective erase in another color then reaches where they were. */
    feed("\33[2K\33[41m\33[?2K");
    ASSERT_STREQ(row(0), "");
    ASSERT_EQ(mode0_color(ink(3, 0)->bg_color), color_256[1]);
    ASSERT_EQ(mode0_color(ink(4, 0)->bg_color), color_256[1]);
}

/* SL and SR against a model of the row. A row is owed its shifts, and a
 * second shift only changes what it is owed, so each run of one to six
 * shifts, in three colors, is checked as one: the glyphs that are left,
 * the color of every blank, and the guard beside every cell. The row is the
 * cursor's on every fourth run, where it is paid at each shift. */
UTEST(term, shifts_owed_to_a_row_are_the_shifts_done)
{
    uint32_t state = 1;
    for (int run = 0; run < 3000; run++)
    {
        char glyph[80], seq[32];
        int bg[80];
        bool guard[80];
        fresh();
        feed("\33[3;1H");
        for (int i = 0; i < 80; i++)
        {
            glyph[i] = (char)('!' + i);
            bg[i] = 0;
            guard[i] = i % 3 == 0;
            feed(guard[i] ? "\33[1\"q" : "\33[0\"q");
            feedn(&glyph[i], 1);
        }
        feed(run % 4 ? "\33[11;1H" : "\33[3;5H");
        state = state * 1664525u + 1013904223u;
        const int shifts = 1 + (int)((state >> 8) % 6);
        for (int k = 0; k < shifts; k++)
        {
            state = state * 1664525u + 1013904223u;
            const int n = 1 + (int)((state >> 8) % 12 ? (state >> 12) % 9 : (state >> 12) % 90);
            const bool left = (state >> 20) & 1;
            const int color = 1 + (int)((state >> 21) % 8 ? 0 : (state >> 24) % 3);
            snprintf(seq, sizeof seq, "\33[4%dm\33[%d %c", color, n, left ? '@' : 'A');
            feed(seq);
            for (int j = 0; j < 80; j++)
            {
                const int i = left ? j : 79 - j;
                const int f = left ? i + n : i - n;
                const bool in = f >= 0 && f < 80;
                glyph[i] = in ? glyph[f] : ' ';
                bg[i] = in ? bg[f] : color;
                guard[i] = in && guard[f];
            }
        }
        for (int i = 0; i < 80; i++)
        {
            const mode0_cell_t *c = cell(i, 2);
            ASSERT_EQ(c->glyph, (uint8_t)glyph[i]);
            if (bg[i])
                ASSERT_EQ(mode0_color(ink(i, 2)->bg_color), color_256[bg[i]]);
            ASSERT_EQ((int)!!(c->attributes & MODE0_ATTR_GUARD), (int)guard[i]);
        }
    }
}

/* ICH and DCH of every count from several columns: the cells that move
 * keep their glyphs and their colors, and each takes its guard with it, so
 * the selective erase after spares exactly the guarded cells where they now
 * are. Every third cell of the row is guarded, and each cell has one of
 * seven backgrounds. */
UTEST(term, a_run_of_cells_moves_whole_at_every_length)
{
    static const int from[] = {0, 1, 2, 3, 5, 40, 77};
    for (unsigned f = 0; f < sizeof from / sizeof from[0]; f++)
        for (int n = 1; n <= 80 - from[f]; n++)
            for (int insert = 0; insert < 2; insert++)
            {
                char was[81], want[81], spared[81], seq[32];
                int came[80];
                memset(came, -1, sizeof came);
                fresh();
                for (int i = 0; i < 80; i++)
                {
                    was[i] = (char)('!' + i);
                    feed(i % 3 ? "\33[0\"q" : "\33[1\"q");
                    snprintf(seq, sizeof seq, "\33[4%dm", 1 + i % 7);
                    feed(seq);
                    feedn(&was[i], 1);
                }
                const int x = from[f];
                memset(want, ' ', 80);
                memset(spared, ' ', 80);
                for (int i = 0; i < 80; i++)
                {
                    int to = i < x ? i : insert ? i + n : i - n;
                    if (i >= x && !insert && i < x + n)
                        continue; /* deleted */
                    if (to >= 80)
                        continue; /* pushed off the end */
                    want[to] = was[i];
                    came[to] = i;
                    if (i % 3 == 0)
                        spared[to] = was[i];
                }
                for (int end = 80; end > 0 && want[end - 1] == ' '; end--)
                    want[end - 1] = 0;
                for (int end = 80; end > 0 && spared[end - 1] == ' '; end--)
                    spared[end - 1] = 0;
                want[80] = spared[80] = 0;
                snprintf(seq, sizeof seq, "\33[1;%dH\33[%d%c", x + 1, n, insert ? '@' : 'P');
                feed(seq);
                ASSERT_STREQ(row(0), want);
                for (int i = 0; i < 80; i++)
                    if (came[i] >= 0)
                        ASSERT_EQ(mode0_color(ink(i, 0)->bg_color), color_256[1 + came[i] % 7]);
                feed("\33[?2K");
                ASSERT_STREQ(row(0), spared);
            }
}

/* What a row is owed is never seen once it is paid. The same bytes are fed
 * twice: once with every owed fill paid after each sequence, and once with
 * them left owing until the end, through every scroll, insert, erase, shift
 * and change of height that comes meanwhile. The two screens are the same,
 * cell for cell, with the guards beside them. */
static uint32_t lazy_state;
static unsigned lazy_rand(unsigned n)
{
    lazy_state = lazy_state * 1664525u + 1013904223u;
    return (lazy_state >> 8) % n;
}

static void lazy_op(char *out, size_t size)
{
    switch (lazy_rand(27))
    {
    case 0:
    case 1:
    case 2:
    {
        unsigned n = 1 + lazy_rand(100);
        for (unsigned i = 0; i < n; i++)
            out[i] = (char)('a' + lazy_rand(26));
        out[n] = 0;
        break;
    }
    case 3: snprintf(out, size, "\r\n"); break;
    case 4: snprintf(out, size, "\n\n\n"); break;
    case 5: snprintf(out, size, "\33M"); break;
    case 6: snprintf(out, size, "\33[%u;%uH", 1 + lazy_rand(30), 1 + lazy_rand(80)); break;
    case 7: snprintf(out, size, "\33[%uJ", lazy_rand(3)); break;
    case 8: snprintf(out, size, "\33[%uK", lazy_rand(3)); break;
    case 9: snprintf(out, size, "\33[?%u%c", lazy_rand(3), lazy_rand(2) ? 'J' : 'K'); break;
    case 10: snprintf(out, size, "\33[%u%c", 1 + lazy_rand(31), lazy_rand(2) ? 'L' : 'M'); break;
    case 11: snprintf(out, size, "\33[%u%c", 1 + lazy_rand(31), lazy_rand(2) ? 'S' : 'T'); break;
    case 12: snprintf(out, size, "\33[%u%c", 1 + lazy_rand(81), "@PX"[lazy_rand(3)]); break;
    case 13: snprintf(out, size, "\33[%u %c", 1 + lazy_rand(81), lazy_rand(2) ? '@' : 'A'); break;
    case 14: snprintf(out, size, "%c\33[%ub", (char)('A' + lazy_rand(26)), 1 + lazy_rand(3000)); break;
    case 15: snprintf(out, size, "%c\33[%ub", (char)('A' + lazy_rand(26)), 1 + lazy_rand(200)); break;
    case 16:
    {
        unsigned top = 1 + lazy_rand(29);
        snprintf(out, size, "\33[%u;%ur", top, top + lazy_rand(31 - top));
        break;
    }
    case 17: snprintf(out, size, "\33[r"); break;
    case 18: snprintf(out, size, "\33[%u;%um", 30 + lazy_rand(8), 40 + lazy_rand(8)); break;
    case 19: snprintf(out, size, "\33[%um", (unsigned[]){0, 4, 7, 2, 5, 1}[lazy_rand(6)]); break;
    case 20: snprintf(out, size, "\33[%u\"q", lazy_rand(2)); break;
    case 21: snprintf(out, size, "\33#8"); break;
    case 22: snprintf(out, size, "\33[?1049%c", lazy_rand(2) ? 'h' : 'l'); break;
    case 23: snprintf(out, size, "\33#%c", "3456"[lazy_rand(4)]); break;
    case 24: snprintf(out, size, "\33[4%c", lazy_rand(2) ? 'h' : 'l'); break;
    case 25: snprintf(out, size, "\33[?7%c", lazy_rand(4) ? 'h' : 'l'); break;
    default: out[0] = 0; break; /* a change of height, which the caller makes */
    }
}

#define LAZY_OPS 60
static char lazy_seq[LAZY_OPS][128];
static uint8_t lazy_height[LAZY_OPS];
static bool lazy_skip[LAZY_OPS];

static void lazy_make(uint32_t seed)
{
    lazy_state = seed;
    for (int k = 0; k < LAZY_OPS; k++)
    {
        lazy_op(lazy_seq[k], sizeof lazy_seq[k]);
        lazy_height[k] = lazy_seq[k][0] ? 0 : (uint8_t)(10 + lazy_rand(21));
        lazy_skip[k] = false;
    }
}

static void lazy_run(bool pay_each, uint8_t *screen)
{
    fresh();
    for (int k = 0; k < LAZY_OPS; k++)
    {
        if (lazy_skip[k])
            continue;
        if (lazy_height[k])
            cells_set_height(80, lazy_height[k]);
        else
            feed(lazy_seq[k]);
        if (pay_each)
            for (int i = 0; i < TB_SCREEN_SETTLE; i++)
                cells_task();
    }
    cells_set_height(80, 30);
    for (int i = 0; i < TB_SCREEN_SETTLE; i++)
        cells_task();
    for (int y = 0; y < 30; y++)
    {
        const mode0_cell_t *r = tb_screen_row((uint8_t)y);
        memcpy(screen + y * 80 * 10, r, 80 * 4);
        memcpy(screen + y * 80 * 10 + 80 * 4, &mode0_colors[r - mode0_cells], 80 * 6);
    }
}

static uint8_t lazy_paid[30 * 80 * 10], lazy_owing[30 * 80 * 10];
static bool lazy_differs(void)
{
    lazy_run(true, lazy_paid);
    lazy_run(false, lazy_owing);
    return memcmp(lazy_paid, lazy_owing, sizeof lazy_paid) != 0;
}

UTEST(term, what_a_row_is_owed_is_never_seen)
{
    for (uint32_t seed = 1; seed <= 2000; seed++)
    {
        lazy_make(seed);
        if (!lazy_differs())
            continue;
        /* The fewest of this seed's sequences that still differ. */
        for (int k = 0; k < LAZY_OPS; k++)
        {
            lazy_skip[k] = true;
            if (!lazy_differs())
                lazy_skip[k] = false;
        }
        ASSERT_TRUE(lazy_differs());
        int at = 0;
        while (lazy_paid[at] == lazy_owing[at])
            at++;
        printf("seed %u: row %d, byte %d is %02x paid at once and %02x left owing, after:\n",
               (unsigned)seed, at / 800, at % 800, lazy_paid[at], lazy_owing[at]);
        for (int k = 0; k < LAZY_OPS; k++)
        {
            if (lazy_skip[k])
                continue;
            printf("  ");
            if (lazy_height[k])
                printf("height %u", lazy_height[k]);
            for (const char *c = lazy_seq[k]; *c; c++)
                printf(*c == 27 ? "ESC" : *c < 32 ? "^%c" : "%c", *c < 32 ? *c + 64 : *c);
            printf("\n");
        }
        ASSERT_TRUE(false);
    }
}

/* REP draws what printing the glyph that many times draws: the same cells,
 * with the pen's attributes and guard, and the cursor in the same place.
 * Its whole rows are only owed their glyphs, so this is those rows paid. */
static void rep_screen(const char *before, int n, bool rep, uint8_t *screen, int *x, int *y)
{
    char seq[32];
    fresh();
    feed(before);
    feed("Q");
    if (rep)
    {
        snprintf(seq, sizeof seq, "\33[%db", n);
        feed(seq);
    }
    else
        for (int i = 0; i < n; i++)
            feed("Q");
    /* The far half of a double-size row shows when the row is single again. */
    feed("\0337\33[10;1H\33#5\0338");
    for (int i = 0; i < TB_SCREEN_SETTLE; i++)
        cells_task();
    for (int r = 0; r < 30; r++)
    {
        const mode0_cell_t *row = tb_screen_row((uint8_t)r);
        memcpy(screen + r * 80 * 10, row, 80 * 4);
        memcpy(screen + r * 80 * 10 + 80 * 4, &mode0_colors[row - mode0_cells], 80 * 6);
    }
    *x = cx();
    *y = cy();
}

UTEST(term, rep_draws_what_printing_draws)
{
    static const char *const before[] = {
        "",
        "\33[4;5;31;42m\33[1\"q",
        "\33[5;9r\33[6;40H",
        "\33[10;1H\33#6abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\33[9;70H",
        "\33[?7l\33[3;70H",
        "\33[4h\33[2;1Hsome text to push along\33[2;5H",
        "\33[30;1H",
        "\33[2J\33[41m\33[29;60H",
    };
    static const int counts[] = {1, 9, 10, 11, 79, 80, 81, 159, 160, 161, 500, 2399, 2400, 2401};
    static uint8_t a[30 * 80 * 10], b[30 * 80 * 10];
    for (unsigned i = 0; i < sizeof before / sizeof before[0]; i++)
        for (unsigned k = 0; k < sizeof counts / sizeof counts[0]; k++)
        {
            int ax, ay, bx, by;
            rep_screen(before[i], counts[k], true, a, &ax, &ay);
            rep_screen(before[i], counts[k], false, b, &bx, &by);
            if (memcmp(a, b, sizeof a) || ax != bx || ay != by)
                printf("REP %d after case %u differs\n", counts[k], i);
            ASSERT_EQ(0, memcmp(a, b, sizeof a));
            ASSERT_EQ(ax, bx);
            ASSERT_EQ(ay, by);
        }
}

/* The palette a program changed and the cells it guarded are in the
 * savestate: both come back over a console that has since lost them. */
UTEST(term, the_palette_and_the_guards_come_back)
{
    static uint8_t blob[1 << 20];
    fresh();
    feed("\33]4;1;#102030\7\33]10;#405060\7\33[31mA\33[39mB\33[1\"qG\33[0\"qh");
    ASSERT_EQ(sst_save(blob, sizeof blob, 0), (const char *)NULL);
    feed("\33c");
    ASSERT_STREQ(row(0), "");
    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_STREQ(row(0), "ABGh");
    ASSERT_EQ(mode0_color(ink(0, 0)->fg_color), rgb(0x10, 0x20, 0x30));
    ASSERT_EQ(mode0_color(ink(1, 0)->fg_color), rgb(0x40, 0x50, 0x60));
    feed("\33[?2K");
    ASSERT_STREQ(row(0), "  G");
}

/* A shift a row is still owed is saved with the row and paid after the
 * load: the survivors, and the blank color for each side. */
UTEST(term, a_shift_still_owed_comes_back)
{
    static uint8_t blob[1 << 20];
    fresh();
    feed("\33[3;1HABCDEF\33[10;1H\33[41m\33[2 A\33[42m\33[3 @");
    ASSERT_EQ(sst_save(blob, sizeof blob, 0), (const char *)NULL);
    feed("\33c");
    ASSERT_STREQ(row(2), "");
    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_STREQ(row(2), "BCDEF");
    ASSERT_EQ(mode0_color(ink(79, 2)->bg_color), color_256[2]);
    feed("\33[3;1H\33[43m\33[2 A");
    ASSERT_STREQ(row(2), "  BCDEF");
    ASSERT_EQ(mode0_color(ink(0, 2)->bg_color), color_256[3]);
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

/* ---- left and right margins (DECSLRM) ------------------------------------ */

/* Rows 0 to n - 1 hold ten of a, b, c and on. */
static void lettered(int n)
{
    feed("\33[H");
    for (int y = 0; y < n; y++)
    {
        char line[16];
        snprintf(line, sizeof line, "%s", "..........\r\n");
        for (int x = 0; x < 10; x++)
            line[x] = (char)('a' + y);
        feed(line);
    }
}

/* CSI s saves the cursor until DECLRMM makes it DECSLRM, which sends the
 * cursor home. DECRQM reports the mode, and resetting it resets the margins. */
UTEST(term, declrmm_makes_csi_s_set_the_margins)
{
    fresh();
    feed("\33[5;5H\33[s\33[H\33[u");
    ASSERT_EQ(cx(), 4);
    ASSERT_STREQ(ask("\33[?69$p"), "\33[?69;2$y");
    feed("\33[?69h\33[5;10s");
    ASSERT_STREQ(ask("\33[?69$p"), "\33[?69;1$y");
    ASSERT_EQ(cx(), 0); /* home, and DECOM is off */
    ASSERT_EQ(cy(), 0);
    feed("\33[1;5Habcdefgh");
    ASSERT_STREQ(row(0), "    abcdef");
    ASSERT_STREQ(row(1), "    gh");
    feed("\33[?69l\33[1;5Habcdefgh");
    ASSERT_STREQ(row(0), "    abcdefgh");
    /* A right margin not past the left one is not taken. */
    feed("\33[?69h\33[10;10s\33[1;75Habcdefgh");
    ASSERT_STREQ(row(1), "gh  gh");
}

/* A wrap is pending at the right margin, on the margin's own column. */
UTEST(term, a_wrap_is_pending_at_the_right_margin)
{
    fresh();
    feed("\33[?69h\33[5;10s\33[1;5Habcdef");
    ASSERT_EQ(cx(), 9);
    ASSERT_TRUE(pending());
    feed("\33[?7lxyz");
    ASSERT_STREQ(row(0), "    abcdez"); /* DECAWM off: the margin is written over */
}

/* A cursor between the margins stops at them; one outside them stops at the
 * edges of the screen. */
UTEST(term, the_cursor_stops_at_the_left_and_right_margins)
{
    fresh();
    feed("\33[?69h\33[11;20s\33[1;15H");
    feed("\33[99D");
    ASSERT_EQ(cx(), 10);
    feed("\33[99C");
    ASSERT_EQ(cx(), 19);
    feed("\b\b\b\b\b\b\b\b\b\b\b\b");
    ASSERT_EQ(cx(), 10);
    feed("\t\t\t");
    ASSERT_EQ(cx(), 19);
    feed("\r");
    ASSERT_EQ(cx(), 10);
    feed("\33[1;5H\33[99D");
    ASSERT_EQ(cx(), 0);
    feed("\33[1;5H\r");
    ASSERT_EQ(cx(), 0); /* left of the margin, the first column */
    feed("\33[1;30H\33[99C");
    ASSERT_EQ(cx(), 79);
    feed("\r");
    ASSERT_EQ(cx(), 10);
    feed("\33[1;5H\33[99C");
    ASSERT_EQ(cx(), 19); /* left of the margins, a move right stops at the right one */
}

/* Under DECOM a column counts from the left margin and the report does too. */
UTEST(term, decom_counts_columns_from_the_left_margin)
{
    fresh();
    feed("\33[?69h\33[11;20s\33[5;9r\33[?6h");
    ASSERT_EQ(cx(), 10);
    ASSERT_EQ(cy(), 4);
    feed("\33[2;3H");
    ASSERT_EQ(cx(), 12);
    ASSERT_EQ(cy(), 5);
    ASSERT_STREQ(ask("\33[6n"), "\33[2;3R");
    feed("\33[99;99H");
    ASSERT_EQ(cx(), 19);
    ASSERT_EQ(cy(), 8);
    feed("\33[4G");
    ASSERT_EQ(cx(), 13);
}

/* A scroll moves only the columns between the margins, and only for a cursor
 * between them. */
UTEST(term, a_scroll_moves_only_the_columns_between_the_margins)
{
    fresh();
    lettered(5);
    feed("\33[?69h\33[2;4r\33[4;7s\33[4;5H\n");
    ASSERT_STREQ(row(0), "aaaaaaaaaa");
    ASSERT_STREQ(row(1), "bbbccccbbb");
    ASSERT_STREQ(row(2), "cccddddccc");
    ASSERT_STREQ(row(3), "ddd    ddd");
    ASSERT_STREQ(row(4), "eeeeeeeeee");
    ASSERT_EQ(cy(), 3);
    feed("\33[2;5H\33M");
    ASSERT_STREQ(row(1), "bbb    bbb");
    ASSERT_STREQ(row(2), "ccccccc" "ccc");
    ASSERT_STREQ(row(3), "ddddddd" "ddd");
    /* Outside the columns the cursor stays on the margin and nothing moves. */
    feed("\33[4;1H\n");
    ASSERT_EQ(cy(), 3);
    ASSERT_STREQ(row(3), "dddddddddd");
    feed("\33[2;9H\33M");
    ASSERT_EQ(cy(), 1);
    ASSERT_STREQ(row(1), "bbb    bbb");
    /* SU and SD. */
    feed("\33[2S");
    ASSERT_STREQ(row(1), "bbbddddbbb");
    ASSERT_STREQ(row(2), "ccc    ccc");
    ASSERT_STREQ(row(3), "ddd    ddd");
    feed("\33[T");
    ASSERT_STREQ(row(1), "bbb    bbb");
    ASSERT_STREQ(row(2), "cccddddccc");
}

/* The rows that come in take the pen's background, between the margins. */
UTEST(term, a_scroll_between_margins_erases_in_the_pens_colors)
{
    fresh();
    lettered(5);
    feed("\33[?69h\33[2;4r\33[4;7s\33[4;5H\33[44m\n");
    ASSERT_EQ(mode0_color(ink(3, 3)->bg_color), color_256[4]);
    ASSERT_EQ(mode0_color(ink(6, 3)->bg_color), color_256[4]);
    ASSERT_EQ(mode0_color(ink(2, 3)->bg_color), color_256[0]);
    ASSERT_EQ(mode0_color(ink(7, 3)->bg_color), color_256[0]);
}

/* IL and DL move the columns between the margins from the cursor's row, and
 * leave the cursor on the left margin. Outside the margins they do nothing. */
UTEST(term, il_and_dl_keep_between_the_margins)
{
    fresh();
    lettered(5);
    feed("\33[?69h\33[4;7s\33[2;6H\33[L");
    ASSERT_STREQ(row(1), "bbb    bbb");
    ASSERT_STREQ(row(2), "cccbbbbccc");
    ASSERT_STREQ(row(4), "eeeddddeee");
    ASSERT_EQ(cx(), 3);
    feed("\33[2M");
    ASSERT_STREQ(row(1), "bbbccccbbb");
    ASSERT_STREQ(row(2), "cccddddccc");
    feed("\33[2;1H\33[L\33[2;9H\33[M");
    ASSERT_STREQ(row(1), "bbbccccbbb");
}

/* ICH, DCH and insert mode stop at the right margin; SL and SR move the
 * columns between the margins. */
UTEST(term, editing_stops_at_the_right_margin)
{
    fresh();
    feed("0123456789\r\n0123456789\33[?69h\33[4;7s\33[1;4H\33[2@");
    ASSERT_STREQ(row(0), "012  34789");
    feed("\33[P");
    ASSERT_STREQ(row(0), "012 34 789");
    feed("\33[4hZ\33[4l");
    ASSERT_STREQ(row(0), "012Z 34789");
    feed("\33[1;2H\33[5@\33[5P");
    ASSERT_STREQ(row(0), "012Z 34789"); /* outside the margins */
    feed("\33[1;5H\33[2 @");
    ASSERT_STREQ(row(0), "01234  789");
    ASSERT_STREQ(row(1), "01256  789");
    feed("\33[ A");
    ASSERT_STREQ(row(0), "012 34 789");
    ASSERT_STREQ(row(1), "012 56 789");
    feed("\33[1;1H\33[3 @");
    ASSERT_STREQ(row(0), "012 34 789"); /* the cursor is outside the margins */
}

/* REP wraps between the margins, however long. */
UTEST(term, rep_wraps_between_the_margins)
{
    fresh();
    feed("\33[?69h\33[3;6sa\33[9b");
    ASSERT_STREQ(row(0), "aaaaaa");
    ASSERT_STREQ(row(1), "  aaaa");
    ASSERT_EQ(cx(), 5);
    ASSERT_TRUE(pending());
    feed("\33[2J\33[1;3Hb\33[65535b");
    for (int y = 0; y < 30; y++)
        ASSERT_STREQ(row(y), "  bbbb");
    /* 65536 glyphs from the left margin, four a row: the last fills a row. */
    ASSERT_EQ(cx(), 5);
    ASSERT_TRUE(pending());
}

/* With the margins allowed every line is single size, and stays so. DECSTR
 * and DECALN reset the margins, and DECSTR the mode with them. */
UTEST(term, declrmm_has_no_double_size_lines)
{
    fresh();
    feed("\33#6");
    ASSERT_EQ(tb_screen_row_size(0), TERM_LINE_WIDE);
    feed("\33[?69h");
    ASSERT_EQ(tb_screen_row_size(0), TERM_LINE_NORMAL);
    feed("\33#6\33#3");
    ASSERT_EQ(tb_screen_row_size(0), TERM_LINE_NORMAL);
    feed("\33[5;10s\33#8\33[1;9Hxyz");
    ASSERT_STREQ(ask("\33[?69$p"), "\33[?69;1$y");
    ASSERT_EQ(ch(10, 0), 'z'); /* DECALN reset the margins */
    feed("\33[5;10s\33[!p");
    ASSERT_STREQ(ask("\33[?69$p"), "\33[?69;2$y");
    feed("\33[2J\33[1;9Hxyz");
    ASSERT_EQ(ch(10, 0), 'z');
}

/* The margins are part of a savestate. */
UTEST(term, the_left_and_right_margins_come_back)
{
    static uint8_t blob[1 << 20];
    fresh();
    feed("\33[?69h\33[5;10s");
    ASSERT_EQ(sst_save(blob, sizeof blob, 0), (const char *)NULL);
    feed("\33c");
    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    feed("\33[1;5Habcdefgh");
    ASSERT_STREQ(row(0), "    abcdef");
    ASSERT_STREQ(row(1), "    gh");
}

/* ---- rectangles ---------------------------------------------------------- */

/* DECFRA fills with a character as the pen writes it. A whole row is owed
 * its fill, and the cursor's row is filled at once. */
UTEST(term, decfra_fills_a_rectangle_in_the_pen)
{
    fresh();
    feed("\33[31;4m\33[88;2;3;4;6$x\33[0m");
    ASSERT_STREQ(row(0), "");
    ASSERT_STREQ(row(1), "  XXXX");
    ASSERT_STREQ(row(3), "  XXXX");
    ASSERT_STREQ(row(4), "");
    ASSERT_EQ(mode0_color(ink(2, 1)->fg_color), color_256[1]);
    ASSERT_EQ(cell(5, 3)->attributes, TERM_ATTR_UL(TERM_UL_SINGLE));
    ASSERT_EQ(cx(), 0);
    ASSERT_EQ(cy(), 0);
    feed("\33[45;1;1;3;80$x");
    for (int y = 0; y < 3; y++)
    {
        ASSERT_EQ(ch(0, y), '-');
        ASSERT_EQ(ch(79, y), '-');
    }
    ASSERT_STREQ(row(3), "  XXXX");
    /* Left out, the sides are the screen's and the character a space. */
    feed("\33[;29$x");
    ASSERT_STREQ(row(27), "");
    feed("\33(0\33[113;30$x\33(B");
    ASSERT_TRUE(mode0_glyph_is_dec(cell(40, 29)->glyph));
    /* A control is no character to fill with. */
    feed("\33[7;1;1;2;2$x\33[127;1;1;2;2$x\33[256;1;1;2;2$x");
    ASSERT_EQ(ch(0, 0), '-');
}

/* A rectangle on a row that is still owed an erase lands on the erased row. */
UTEST(term, a_rectangle_lands_on_a_row_as_it_is_owed)
{
    fresh();
    feed("old\33[5;1Hold\33[1;1H\33[44m\33[2J\33[0m\33[88;5;5;5;6$x");
    ASSERT_STREQ(row(4), "    XX");
    ASSERT_EQ(mode0_color(ink(0, 4)->bg_color), color_256[4]);
    ASSERT_EQ(mode0_color(ink(4, 4)->bg_color), color_256[0]);
}

/* DECERA erases protected cells too; DECSERA leaves them. */
UTEST(term, decera_erases_and_decsera_spares_the_protected)
{
    fresh();
    feed("\33#8\33[2;2H\33[1\"qP\33[0\"q\33[44m\33[2;2;3;4$z\33[0m");
    ASSERT_STREQ(row(0), "EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE");
    ASSERT_EQ(ch(0, 1), 'E');
    ASSERT_EQ(ch(1, 1), ' ');
    ASSERT_EQ(ch(3, 2), ' ');
    ASSERT_EQ(ch(4, 2), 'E');
    ASSERT_EQ(ch(1, 3), 'E');
    ASSERT_EQ(mode0_color(ink(1, 1)->bg_color), color_256[4]);
    feed("\33[5;5H\33[1\"qPQ\33[0\"q\33[5;1;6;80${");
    ASSERT_STREQ(row(4), "    PQ");
    ASSERT_STREQ(row(5), "");
    feed("\33[5;1;5;80$z");
    ASSERT_STREQ(row(4), "");
}

/* DECCRA copies cells, colors and attributes, onto itself too, and loses
 * what would land past the edge. */
UTEST(term, deccra_copies_a_rectangle)
{
    fresh();
    feed("\33[31;4mabc\33[0mde\r\nfghij");
    feed("\33[1;1;1;3;1;3;10;1$v");
    ASSERT_STREQ(row(2), "         abc");
    ASSERT_EQ(mode0_color(ink(9, 2)->fg_color), color_256[1]);
    ASSERT_EQ(cell(11, 2)->attributes, TERM_ATTR_UL(TERM_UL_SINGLE));
    ASSERT_STREQ(row(0), "abcde");
    /* Onto itself, a column right and then a row down. */
    feed("\33[1;1;1;5;1;1;2;1$v");
    ASSERT_STREQ(row(0), "aabcde");
    ASSERT_EQ(mode0_color(ink(1, 0)->fg_color), color_256[1]);
    feed("\33[1;1;2;6;1;2;1;1$v");
    ASSERT_STREQ(row(0), "aabcde");
    ASSERT_STREQ(row(1), "aabcde");
    ASSERT_STREQ(row(2), "fghij    abc");
    /* A column left and a row up. */
    feed("\33[2;2;3;12;1;1;1;1$v");
    ASSERT_STREQ(row(0), "abcde");
    ASSERT_STREQ(row(1), "ghij    abc");
    /* Past the edges. */
    feed("\33[1;1;2;5;1;30;79;1$v");
    ASSERT_EQ(ch(78, 29), 'a');
    ASSERT_EQ(ch(79, 29), 'b');
    ASSERT_STREQ(row(0), "abcde");
}

/* DECCARA sets and clears bold, underline, blink and reverse on cells
 * already written, and leaves them as the pen would have written them. */
UTEST(term, deccara_changes_the_attributes_of_cells)
{
    fresh();
    const uint16_t fg = MODE0_ENTRY(MODE0_DEFAULT_FG, false);
    const uint16_t bg = MODE0_ENTRY(MODE0_DEFAULT_BG, false);
    feed("\33[2*xhello\33[1;1;1;4;1;4;5;7$r");
    ASSERT_EQ(cell(0, 0)->attributes,
              TERM_ATTR_UL(TERM_UL_SINGLE) | TERM_ATTR_BLINK | MODE0_ATTR_BOLD |
                  MODE0_ATTR_BOLD_DEFAULT | MODE0_ATTR_REVERSE);
    ASSERT_EQ(ink(0, 0)->fg_color, bg);
    ASSERT_EQ(ink(0, 0)->bg_color, MODE0_ENTRY(15, false));
    ASSERT_EQ(ink(0, 0)->ul_color, bg);
    ASSERT_EQ(cell(4, 0)->attributes, 0);
    /* The same cells as the pen writes them. */
    feed("\33[2;1H\33[1;4;5;7mhell\33[0m");
    ASSERT_EQ(cell(0, 1)->attributes, cell(0, 0)->attributes);
    ASSERT_EQ(ink(0, 1)->fg_color, ink(0, 0)->fg_color);
    ASSERT_EQ(ink(0, 1)->bg_color, ink(0, 0)->bg_color);
    /* One at a time off, on either row. */
    feed("\33[1;1;2;4;27$r");
    ASSERT_EQ(ink(1, 1)->fg_color, MODE0_ENTRY(15, false));
    ASSERT_EQ(ink(1, 1)->bg_color, bg);
    feed("\33[1;1;2;4;22;24$r");
    ASSERT_EQ(ink(1, 1)->fg_color, fg);
    ASSERT_EQ(cell(1, 1)->attributes, TERM_ATTR_BLINK);
    feed("\33[1;1;2;4;25$r");
    ASSERT_EQ(cell(1, 0)->attributes, 0);
    ASSERT_EQ(cell(1, 1)->attributes, 0);
    /* 0 clears the four, and no parameter is 0. */
    feed("\33[1;1;1;4;1;7$r\33[1;1;1;2;0$r\33[1;3;1;4$r");
    for (int x = 0; x < 4; x++)
    {
        ASSERT_EQ(cell(x, 0)->attributes, 0);
        ASSERT_EQ(ink(x, 0)->fg_color, fg);
        ASSERT_EQ(ink(x, 0)->bg_color, bg);
    }
}

/* Bold brightens the ink a palette color has a bright one for, and taking
 * bold off leaves a color that was bright without it. */
UTEST(term, deccara_bold_is_the_pens_bold)
{
    fresh();
    feed("\33[2*x\33[31mA\33[91mB\33[38;5;100mC\33[38;2;1;2;3mD\33[31;2mE\33[0;8;34mF\33[0m");
    feed("\33[1;1;1;6;1$r");
    ASSERT_EQ(ink(0, 0)->fg_color, MODE0_ENTRY(9, false));
    ASSERT_EQ(ink(1, 0)->fg_color, MODE0_ENTRY(9, false));
    ASSERT_EQ(ink(2, 0)->fg_color, MODE0_ENTRY(100, false));
    ASSERT_EQ(mode0_color(ink(3, 0)->fg_color), rgb(1, 2, 3));
    ASSERT_EQ(ink(4, 0)->fg_color, MODE0_ENTRY(9, true));
    ASSERT_EQ(ink(5, 0)->fg_color, ink(5, 0)->bg_color); /* concealed still */
    feed("\33[1;1;1;6;22$r");
    ASSERT_EQ(ink(0, 0)->fg_color, MODE0_ENTRY(1, false));
    ASSERT_EQ(ink(1, 0)->fg_color, MODE0_ENTRY(9, false));
    ASSERT_EQ(ink(4, 0)->fg_color, MODE0_ENTRY(1, true));
    /* The ink of a reversed cell is its background. */
    feed("\33[2;1H\33[7;32mG\33[0m\33[2;1;2;1;1$r");
    ASSERT_EQ(ink(0, 1)->bg_color, MODE0_ENTRY(10, false));
    feed("\33[2;1;2;1;0$r");
    ASSERT_EQ(ink(0, 1)->fg_color, MODE0_ENTRY(2, false));
    ASSERT_EQ(cell(0, 1)->attributes, 0);
}

/* An underline DECCARA takes off takes its color with it, and blink is not
 * set in iCE colors mode, where no cell blinks. */
UTEST(term, deccara_underline_and_blink)
{
    fresh();
    feed("\33[2*x\33[4:3;58;5;2mA\33[0mB\33[1;1;1;2;24$r\33[1;1;1;2;4$r");
    ASSERT_EQ(cell(0, 0)->attributes, TERM_ATTR_UL(TERM_UL_SINGLE));
    ASSERT_EQ(cell(1, 0)->attributes, TERM_ATTR_UL(TERM_UL_SINGLE));
    ASSERT_EQ(ink(0, 0)->ul_color, ink(0, 0)->fg_color);
    feed("\33[1;1;1;2;4$r");
    ASSERT_EQ(cell(0, 0)->attributes, TERM_ATTR_UL(TERM_UL_SINGLE));
    feed("\33[?33h\33[1;1;1;2;5$r");
    ASSERT_EQ(cell(0, 0)->attributes & TERM_ATTR_ANY_BLINK, 0);
}

/* DECRARA reverses the attributes, and twice is as it was. */
UTEST(term, decrara_reverses_the_attributes)
{
    fresh();
    feed("\33[2*x\33[1mA\33[0mB\33[1;1;1;2;1;7$t");
    ASSERT_EQ(cell(0, 0)->attributes, MODE0_ATTR_REVERSE);
    ASSERT_EQ(ink(0, 0)->bg_color, MODE0_ENTRY(MODE0_DEFAULT_FG, false));
    ASSERT_EQ(cell(1, 0)->attributes,
              MODE0_ATTR_BOLD | MODE0_ATTR_BOLD_DEFAULT | MODE0_ATTR_REVERSE);
    ASSERT_EQ(ink(1, 0)->bg_color, MODE0_ENTRY(15, false));
    feed("\33[1;1;1;2;1;7$t");
    ASSERT_EQ(cell(0, 0)->attributes, MODE0_ATTR_BOLD | MODE0_ATTR_BOLD_DEFAULT);
    ASSERT_EQ(cell(1, 0)->attributes, 0);
    ASSERT_EQ(ink(1, 0)->fg_color, MODE0_ENTRY(MODE0_DEFAULT_FG, false));
    /* 0 is all four. */
    feed("\33[1;2;1;2;0$t");
    ASSERT_EQ(cell(1, 0)->attributes,
              TERM_ATTR_UL(TERM_UL_SINGLE) | TERM_ATTR_BLINK | MODE0_ATTR_BOLD |
                  MODE0_ATTR_BOLD_DEFAULT | MODE0_ATTR_REVERSE);
    feed("\33[1;2;1;2$t");
    ASSERT_EQ(cell(1, 0)->attributes, 0);
}

/* Without DECSACE 2 the attributes change in the stream of cells from the
 * first corner to the last. A reset goes back to that. */
UTEST(term, decsace_chooses_the_stream_or_the_rectangle)
{
    fresh();
    feed("\33[1;3;3;5;4$r");
    ASSERT_EQ(cell(1, 0)->attributes, 0);
    ASSERT_NE(cell(2, 0)->attributes, 0);
    ASSERT_NE(cell(79, 0)->attributes, 0);
    ASSERT_NE(cell(0, 1)->attributes, 0);
    ASSERT_NE(cell(79, 1)->attributes, 0);
    ASSERT_NE(cell(4, 2)->attributes, 0);
    ASSERT_EQ(cell(5, 2)->attributes, 0);
    feed("\33[2*x\33[5;3;7;5;4$r");
    ASSERT_EQ(cell(1, 5)->attributes, 0);
    ASSERT_NE(cell(2, 5)->attributes, 0);
    ASSERT_NE(cell(4, 5)->attributes, 0);
    ASSERT_EQ(cell(5, 5)->attributes, 0);
    feed("\33c\33[1;3;2;5;4$r");
    ASSERT_NE(cell(79, 0)->attributes, 0);
}

/* Under DECOM a rectangle counts from the margins and stays inside them. */
UTEST(term, a_rectangle_counts_from_the_margins_under_decom)
{
    fresh();
    feed("\33[?69h\33[11;20s\33[5;9r\33[?6h\33[88;1;1;99;99$x");
    ASSERT_EQ(ch(9, 4), ' ');
    ASSERT_EQ(ch(10, 4), 'X');
    ASSERT_EQ(ch(19, 8), 'X');
    ASSERT_EQ(ch(20, 8), ' ');
    ASSERT_EQ(ch(10, 3), ' ');
    ASSERT_EQ(ch(10, 9), ' ');
    feed("\33[2;2;2;3$z");
    ASSERT_EQ(ch(10, 5), 'X');
    ASSERT_EQ(ch(11, 5), ' ');
    ASSERT_EQ(ch(12, 5), ' ');
    ASSERT_EQ(ch(13, 5), 'X');
}

/* A double-size line has only its own columns of a rectangle. */
UTEST(term, a_rectangle_keeps_to_a_double_size_lines_columns)
{
    fresh();
    feed("\33[2;1H\33#6\33[88;1;1;3;80$x\33[2;1H\33#5");
    ASSERT_EQ(ch(79, 0), 'X');
    ASSERT_EQ(ch(39, 1), 'X');
    ASSERT_EQ(ch(40, 1), ' ');
    ASSERT_EQ(ch(79, 2), 'X');
}

/* A copy inside a row, to the left and to the right, from odd and even
 * columns and of odd and even counts, moves every cell with its colors. */
UTEST(term, deccra_moves_cells_and_colors_inside_a_row)
{
    static const struct
    {
        int from, to, n;
    } moves[] = {{1, 3, 8}, {3, 1, 8}, {1, 3, 7}, {3, 1, 7}, {2, 3, 8}, {3, 2, 9},
                 {0, 1, 20}, {1, 0, 21}, {5, 11, 12}, {11, 5, 13}, {1, 41, 30}, {41, 1, 31}};
    fresh();
    char want[80];
    uint8_t color[80];
    for (int x = 0; x < 80; x++)
    {
        char cell_seq[32];
        want[x] = (char)('A' + x % 26);
        color[x] = (uint8_t)(16 + x);
        snprintf(cell_seq, sizeof cell_seq, "\33[38;5;%d;48;5;%dm%c", color[x], color[x] + 100,
                 want[x]);
        feed(cell_seq);
    }
    for (size_t m = 0; m < sizeof moves / sizeof moves[0]; m++)
    {
        char seq[48];
        snprintf(seq, sizeof seq, "\33[1;%d;1;%d;1;1;%d;1$v", moves[m].from + 1,
                 moves[m].from + moves[m].n, moves[m].to + 1);
        feed(seq);
        memmove(want + moves[m].to, want + moves[m].from, (size_t)moves[m].n);
        memmove(color + moves[m].to, color + moves[m].from, (size_t)moves[m].n);
        for (int x = 0; x < 80; x++)
        {
            ASSERT_EQ(ch(x, 0), want[x]);
            ASSERT_EQ(ink(x, 0)->fg_color, MODE0_ENTRY(color[x], false));
            ASSERT_EQ(ink(x, 0)->bg_color, MODE0_ENTRY(color[x] + 100, false));
        }
    }
}

/* A scroll between margins is the same scroll whether the cells between
 * them are copied or the rows move and the cells either side are put back:
 * every width of margins, both ways, by every count. */
UTEST(term, a_scroll_between_margins_is_right_at_every_width)
{
    static const int margins[][2] = {{2, 79}, {1, 79}, {2, 80}, {21, 60}, {30, 80}, {1, 40},
                                     {10, 12}, {5, 76}, {40, 41}};
    static char want[30][80];
    for (size_t m = 0; m < sizeof margins / sizeof margins[0]; m++)
        for (int n = 1; n <= 9; n += n < 3 ? 1 : 3)
            for (int way = 0; way < 4; way++)
            {
                /* All at once, or a row at a time, which adds up. */
                const int up = way & 1, single = way & 2;
                const int left = margins[m][0] - 1, right = margins[m][1] - 1;
                const int top = 2, bot = 11, h = bot - top + 1;
                fresh();
                for (int y = 0; y < 14; y++)
                {
                    char line[100];
                    for (int x = 0; x < 80; x++)
                        want[y][x] = line[x] = (char)(33 + (y * 17 + x * 5) % 90);
                    snprintf(line + 80, sizeof line - 80, "%s", y < 13 ? "\33[?7l\r\n" : "");
                    feed("\33[?7h");
                    feedn(line, 80);
                    feed(line + 80);
                }
                char seq[64];
                snprintf(seq, sizeof seq, "\33[?69h\33[%d;%dr\33[%d;%ds\33[%d;%dH", top + 1,
                         bot + 1, left + 1, right + 1, top + 1, left + 1);
                feed(seq);
                snprintf(seq, sizeof seq, "\33[%d%c", single ? 1 : n, up ? 'S' : 'T');
                for (int i = 0; i < (single ? n : 1); i++)
                    feed(seq);
                const int k = n < h ? n : h;
                for (int x = left; x <= right; x++)
                {
                    char col[30];
                    for (int y = top; y <= bot; y++)
                        col[y] = want[y][x];
                    for (int y = top; y <= bot; y++)
                    {
                        const int from = up ? y + k : y - k;
                        want[y][x] = from >= top && from <= bot ? col[from] : ' ';
                    }
                }
                for (int y = 0; y < 14; y++)
                    for (int x = 0; x < 80; x++)
                        ASSERT_EQ(ch(x, y), want[y][x]);
            }
}

/* A change of attributes a row is owed is saved with the row. */
UTEST(term, attributes_still_owed_come_back)
{
    static uint8_t blob[1 << 20];
    fresh();
    feed("\33[3;1HABCDEF\33[10;1H\33[2*x\33[3;2;3;4;4;7$r");
    ASSERT_EQ(sst_save(blob, sizeof blob, 0), (const char *)NULL);
    feed("\33c");
    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_EQ(cell(0, 2)->attributes, 0);
    ASSERT_EQ(cell(1, 2)->attributes, TERM_ATTR_UL(TERM_UL_SINGLE) | MODE0_ATTR_REVERSE);
    ASSERT_EQ(cell(3, 2)->attributes, TERM_ATTR_UL(TERM_UL_SINGLE) | MODE0_ATTR_REVERSE);
    ASSERT_EQ(cell(4, 2)->attributes, 0);
}

/* What a row is owed is paid in order: a change of attributes, then a
 * shift, a selective erase or an erase over it. */
UTEST(term, attributes_owed_are_paid_before_what_comes_after)
{
    fresh();
    feed("\33[3;1HABCDEF\33[4;1H\33[1\"qGH\33[0\"qIJ\33[5;1HKLMN\33[10;1H\33[2*x");
    feed("\33[3;1;5;6;7$r\33[2 A\33[4;1;4;80${\33[5;1;5;80$z\33[5;1;5;2;4$r");
    ASSERT_STREQ(row(2), "  ABCDEF");
    ASSERT_EQ(cell(1, 2)->attributes, 0);
    ASSERT_EQ(cell(2, 2)->attributes, MODE0_ATTR_REVERSE);
    ASSERT_STREQ(row(3), "  GH");
    ASSERT_EQ(cell(2, 3)->attributes, MODE0_ATTR_REVERSE | MODE0_ATTR_GUARD);
    ASSERT_STREQ(row(4), "");
    ASSERT_EQ(cell(0, 4)->attributes, TERM_ATTR_UL(TERM_UL_SINGLE));
    ASSERT_EQ(cell(2, 4)->attributes, 0);
}

/* REP between margins draws what printing draws, wherever the cursor starts
 * and however far the region scrolls under it. */
UTEST(term, rep_between_margins_draws_what_printing_draws)
{
    static const char *const cases[] = {
        "\33[?69h\33[21;60s\33[1;21H",
        "\33[?69h\33[21;60s\33[30;21H",
        "\33[?69h\33[2;79s\33[5;25r\33[20;30H",
        "\33[?69h\33[21;60s\33[5;25r\33[3;30H",
        "\33[?69h\33[21;60s\33[5;25r\33[28;30H",
        "\33[?69h\33[21;60s\33[5;25r\33[20;5H",
        "\33[?69h\33[21;60s\33[5;25r\33[25;70H",
        "\33[?69h\33[21;60s\33[?7l\33[10;30H",
        "\33[?69h\33[30;31s\33[29;30H",
        "\33[?69h\33[1;79s\33[44;4m\33[30;1H",
    };
    static const int counts[] = {1, 2, 39, 40, 41, 79, 80, 81, 200, 1199, 1200, 1201, 2400, 5000};
    static uint8_t a[30 * 80 * 10], b[30 * 80 * 10];
    static char before[30 * 90 + 80];
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++)
    {
        int at = 0;
        for (int y = 0; y < 30; y++)
        {
            at += snprintf(before + at, sizeof before - (size_t)at, "\33[%d;1H", y + 1);
            for (int x = 0; x < 79; x++)
                before[at++] = (char)(33 + (y * 13 + x * 3) % 90);
        }
        snprintf(before + at, sizeof before - (size_t)at, "%s", cases[i]);
        for (unsigned k = 0; k < sizeof counts / sizeof counts[0]; k++)
        {
            int ax, ay, bx, by;
            rep_screen(before, counts[k], true, a, &ax, &ay);
            rep_screen(before, counts[k], false, b, &bx, &by);
            if (memcmp(a, b, sizeof a) || ax != bx || ay != by)
                printf("REP %d after case %u differs\n", counts[k], i);
            ASSERT_EQ(0, memcmp(a, b, sizeof a));
            ASSERT_EQ(ax, bx);
            ASSERT_EQ(ay, by);
        }
    }
}

/* Line feeds between margins add up to one scroll, and whatever comes
 * between them is done in its turn: another pen, the other screen, a
 * savestate. */
UTEST(term, line_feeds_between_margins_add_up)
{
    static uint8_t blob[1 << 20];
    fresh();
    lettered(6);
    feed("\33[?69h\33[2;5r\33[4;7s\33[5;5H\n\n");
    ASSERT_STREQ(row(1), "bbbddddbbb");
    ASSERT_STREQ(row(2), "ccceeeeccc");
    ASSERT_STREQ(row(3), "ddd    ddd");
    ASSERT_STREQ(row(4), "eee    eee");
    feed("\33[44m\n\33[45m\n\33[0m");
    ASSERT_STREQ(row(1), "bbb    bbb");
    ASSERT_EQ(mode0_color(ink(3, 2)->bg_color), color_256[0]);
    ASSERT_EQ(mode0_color(ink(3, 3)->bg_color), color_256[4]);
    ASSERT_EQ(mode0_color(ink(3, 4)->bg_color), color_256[5]);
    ASSERT_EQ(mode0_color(ink(2, 4)->bg_color), color_256[0]);
    /* A glyph lands on the row the scroll brought in. */
    feed("\33[2;5Hxy\33[5;5H\nz");
    ASSERT_STREQ(row(1), "bbb    bbb");
    ASSERT_STREQ(row(4), "eee z  eee");
    /* The other screen, with a scroll still owed to this one. */
    feed("\33[2;4Hpq\33[5;5H\n\33[?47h\33[?69h\33[2;5r\33[4;7s\33[5;5H\nalt\33[?47l");
    ASSERT_STREQ(row(1), "bbb    bbb");
    ASSERT_STREQ(row(3), "ddd z  ddd");
    /* A savestate with a scroll owed. */
    feed("\33[2;4Hrs\33[5;5H\n");
    ASSERT_EQ(sst_save(blob, sizeof blob, 0), (const char *)NULL);
    feed("\33c");
    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_STREQ(row(1), "bbb    bbb");
    ASSERT_STREQ(row(2), "ccc z  ccc");
    ASSERT_STREQ(row(0), "aaaaaaaaaa");
}

/* ---- xterm's key options, and columns -------------------------------------- */

/* A program sets modifyOtherKeys and formatOtherKeys and reads them back.
 * No level, or no option, is the reset one, and a reset turns them off. */
UTEST(term, xterm_key_options_set_and_report)
{
    fresh();
    ASSERT_STREQ(ask("\33[?4m\33[?4g"), "\33[>4;0m\33[>4;0f");
    ASSERT_STREQ(ask("\33[>4;2m\33[?4m"), "\33[>4;2m");
    ASSERT_STREQ(ask("\33[>4;3m\33[?4m"), "\33[>4;3m");
    ASSERT_STREQ(ask("\33[>4;9m\33[?4m"), "\33[>4;3m");
    ASSERT_STREQ(ask("\33[>4m\33[?4m"), "\33[>4;0m");
    ASSERT_STREQ(ask("\33[>4;1m\33[>4;1f\33[?4m\33[?4g"), "\33[>4;1m\33[>4;1f");
    ASSERT_STREQ(ask("\33[>1;2m\33[>2;0f\33[?4m\33[?4g"), "\33[>4;1m\33[>4;1f");
    ASSERT_STREQ(ask("\33[>4n\33[?4m"), "\33[>4;0m");
    ASSERT_STREQ(ask("\33[>4;2m\33[>m\33[>f\33[?4m\33[?4g"), "\33[>4;0m\33[>4;0f");
    ASSERT_STREQ(ask("\33[>4;2m\33[>4;1f\33c\33[?4m\33[?4g"), "\33[>4;0m\33[>4;0f");
    /* The options of the other keys are not the console's to set or tell. */
    ASSERT_STREQ(ask("\33[?1m\33[?2g"), "");
    /* The secondary device attributes are still asked with the same marker. */
    ASSERT_STREQ(ask("\33[>c"), "\33[>0;1;0c");
    /* The end of a program turns them off. */
    feed("\33[>4;2m\33[>4;1f");
    feed(STR_TERM_SOFT_RESET);
    ASSERT_STREQ(ask("\33[?4m\33[?4g"), "\33[>4;0m\33[>4;0f");
}

/* DECIC and DECDC insert and delete columns at the cursor, on every line of
 * the scroll region, up to the right margin. */
UTEST(term, decic_and_decdc_insert_and_delete_columns)
{
    fresh();
    feed("0123456789\r\n0123456789\r\n0123456789\33[1;2r\33[1;4H\33[2'}");
    ASSERT_STREQ(row(0), "012  3456789");
    ASSERT_STREQ(row(1), "012  3456789");
    ASSERT_STREQ(row(2), "0123456789"); /* outside the region */
    ASSERT_EQ(cx(), 3);
    feed("\33['~");
    ASSERT_STREQ(row(0), "012 3456789");
    feed("\33[3;1H\33['}");
    ASSERT_STREQ(row(2), "0123456789"); /* the cursor is outside the region */
    feed("\33[r\33[?69h\33[4;7s\33[1;5H\33['}");
    ASSERT_STREQ(row(0), "012  346789");
    ASSERT_STREQ(row(2), "0123 45789");
    feed("\33[2'~");
    ASSERT_STREQ(row(2), "01235  789");
    feed("\33[1;2H\33[5'}");
    ASSERT_STREQ(row(2), "01235  789"); /* the cursor is outside the margins */
}

/* DECBI and DECFI move the cursor a column, and at a margin move the columns
 * between the margins the other way instead. */
UTEST(term, decbi_and_decfi_index_columns)
{
    fresh();
    feed("0123456789\r\n0123456789\33[1;5H\0336");
    ASSERT_EQ(cx(), 3);
    feed("\0339\0339");
    ASSERT_EQ(cx(), 5);
    ASSERT_STREQ(row(0), "0123456789");
    feed("\33[1;1H\0336");
    ASSERT_STREQ(row(0), " 0123456789");
    ASSERT_STREQ(row(1), " 0123456789");
    ASSERT_EQ(cx(), 0);
    feed("\33[1;80H\0339");
    ASSERT_STREQ(row(0), "0123456789");
    ASSERT_EQ(cx(), 79);
    feed("\33[?69h\33[4;7s\33[1;4H\0336");
    ASSERT_STREQ(row(0), "012 345789");
    feed("\33[1;7H\0339");
    ASSERT_STREQ(row(0), "012345 789");
    ASSERT_EQ(cx(), 6);
    /* Outside the margins the cursor only moves. */
    feed("\33[1;1H\0336\33[1;9H\0339");
    ASSERT_STREQ(row(0), "012345 789");
    ASSERT_EQ(cx(), 9);
}

/* ---- what a program asks ------------------------------------------------- */

/* DECRQSS for SGR answers with the pen: a 0, the attributes, then the
 * colors, with an underline style and an RGB color in the colon forms. */
UTEST(term, decrqss_answers_with_the_pen)
{
    fresh();
    ASSERT_STREQ(ask("\33P$qm\33\\"), "\33P1$r0m\33\\");
    ASSERT_STREQ(ask("\33[4m\33P$qm\33\\"), "\33P1$r0;4m\33\\");
    ASSERT_STREQ(ask("\33[0;4:3m\33P$qm\33\\"), "\33P1$r0;4:3m\33\\");
    ASSERT_STREQ(ask("\33[0;21m\33P$qm\33\\"), "\33P1$r0;4:2m\33\\");
    ASSERT_STREQ(ask("\33[0;31;44m\33P$qm\33\\"), "\33P1$r0;31;44m\33\\");
    ASSERT_STREQ(ask("\33[0;92;105m\33P$qm\33\\"), "\33P1$r0;92;105m\33\\");
    ASSERT_STREQ(ask("\33[0;38;5;100;48;5;200;4;58;5;9m\33P$qm\33\\"),
                 "\33P1$r0;4;38:5:100;48:5:200;58:5:9m\33\\");
    ASSERT_STREQ(ask("\33[0;38;2;10;20;30m\33P$qm\33\\"), "\33P1$r0;38:2::10:20:30m\33\\");
    ASSERT_STREQ(ask("\33[0;48:2:1:2:3m\33P$qm\33\\"), "\33P1$r0;48:2::1:2:3m\33\\");
    ASSERT_STREQ(ask("\33[0;48:2::4:5:6m\33P$qm\33\\"), "\33P1$r0;48:2::4:5:6m\33\\");
    ASSERT_STREQ(ask("\33[0;6;7m\33P$qm\33\\"), "\33P1$r0;6;7m\33\\");
    /* Everything at once is the longest answer there is. */
    ASSERT_STREQ(ask("\33[0;1;2;3;4:5;5;7;8;9;53m\33[38:2::255:255:255m"
                     "\33[48:2::255:255:255m\33[58:2::255:255:255m\33P$qm\33\\"),
                 "\33P1$r0;1;2;3;4:5;5;7;8;9;53;38:2::255:255:255;48:2::255:255:255"
                 ";58:2::255:255:255m\33\\");
    /* The saved pen keeps its colors as they were given. */
    ASSERT_STREQ(ask("\33[0;38;2;9;8;7m\0337\33[0m\0338\33P$qm\33\\"),
                 "\33P1$r0;38:2::9:8:7m\33\\");
}

/* The other settings DECRQSS gives, and its answer to one it has not. */
UTEST(term, decrqss_answers_with_the_other_settings)
{
    fresh();
    ASSERT_STREQ(ask("\33P$qr\33\\"), "\33P1$r1;30r\33\\");
    ASSERT_STREQ(ask("\33[5;20r\33P$qr\33\\"), "\33P1$r5;20r\33\\");
    ASSERT_STREQ(ask("\33P$qs\33\\"), "\33P1$r1;80s\33\\");
    ASSERT_STREQ(ask("\33[?69h\33[11;70s\33P$qs\33\\"), "\33P1$r11;70s\33\\");
    ASSERT_STREQ(ask("\33P$q q\33\\"), "\33P1$r1 q\33\\");
    ASSERT_STREQ(ask("\33[4 q\33P$q q\33\\"), "\33P1$r4 q\33\\");
    ASSERT_STREQ(ask("\33P$q\"q\33\\"), "\33P1$r0\"q\33\\");
    ASSERT_STREQ(ask("\33[1\"q\33P$q\"q\33\\"), "\33P1$r1\"q\33\\");
    ASSERT_STREQ(ask("\33P$q*x\33\\"), "\33P1$r0*x\33\\");
    ASSERT_STREQ(ask("\33[2*x\33P$q*x\33\\"), "\33P1$r2*x\33\\");
    ASSERT_STREQ(ask("\33P$q\"p\33\\"), "\33P0$r\33\\");
    ASSERT_STREQ(ask("\33P$qz\33\\"), "\33P0$r\33\\");
}

/* Any other DCS string is dropped to its ST, and so is one cut short. */
UTEST(term, a_dcs_string_that_is_not_decrqss_is_dropped)
{
    fresh();
    ASSERT_STREQ(ask("\33P+q5463;524742;73657472676266;73657472676262\33\\a"), "");
    ASSERT_STREQ(ask("\33P+q4d73\33\\b"), "");
    ASSERT_STREQ(ask("\33P$q\33\\c"), "\33P0$r\33\\");
    ASSERT_STREQ(ask("\33P$qm\30" "d"), "");
    ASSERT_STREQ(ask("\33P$qm\33[1m" "e"), "");
    ASSERT_STREQ(ask("\33P$qmmmmmmmmmmmmmmmmmmmmmmmm\33\\f"), "\33P0$r\33\\");
    ASSERT_STREQ(ask("\33P+qmmmmmmmmmmmmmmmmmmmmmmmm\33\\"), "");
    ASSERT_STREQ(row(0), "abcdef");
    /* A string split across a savestate is still answered. */
    static uint8_t blob[1 << 20];
    feed("\33P$qr\33");
    ASSERT_EQ(sst_save(blob, sizeof blob, 0), (const char *)NULL);
    feed("\33c");
    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_STREQ(ask("\\"), "\33P1$r1;30r\33\\");
}

/* The color a channel of five bits is, as four hex digits. */
static unsigned hex4(unsigned five)
{
    return five * 0xFFFFu / 31u;
}

static const char *color_answer(const char *head, uint16_t pixel, const char *end)
{
    static char want[64];
    snprintf(want, sizeof want, "%s;rgb:%04x/%04x/%04x%s", head,
             hex4(SCANVIDEO_R5_FROM_PIXEL(pixel)), hex4(SCANVIDEO_G5_FROM_PIXEL(pixel)),
             hex4(SCANVIDEO_B5_FROM_PIXEL(pixel)), end);
    return want;
}

/* A "?" for a color's spec asks what the color is. The answer is the OSC
 * that would set it, and ends as the question did. */
UTEST(term, osc_colors_can_be_asked_for)
{
    fresh();
    ASSERT_STREQ(ask("\33]11;?\7"), "\33]11;rgb:0000/0000/0000\7");
    ASSERT_STREQ(ask("\33]10;?\33\\"), color_answer("\33]10", color_256[7], "\33\\"));
    ASSERT_STREQ(ask("\33]12;?\7"), color_answer("\33]12", color_256[7], "\7"));
    ASSERT_STREQ(ask("\33]4;9;?\7"), color_answer("\33]4;9", color_256[9], "\7"));
    ASSERT_STREQ(ask("\33]4;255;?\33\\"), "\33]4;255;rgb:ef7a/ef7a/ef7a\33\\");
    /* A color a program set is the color it gets back, to five bits. */
    ASSERT_STREQ(ask("\33]4;1;#ff0000\7\33]4;1;?\7"), "\33]4;1;rgb:ffff/0000/0000\7");
    ASSERT_STREQ(ask("\33]11;#00ff00\7\33]11;?\7"), "\33]11;rgb:0000/ffff/0000\7");
    ASSERT_STREQ(ask("\33]12;#0000ff\7\33]12;?\7"), "\33]12;rgb:0000/0000/ffff\7");
    /* Each item of OSC 10 is the next color, asked or set. */
    ASSERT_STREQ(ask("\33]10;#ff0000;?;?\7"),
                 "\33]11;rgb:0000/ffff/0000\7\33]12;rgb:0000/0000/ffff\7");
    ASSERT_STREQ(ask("\33]10;?\7"), "\33]10;rgb:ffff/0000/0000\7");
    /* Two entries of the palette in one OSC 4, each as it was when asked
     * for, and every answer ends as the OSC did. */
    ASSERT_STREQ(ask("\33]4;1;?;1;#00ff00;2;#0000ff\33\\\33]4;2;?;1;?\7"),
                 "\33]4;1;rgb:ffff/0000/0000\33\\"
                 "\33]4;2;rgb:0000/0000/ffff\7\33]4;1;rgb:0000/ffff/0000\7");
    /* An OSC cut short answers nothing. */
    ASSERT_STREQ(ask("\33]4;1;?;2;?\30\33]4;2;?\7"), "\33]4;2;rgb:0000/0000/ffff\7");
    /* No entry 256, and a reset gives the first colors back. */
    ASSERT_STREQ(ask("\33]4;256;?\7"), "");
    ASSERT_STREQ(ask("\33c\33]4;1;?\7"), color_answer("\33]4;1", color_256[1], "\7"));
}

/* Everything Neovim asks as it starts, in one burst with nothing read
 * until the end: every answer is there, in order, and the screen, the
 * cursor and the pen's underline are as a program expects. */
UTEST(term, what_neovim_asks_as_it_starts_is_all_answered)
{
    fresh();
    feed("\33[?69$p\33[?2026$p\33[?2027$p\33[?2031$p\33[?2048$p"
         "\33[0m\33[4:3m\33P$qm\33\\"
         "\33[?u\33[c"
         "\33]11;?\7\33[5n"
         "\33]11;?\7\33[5n"
         "\33P+q5463;524742;73657472676266;73657472676262\33\\"
         "\33[0m\33[48;2;1;2;3m\33P$qm\33\\"
         "\33[c\33P+q4d73\33\\"
         "\33[> q"
         "\33[>4;2m\33[0m");
    ASSERT_STREQ(answer(),
                 "\33[?69;2$y\33[?2026;0$y\33[?2027;0$y\33[?2031;0$y\33[?2048;0$y"
                 "\33P1$r0;4:3m\33\\"
                 "\33[?6c"
                 "\33]11;rgb:0000/0000/0000\7\33[0n"
                 "\33]11;rgb:0000/0000/0000\7\33[0n"
                 "\33P1$r0;48:2::1:2:3m\33\\"
                 "\33[?6c");
    ASSERT_STREQ(row(0), "");
    ASSERT_EQ(cx(), 0);
    ASSERT_EQ(cy(), 0);
    ASSERT_STREQ(ask("\33P$qm\33\\"), "\33P1$r0m\33\\");
    /* And as it leaves. */
    ASSERT_STREQ(ask("\33[>4;0m\33[c"), "\33[?6c");
}

/* ---- smooth scroll ---- */

/* The row a smooth scroll is moving out, as text, trailing spaces dropped. */
static const char *ghost(void)
{
    static char text[96];
    const mode0_cell_t *c = &mode0_cells[MODE0_ROW_CELL(mode0_regs.smooth_ghost)];
    int last = 0;
    for (int x = 0; x < 80; x++)
    {
        text[x] = (char)mode0_glyph_code(c[x].glyph);
        if (text[x] != ' ')
            last = x + 1;
    }
    text[last] = 0;
    return text;
}

UTEST(term, decsclm_is_reported)
{
    fresh();
    ASSERT_STREQ(ask("\33[?4$p"), "\33[?4;2$y");
    ASSERT_STREQ(ask("\33[?4h\33[?4$p"), "\33[?4;1$y");
}

/* The row going out still shows while it goes, so an erase it was owed is
 * paid first: a whole-screen scroll after ED 2 moves out a blank row. */
UTEST(term, a_smooth_scroll_moves_out_the_row_as_it_is)
{
    fresh();
    feed("\33[HXXXX\33[?4h\33[2J\33[30;1H\n");
    ASSERT_TRUE(mode0_regs.smooth & MODE0_SMOOTH_ON);
    ASSERT_STREQ(ghost(), "");
}

/* The same for a reverse index in a region, whose bottom row goes out. */
UTEST(term, a_smooth_reverse_index_moves_out_the_bottom_row_as_it_is)
{
    fresh();
    feed("\33[10;1HYYYY\33[?4h\33[5;10r\33[5;1H\33[J\33M");
    ASSERT_FALSE(mode0_regs.smooth & MODE0_SMOOTH_UP);
    ASSERT_STREQ(ghost(), "");
    feed("\33[r");
}

/* A line feed that would scroll the moving rows again waits for them to
 * land, which takes TERM_SMOOTH_FRAMES frames. Text goes on into the row
 * coming in. */
UTEST(term, a_smooth_scroll_holds_the_next_line_feed)
{
    fresh();
    feed("\33[?4h\33[30;1H\n");
    ASSERT_TRUE(term_holds(&cells_term, '\n'));
    ASSERT_FALSE(term_holds(&cells_term, 'A'));
    int frames = 0;
    while (term_holds(&cells_term, '\n') && frames < 100)
    {
        vga_run_frame();
        frames++;
    }
    ASSERT_EQ(frames, TERM_SMOOTH_FRAMES);
}

/* A line feed that has to wait, behind one that is scrolling: what the
 * console's output holds while a smooth scroll moves. */
static bool held_line_feed(void)
{
    fresh();
    feed("\33[?4h\33[30;1H\n\n");
    return !manifold_drained();
}

/* A sync of the console returns once both lines have arrived. */
UTEST(term, a_console_sync_waits_for_the_scroll)
{
    ASSERT_TRUE(held_line_feed());
    api_errno err;
    ASSERT_EQ(console_std_sync(1, &err), STD_PENDING);
    int frames = 0;
    while (console_std_sync(1, &err) == STD_PENDING && frames < 100)
    {
        vga_run_frame();
        frames++;
    }
    ASSERT_TRUE(frames > TERM_SMOOTH_FRAMES);
    ASSERT_TRUE(frames <= 2 * TERM_SMOOTH_FRAMES);
    ASSERT_TRUE(manifold_drained());
}

/* The end of a chain waits for no scroll: what was held goes out at once. */
UTEST(term, the_end_of_a_chain_releases_a_held_line)
{
    ASSERT_TRUE(held_line_feed());
    term_release_smooth(&cells_term);
    manifold_task();
    ASSERT_TRUE(manifold_drained());
    /* DECSCLM set again holds again. */
    feed("\33[?4h\n\n");
    ASSERT_TRUE(!manifold_drained());
}

static char rln_got[8];
static void rln_cb(bool timeout, const char *buf)
{
    (void)timeout;
    snprintf(rln_got, sizeof rln_got, "%s", buf);
}

/* A read started while a line is held is laid out once it has arrived, and
 * the keys typed wait for it. */
UTEST(term, a_read_waits_for_the_held_line)
{
    ASSERT_TRUE(held_line_feed());
    rln_read_line(rln_cb, NULL);
    manifold_uart_push("hi\r", 3);
    rln_got[0] = 0;
    int frames = 0;
    while (!rln_got[0] && frames < 100)
    {
        vga_run_frame();
        frames++;
    }
    ASSERT_STREQ(rln_got, "hi");
    ASSERT_TRUE(frames > TERM_SMOOTH_FRAMES);
    ASSERT_EQ(cy(), 29);
}

UTEST_MAIN();
