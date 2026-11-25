/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The console's mouse selecting (core/term/vterm_mouse.h), and the copy and
 * paste keys (core/hid/key.h), from the tablet's host pointer and the host's
 * keys to the selection, the primary and the clipboard. */

#include "core/api/xreg.h"
#include "core/com/rln.h"
#include "core/com/console.h"
#include "core/hid/typed.h"
#include "core/com/com.h"
#include "core/com/manifold.h"
#include "core/hid/clip.h"
#include "tb_seed.h"
#include "core/hid/hostkey.h"
#include "core/hid/keyboard.h"
#include "core/hid/paste_host.h"
#include "core/hid/tablet.h"
#include "core/hid/usage.h"
#include "core/sys/ria.h"
#include "core/sys/sst.h"
#include "core/sys/sys.h"
#include "core/sys/timer.h"
#include "core/sys/xram.h"
#include "core/term/draw.h"
#include "core/term/vterm.h"
#include "core/term/vterm_mouse.h"
#include "core/vga/vga_emu.h"
#include "host/host.h"
#include "tb_screen.h"
#include "utest.h"
#include <string.h>

/* A machine on the console, 80 columns of 30 rows of 16 lines on a 640x480
 * canvas, the pointer gone from it, no click to count from, nothing
 * selected and nothing typed. */
static void start(void)
{
    sys_init();
    manifold_set_paced(false);
    tablet_host_clear();
    vterm_mouse_reset();
    vterm_unselect();
    keyboard_set_key(HID_KEY_SHIFT_LEFT, false);
    com_rx_clear();
}

static void show(const char *s)
{
    manifold_write_to(COM_TERM_SCREEN, s, strlen(s));
}

/* The pointer over column x of row y of the 80-column console, buttons held,
 * taken by the console's mouse at once. */
static void at(int x, int y, uint8_t buttons)
{
    tablet_host_pointer(x * 8 + 3, y * 16 + 7, buttons, true);
    vterm_mouse_task();
}

/* The same on the left pixel of column x: by characters, a press there
 * holds at the gap before the cell going on, and at the gap after it going
 * back, and a drag there ends at the gap before it, the nearest. */
static void edge(int x, int y, uint8_t buttons)
{
    tablet_host_pointer(x * 8, y * 16 + 7, buttons, true);
    vterm_mouse_task();
}

/* A drag with the left button from the left pixel of one cell to the left
 * pixel of another: from the first up to the second, that one not taken,
 * going on; back from the first, that one not taken, to the second. */
static void drag(int x0, int y0, int x1, int y1)
{
    edge(x0, y0, 0);
    edge(x0, y0, TABLET_FLAG_LEFT);
    edge(x1, y1, TABLET_FLAG_LEFT);
    edge(x1, y1, 0);
}

/* A click with the left button, as quick as the mouse can. */
static void click(int x, int y)
{
    edge(x, y, TABLET_FLAG_LEFT);
    edge(x, y, 0);
}

/* The machine's clock moves on by us, which runs the beam. */
static void machine_us(uint64_t us)
{
    timer_mach_t d = timer_mach_in_us(us);
    while (!timer_mach_passed(d))
        vga_task();
}

/* All the keyboard row gives, a paste's text among it. */
static const char *typed(void)
{
    static char out[512];
    out[com_read(COM_SOURCE_KEYBOARD, out, sizeof out - 1)] = 0;
    return out;
}

/* The primary, as a paste of it types it. */
static const char *primary(void)
{
    clip_paste(CLIP_PRIMARY);
    return typed();
}

/* The selection shows from row y0, column x0 up to row y1, column x1 of the
 * screen showing. */
static bool selected(int x0, int y0, int x1, int y1)
{
    const term_grid_t *g = vterm_get_shown(&vterm);
    const screen_selection_t *v = &vterm_selection;
    return v->shows && v->screen == g->shown && v->start_row == screen_mem(g->shown, (uint8_t)y0) &&
           v->start_col == x0 && v->end_row == screen_mem(g->shown, (uint8_t)y1) &&
           v->end_col == x1;
}

/* Row y of the screen showing as text, up to its last character that is
 * not a space. */
static const char *row(int y)
{
    static char out[81];
    const mode0_cell_t *cells = tb_screen_row((uint8_t)y);
    int last = 0;
    for (int x = 0; x < 80; x++)
    {
        out[x] = (char)mode0_glyph_code(cells[x].glyph);
        if (out[x] != ' ')
            last = x + 1;
    }
    out[last] = 0;
    return out;
}

#define TEXT "\33[H\33[2Jhello world foo.bar\r\nsecond line\r\n"

/* ---- characters (step 19) ------------------------------------------------ */

/* A drag selects from the cell it went down on up to the cell it came up
 * on, that one not taken, and the release fills the primary. */
UTEST(mouse, a_drag_selects_and_fills_the_primary)
{
    start();
    show(TEXT);
    drag(6, 0, 11, 0);
    ASSERT_TRUE(selected(6, 0, 11, 0));
    ASSERT_STREQ(primary(), "world");
}

/* A drag back from where it went down selects back to the cell it went
 * down on, that one taken, across rows. */
UTEST(mouse, a_drag_backwards_and_across_rows)
{
    start();
    show(TEXT);
    drag(3, 1, 12, 0);
    ASSERT_TRUE(selected(12, 0, 3, 1));
    ASSERT_STREQ(primary(), "foo.bar\rsec");
}

/* What a drag from canvas pixel x0 to x1 of row y of the 80-column
 * console, both with the left button held, selects: "start-end" of the
 * columns of its gaps, or "none". */
static const char *cols(int x0, int x1, int y)
{
    static char out[16];
    tablet_host_pointer(x0, y * 16 + 7, 0, true);
    vterm_mouse_task();
    tablet_host_pointer(x0, y * 16 + 7, TABLET_FLAG_LEFT, true);
    vterm_mouse_task();
    tablet_host_pointer(x1, y * 16 + 7, TABLET_FLAG_LEFT, true);
    vterm_mouse_task();
    tablet_host_pointer(x1, y * 16 + 7, 0, true);
    vterm_mouse_task();
    machine_us(300 * 1000);
    if (!vterm_selection.shows)
        return "none";
    snprintf(out, sizeof out, "%d-%d", vterm_selection.start_col, vterm_selection.end_col);
    return out;
}

/* By characters, the press's character is the selection's start, taken as
 * soon as the pointer moves from the press's pixel, either way, even
 * within it; and the end the drag moves is the gap nearest the pointer, a
 * further character joining once the pointer passes its middle, 4 of its
 * 8 pixels. (Before, the ends were the left edges of the cells under the
 * press and the pointer, as xterm's are: going on, the press's character
 * joined only once the pointer left it and each further one only once the
 * pointer left that one, all 8 pixels across; going back, the press's
 * character never joined and each further one joined at its last pixel.) */
UTEST(mouse, a_drag_by_characters_ends_at_the_nearest_gap)
{
    start();
    show(TEXT);
    /* A press that does not move selects nothing. */
    for (int px = 8; px < 16; px++)
        ASSERT_STREQ(cols(px, px, 0), "none");
    /* Within the press's character, either way, that character. */
    ASSERT_STREQ(cols(9, 10, 0), "1-2");
    ASSERT_STREQ(cols(9, 15, 0), "1-2");
    ASSERT_STREQ(cols(14, 9, 0), "1-2");
    ASSERT_STREQ(cols(14, 8, 0), "1-2");
    /* Going on, the next character joins past its middle. */
    ASSERT_STREQ(cols(9, 16, 0), "1-2");
    ASSERT_STREQ(cols(9, 19, 0), "1-2");
    ASSERT_STREQ(cols(9, 20, 0), "1-3");
    ASSERT_STREQ(cols(9, 27, 0), "1-3");
    ASSERT_STREQ(cols(9, 28, 0), "1-4");
    /* Going back, the same from the other side. */
    ASSERT_STREQ(cols(22, 13, 0), "2-3");
    ASSERT_STREQ(cols(22, 12, 0), "2-3");
    ASSERT_STREQ(cols(22, 11, 0), "1-3");
    ASSERT_STREQ(cols(22, 4, 0), "1-3");
    ASSERT_STREQ(cols(22, 3, 0), "0-3");
}

/* What a drag from canvas pixel x0, y0 through the n points of to, each a
 * move, and released at the last, selects, as cols has it. */
static const char *path(int x0, int y0, const int (*to)[2], int n)
{
    static char out[16];
    tablet_host_pointer(x0, y0, 0, true);
    vterm_mouse_task();
    tablet_host_pointer(x0, y0, TABLET_FLAG_LEFT, true);
    vterm_mouse_task();
    for (int i = 0; i < n; i++)
    {
        tablet_host_pointer(to[i][0], to[i][1], TABLET_FLAG_LEFT, true);
        vterm_mouse_task();
    }
    tablet_host_pointer(to[n - 1][0], to[n - 1][1], 0, true);
    vterm_mouse_task();
    machine_us(300 * 1000);
    if (!vterm_selection.shows)
        return "none";
    snprintf(out, sizeof out, "%d-%d", vterm_selection.start_col, vterm_selection.end_col);
    return out;
}

/* A move straight up or down inside the press's character is a move away
 * from the press too, and takes that character, even from its last pixel
 * column, whose margin is only for a pointer past it; and a drag that has
 * moved keeps it when the pointer comes back to the press's pixel. */
UTEST(mouse, any_move_from_the_press_takes_its_character)
{
    start();
    show(TEXT);
    static const int down[][2] = {{9, 10}};
    ASSERT_STREQ(path(9, 7, down, 1), "1-2");
    static const int up[][2] = {{15, 2}};
    ASSERT_STREQ(path(15, 7, up, 1), "1-2");
    static const int back[][2] = {{12, 7}, {9, 7}};
    ASSERT_STREQ(path(9, 7, back, 2), "1-2");
    static const int still[][2] = {{9, 7}};
    ASSERT_STREQ(path(9, 7, still, 1), "none");
}

/* A press in its character's last pixel column, then a drag on, does not
 * take that character, nor one in its first column then a drag back: the
 * drag starts at that edge. Every other pixel takes it. */
UTEST(mouse, the_press_s_character_has_a_pixel_of_margin)
{
    start();
    show(TEXT);
    ASSERT_STREQ(cols(15, 16, 0), "none");
    ASSERT_STREQ(cols(15, 19, 0), "none");
    ASSERT_STREQ(cols(15, 20, 0), "2-3");
    ASSERT_STREQ(cols(14, 16, 0), "1-2");
    ASSERT_STREQ(cols(16, 15, 0), "none");
    ASSERT_STREQ(cols(16, 12, 0), "none");
    ASSERT_STREQ(cols(16, 11, 0), "1-2");
    ASSERT_STREQ(cols(17, 15, 0), "2-3");
    /* The margin is the press's own way: back from the last column, or on
     * from the first, the character is taken. */
    ASSERT_STREQ(cols(15, 14, 0), "1-2");
    ASSERT_STREQ(cols(8, 9, 0), "1-2");
}

/* On a row of double width a character is 16 pixels: its middle is 8 in,
 * and its margin is still a pixel. */
UTEST(mouse, a_wide_character_has_its_middle_at_8_pixels)
{
    start();
    show("\33[H\33[2J\33#6hello world\r\n");
    ASSERT_STREQ(cols(4, 5, 0), "0-1");
    ASSERT_STREQ(cols(4, 23, 0), "0-1");
    ASSERT_STREQ(cols(4, 24, 0), "0-2");
    ASSERT_STREQ(cols(15, 16, 0), "none");
    ASSERT_STREQ(cols(15, 24, 0), "1-2");
    ASSERT_STREQ(cols(46, 40, 0), "2-3");
    ASSERT_STREQ(cols(46, 24, 0), "2-3");
    ASSERT_STREQ(cols(46, 23, 0), "1-3");
    ASSERT_STREQ(cols(32, 31, 0), "none");
    ASSERT_STREQ(cols(32, 23, 0), "1-2");
}

/* A click with no drag selects nothing, which ends the selection, and the
 * primary keeps what it had. */
UTEST(mouse, a_click_ends_the_selection)
{
    start();
    show(TEXT);
    drag(0, 0, 5, 0);
    ASSERT_TRUE(vterm_selection.shows);
    click(8, 1);
    ASSERT_FALSE(vterm_selection.shows);
    ASSERT_STREQ(primary(), "hello");
}

/* A point past the end of a row's text stays on that row, highlighted to
 * where it is. The last row of a selection never takes its line end: a
 * drag takes a row's line end only by going on into the next row, to the
 * gap before its first character, which gives nothing. Every row before
 * the last takes its line end, and its cells never written give nothing,
 * so a press past the text on one row and a drag down takes that row's
 * line end alone. */
UTEST(mouse, past_the_text_stays_on_its_row)
{
    start();
    show(TEXT);
    drag(7, 1, 60, 1);
    ASSERT_TRUE(selected(7, 1, 60, 1));
    ASSERT_STREQ(primary(), "line");
    machine_us(300 * 1000);
    drag(7, 1, 0, 2);
    ASSERT_TRUE(selected(7, 1, 0, 2));
    ASSERT_STREQ(primary(), "line\r");
    drag(30, 0, 3, 1);
    ASSERT_TRUE(selected(30, 0, 3, 1));
    ASSERT_STREQ(primary(), "\rsec");
    drag(30, 1, 40, 0);
    ASSERT_TRUE(selected(40, 0, 30, 1));
    ASSERT_STREQ(primary(), "\rsecond line");
}

/* Output over a selected cell drops the highlight, and the primary keeps
 * the text. */
UTEST(mouse, output_drops_the_highlight)
{
    start();
    show(TEXT);
    drag(6, 0, 11, 0);
    ASSERT_TRUE(vterm_selection.shows);
    show("\33[1;8HX");
    ASSERT_FALSE(vterm_selection.shows);
    ASSERT_STREQ(primary(), "world");
}

/* Output that scrolls during a drag keeps where it went down on its text,
 * and when that row goes out at the top, the drag selects from the first
 * cell, as xterm's does, however far the screen scrolls: never from the
 * row that comes back round as the last. */
UTEST(mouse, a_drag_while_output_scrolls)
{
    start();
    show(TEXT);
    edge(6, 1, 0);
    edge(6, 1, TABLET_FLAG_LEFT);
    show("\33[30;1H\n");
    edge(4, 3, TABLET_FLAG_LEFT);
    ASSERT_TRUE(selected(6, 0, 4, 3));
    show("\n\n\n");
    edge(5, 3, TABLET_FLAG_LEFT);
    ASSERT_TRUE(selected(0, 0, 5, 3));
    for (int i = 0; i < 40; i++)
        show("\n");
    show("\33[15;1Hlate text");
    edge(4, 14, TABLET_FLAG_LEFT);
    edge(4, 14, 0);
    ASSERT_TRUE(selected(0, 0, 4, 14));
    ASSERT_STREQ(primary(), "\r\r\r\r\r\r\r\r\r\r\r\r\r\rlate");
}

/* A drag going down when the screen scrolls down under it keeps its start
 * at the end of the last row once that row goes out at the bottom. */
UTEST(mouse, a_drag_while_the_screen_scrolls_down)
{
    start();
    show(TEXT);
    at(3, 29, 0);
    at(3, 29, TABLET_FLAG_LEFT);
    show("\33[1;1H\33M");
    at(2, 1, TABLET_FLAG_LEFT);
    at(2, 1, 0);
    ASSERT_TRUE(selected(2, 1, 80, 29));
}

/* A finger, which has no hover, selects as the mouse does. */
UTEST(mouse, a_touch_selects)
{
    start();
    show(TEXT);
    tablet_point_t pt = {6 * 8, 7};
    tablet_host_touch(&pt, 1);
    vterm_mouse_task();
    pt.x = 11 * 8;
    tablet_host_touch(&pt, 1);
    vterm_mouse_task();
    tablet_host_clear();
    vterm_mouse_task();
    ASSERT_TRUE(selected(6, 0, 11, 0));
    ASSERT_STREQ(primary(), "world");
}

/* A program that maps the tablet and clears its share bit has the tablet
 * to itself: the terminal selects nothing. */
UTEST(mouse, a_program_that_does_not_share_gets_no_selection)
{
    start();
    show(TEXT);
    ASSERT_TRUE(xreg0(0, 3, 0xFF00));
    xram[0xFF00 + TABLET_OFF_STATUS] = 0;
    drag(6, 0, 11, 0);
    ASSERT_FALSE(vterm_selection.shows);
    xram[0xFF00 + TABLET_OFF_STATUS] = TABLET_STATUS_SHARED;
    drag(6, 0, 11, 0);
    ASSERT_TRUE(selected(6, 0, 11, 0));
    ASSERT_TRUE(xreg0(0, 3, 0xFFFF));
}

/* Mode 0 on a custom canvas: a drag that leaves its rows goes on to its
 * last row, and one that starts above them is no press of the terminal's. */
UTEST(mouse, a_drag_leaving_mode_0_goes_on)
{
    start();
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_320_240));
    /* 26 rows of 8 lines of the 40 columns from line 16. */
    ASSERT_TRUE(xreg1(0, 3, 16) && xreg1(0, 4, 224) && xreg1(0, 1, 0));
    show("\33[H\33[2Jtop row\r\n");
    tablet_host_pointer(2 * 8, 16 + 3, 0, true);
    vterm_mouse_task();
    tablet_host_pointer(2 * 8, 16 + 3, TABLET_FLAG_LEFT, true);
    vterm_mouse_task();
    tablet_host_pointer(300, 239, TABLET_FLAG_LEFT, true);
    vterm_mouse_task();
    tablet_host_pointer(300, 239, 0, true);
    vterm_mouse_task();
    const term_grid_t *g = vterm_get_shown(&vterm);
    ASSERT_EQ(g->width, 40);
    ASSERT_TRUE(selected(2, 0, 38, 25));
    /* Every row but the last is a line, as xterm copies them: the first's
     * text and the empty ones after it. */
    char want[6 + 25];
    memcpy(want, "p row", 5);
    memset(want + 5, '\r', 25);
    want[30] = 0;
    ASSERT_STREQ(primary(), want);

    tablet_host_pointer(10, 8, TABLET_FLAG_LEFT, true);
    vterm_mouse_task();
    tablet_host_pointer(100, 100, TABLET_FLAG_LEFT, true);
    vterm_mouse_task();
    tablet_host_pointer(100, 100, 0, true);
    vterm_mouse_task();
    ASSERT_TRUE(selected(2, 0, 38, 25));
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_console));
}

/* There is one selection: selecting on the alternate screen moves it
 * there, and back on the primary it is the primary's again. */
UTEST(mouse, selecting_on_the_other_screen_moves_it)
{
    start();
    show(TEXT);
    drag(0, 0, 5, 0);
    const term_grid_t *g = vterm_get_shown(&vterm);
    ASSERT_TRUE(vterm_selection.screen == &g->screen[0]);
    show("\33[?1049h\33[Halternate");
    drag(0, 0, 3, 0);
    ASSERT_TRUE(vterm_selection.screen == &g->screen[1]);
    ASSERT_TRUE(selected(0, 0, 3, 0));
    ASSERT_STREQ(primary(), "alt");
    show("\33[?1049l");
    drag(6, 0, 11, 0);
    ASSERT_TRUE(vterm_selection.screen == &g->screen[0]);
    ASSERT_STREQ(primary(), "world");
}

/* ---- words and lines (step 20) ------------------------------------------ */

UTEST(mouse, a_double_click_selects_a_word)
{
    start();
    show(TEXT);
    click(8, 0);
    click(8, 0);
    ASSERT_TRUE(selected(6, 0, 11, 0));
    ASSERT_STREQ(primary(), "world");
}

/* Punctuation is a class of its own for each character, as xterm's. */
UTEST(mouse, punctuation_is_a_word_of_its_own)
{
    start();
    show(TEXT);
    click(15, 0);
    click(15, 0);
    ASSERT_TRUE(selected(15, 0, 16, 0));
    click(13, 0);
    click(13, 0);
    ASSERT_STREQ(primary(), "foo");
}

/* A third click selects the line, which a row that wrapped on its own
 * joins to the next, with the line end after it; a fourth and a fifth go
 * back to characters, and a sixth to words, as xterm's clicks go. */
UTEST(mouse, a_triple_click_selects_a_wrapped_line)
{
    start();
    show("\33[H\33[2J");
    static char line[101];
    for (int i = 0; i < 100; i++)
        line[i] = (char)('a' + i % 26);
    show(line);
    show("\r\nnext\r\n");
    click(4, 1);
    click(4, 1);
    click(4, 1);
    ASSERT_TRUE(selected(0, 0, 0, 2));
    char want[102];
    memcpy(want, line, 100);
    want[100] = '\r';
    want[101] = 0;
    ASSERT_STREQ(primary(), want);
    click(4, 1);
    ASSERT_FALSE(vterm_selection.shows);
    click(4, 1);
    ASSERT_FALSE(vterm_selection.shows);
    click(4, 1);
    ASSERT_TRUE(selected(0, 0, 20, 1));
}

/* A drag after a double click goes by words, across rows. */
UTEST(mouse, a_drag_after_a_double_click_goes_by_words)
{
    start();
    show(TEXT);
    click(1, 0);
    at(1, 0, TABLET_FLAG_LEFT);
    at(2, 1, TABLET_FLAG_LEFT);
    at(2, 1, 0);
    ASSERT_TRUE(selected(0, 0, 6, 1));
    ASSERT_STREQ(primary(), "hello world foo.bar\rsecond");
}

/* A second click after the clicking time, or on another cell, is a new
 * click. */
UTEST(mouse, a_slow_second_click_is_a_new_click)
{
    start();
    show(TEXT);
    click(8, 0);
    machine_us(300000);
    click(8, 0);
    ASSERT_FALSE(vterm_selection.shows);
    click(8, 0);
    click(9, 0);
    ASSERT_FALSE(vterm_selection.shows);
}

/* ---- extending (step 21) ------------------------------------------------- */

/* The right button takes the nearer end to the click: before the start,
 * the start; after the end, the end. */
UTEST(mouse, the_right_button_extends_from_the_nearer_end)
{
    start();
    show(TEXT);
    drag(6, 0, 11, 0);
    edge(2, 0, TABLET_FLAG_RIGHT);
    edge(2, 0, 0);
    ASSERT_TRUE(selected(2, 0, 11, 0));
    ASSERT_STREQ(primary(), "llo world");
    edge(15, 0, TABLET_FLAG_RIGHT);
    edge(15, 0, 0);
    ASSERT_TRUE(selected(2, 0, 15, 0));
    ASSERT_STREQ(primary(), "llo world foo");
}

/* Shift with the left button extends too, in the unit of the selection:
 * from a word, by words. */
UTEST(mouse, shift_and_the_left_button_extend_by_word)
{
    start();
    show(TEXT);
    click(8, 0);
    click(8, 0);
    machine_us(300000);
    keyboard_set_key(HID_KEY_SHIFT_LEFT, true);
    click(17, 0);
    keyboard_set_key(HID_KEY_SHIFT_LEFT, false);
    ASSERT_TRUE(selected(6, 0, 19, 0));
    ASSERT_STREQ(primary(), "world foo.bar");
}

/* A press that extends is no click: a click straight after it, on the
 * cell the left button last went down on, starts again from one click. */
UTEST(mouse, an_extend_breaks_a_run_of_clicks)
{
    start();
    show(TEXT);
    drag(6, 0, 11, 0);
    keyboard_set_key(HID_KEY_SHIFT_LEFT, true);
    click(17, 0);
    keyboard_set_key(HID_KEY_SHIFT_LEFT, false);
    ASSERT_TRUE(selected(6, 0, 17, 0));
    click(6, 0);
    ASSERT_FALSE(vterm_selection.shows);
    click(6, 0);
    ASSERT_TRUE(selected(6, 0, 11, 0));
}

/* With nothing selected, extending selects from the click. */
UTEST(mouse, extending_nothing_selects_from_the_click)
{
    start();
    show(TEXT);
    edge(6, 0, TABLET_FLAG_RIGHT);
    edge(11, 0, TABLET_FLAG_RIGHT);
    edge(11, 0, 0);
    ASSERT_TRUE(selected(6, 0, 11, 0));
}

/* ---- pasting and the keys (step 22) -------------------------------------- */

/* The middle button pastes the primary as it comes up. */
UTEST(mouse, the_middle_button_pastes_the_primary)
{
    start();
    show(TEXT);
    drag(0, 0, 5, 0);
    at(30, 5, TABLET_FLAG_MIDDLE);
    ASSERT_STREQ(typed(), "");
    at(30, 5, 0);
    ASSERT_STREQ(typed(), "hello");
}

static hostkey_result_t chord(uint8_t usage, uint8_t modifier)
{
    return hostkey_press(usage, modifier, false, true);
}

#define CTRL KEYBOARD_MODIFIER_LEFTCTRL
#define SHIFT KEYBOARD_MODIFIER_LEFTSHIFT
#define GUI KEYBOARD_MODIFIER_LEFTGUI

/* While the mouse's highlight shows on the screen, a copy key puts the
 * text it shows in the host's clipboard at the press, keys typed before it
 * still unread, and queues nothing. Ctrl-Shift-C raises no SIGINT; Cmd-C is
 * a copy key on a Mac alone. */
UTEST(mouse, a_copy_key_copies_the_highlight_at_the_press)
{
    start();
    show(TEXT);
    drag(6, 0, 11, 0);
    typed_push("abc", 3);
    ria_get_sigint();
    ASSERT_TRUE(host_clipboard_set(""));
    ASSERT_EQ(chord(HID_KEY_C, CTRL | SHIFT), HOSTKEY_CHORD);
    ASSERT_FALSE(ria_get_sigint());
    ASSERT_STREQ(host_clipboard_get(), "world");
    ASSERT_TRUE(host_clipboard_set(""));
    ASSERT_EQ(chord(HID_KEY_INSERT, CTRL), HOSTKEY_CHORD);
    ASSERT_STREQ(host_clipboard_get(), "world");
    tb_mac_keys = true;
    ASSERT_TRUE(host_clipboard_set(""));
    ASSERT_EQ(chord(HID_KEY_C, GUI), HOSTKEY_CHORD);
    tb_mac_keys = false;
    ASSERT_STREQ(host_clipboard_get(), "world");
    /* A held chord acts once. */
    ASSERT_TRUE(host_clipboard_set(""));
    ASSERT_EQ(hostkey_press(HID_KEY_C, GUI, true, true), HOSTKEY_CHORD);
    ASSERT_STREQ(host_clipboard_get(), "");
    ASSERT_EQ(typed_free(), (size_t)TYPED_QUEUE_SIZE - 3);
    ASSERT_TRUE(selected(6, 0, 11, 0));
    ASSERT_STREQ(typed(), "abc");
    /* Off a Mac, Cmd-C is no chord. */
    ASSERT_NE(chord(HID_KEY_C, GUI), HOSTKEY_CHORD);
    ASSERT_STREQ(host_clipboard_get(), "");
    ASSERT_STREQ(typed(), "c");
}

/* The highlight is copied as it shows, a drag still held included, which
 * has not filled the primary yet. */
UTEST(mouse, a_copy_key_copies_what_a_drag_still_held_shows)
{
    start();
    show(TEXT);
    edge(6, 0, 0);
    edge(6, 0, TABLET_FLAG_LEFT);
    edge(9, 0, TABLET_FLAG_LEFT);
    ASSERT_TRUE(host_clipboard_set(""));
    ASSERT_EQ(chord(HID_KEY_INSERT, CTRL), HOSTKEY_CHORD);
    ASSERT_STREQ(host_clipboard_get(), "wor");
    edge(9, 0, 0);
}

/* With no highlight of the mouse's showing and no line being read, a copy
 * key does nothing, types nothing and holds nothing up: the clipboard
 * keeps what it held, here with the primary still filled by a selection
 * that output has since taken the highlight from. */
UTEST(mouse, a_copy_key_with_no_highlight_and_no_line_is_dropped)
{
    start();
    show(TEXT);
    drag(6, 0, 11, 0);
    show("\33[H\33[2J");
    ASSERT_FALSE(vterm_selection.shows);
    ASSERT_TRUE(host_clipboard_set("kept"));
    ria_get_sigint();
    ASSERT_EQ(chord(HID_KEY_C, CTRL | SHIFT), HOSTKEY_CHORD);
    ASSERT_EQ(chord(HID_KEY_INSERT, CTRL), HOSTKEY_CHORD);
    typed_push("a", 1);
    ASSERT_STREQ(typed(), "a");
    ASSERT_FALSE(ria_get_sigint());
    ASSERT_STREQ(host_clipboard_get(), "kept");
    ASSERT_STREQ(primary(), "world");
}

/* A cut key is the program's: with no line being read it types what the
 * key types, once however long it is held, and does nothing to the text
 * the mouse has highlighted, nor to the clipboard. Cmd-X is a cut key on a
 * Mac alone, and types nothing. */
UTEST(mouse, a_cut_key_with_no_line_read_types_its_bytes)
{
    start();
    show(TEXT);
    drag(6, 0, 11, 0);
    ASSERT_TRUE(host_clipboard_set("kept"));
    ASSERT_EQ(chord(HID_KEY_DELETE, SHIFT), HOSTKEY_CHORD);
    ASSERT_EQ(hostkey_press(HID_KEY_DELETE, SHIFT, true, true), HOSTKEY_CHORD);
    ASSERT_STREQ(typed(), "\33[3;2~");
    ASSERT_EQ(chord(HID_KEY_X, CTRL | SHIFT), HOSTKEY_CHORD);
    ASSERT_EQ(hostkey_press(HID_KEY_X, CTRL | SHIFT, true, true), HOSTKEY_CHORD);
    ASSERT_STREQ(typed(), "\x18");
    tb_mac_keys = true;
    ASSERT_EQ(hostkey_press(HID_KEY_X, GUI, false, false), HOSTKEY_CHORD);
    ASSERT_EQ(chord(HID_KEY_X, GUI), HOSTKEY_CHORD);
    tb_mac_keys = false;
    ASSERT_STREQ(typed(), "");
    ASSERT_EQ(typed_free(), (size_t)TYPED_QUEUE_SIZE);
    ASSERT_NE(chord(HID_KEY_X, GUI), HOSTKEY_CHORD);
    ASSERT_STREQ(typed(), "x");
    /* In VT52 mode Shift-Delete types what a VT52's Delete does. */
    show("\33[?2l");
    ASSERT_EQ(chord(HID_KEY_DELETE, SHIFT), HOSTKEY_CHORD);
    show("\33<");
    ASSERT_STREQ(typed(), "\33[3~");
    ASSERT_TRUE(selected(6, 0, 11, 0));
    ASSERT_STREQ(host_clipboard_get(), "kept");
}

/* The paste keys paste the clipboard, bracketed under ?2004 and not
 * without it, Cmd-V on a Mac alone; plain Ctrl-V and Ctrl-C stay the
 * program's. */
UTEST(mouse, the_paste_keys_paste_the_clipboard)
{
    start();
    ASSERT_TRUE(host_clipboard_set("text"));
    ASSERT_EQ(chord(HID_KEY_V, CTRL | SHIFT), HOSTKEY_CHORD);
    ASSERT_STREQ(typed(), "text");
    show("\33[?2004h");
    ASSERT_EQ(chord(HID_KEY_INSERT, SHIFT), HOSTKEY_CHORD);
    ASSERT_STREQ(typed(), "\33[200~text\33[201~");
    tb_mac_keys = true;
    ASSERT_EQ(chord(HID_KEY_V, GUI), HOSTKEY_CHORD);
    tb_mac_keys = false;
    ASSERT_STREQ(typed(), "\33[200~text\33[201~");
    ASSERT_NE(chord(HID_KEY_V, GUI), HOSTKEY_CHORD);
    ASSERT_EQ(strstr(typed(), "text"), NULL);
    show("\33[?2004l");
    ASSERT_EQ(chord(HID_KEY_V, CTRL), HOSTKEY_TYPED);
    ASSERT_STREQ(typed(), "\x16");
    ria_get_sigint();
    ASSERT_EQ(chord(HID_KEY_C, CTRL), HOSTKEY_TYPED);
    ASSERT_TRUE(ria_get_sigint());
    ASSERT_STREQ(typed(), "\x03");
}

/* A second paste key cuts nothing of the first: both are typed whole, in
 * the order they were pressed, around a key typed between them. */
UTEST(mouse, two_paste_keys_both_paste)
{
    start();
    show("\33[?2004h");
    ASSERT_TRUE(host_clipboard_set("text"));
    ASSERT_EQ(chord(HID_KEY_V, CTRL | SHIFT), HOSTKEY_CHORD);
    typed_push("k", 1);
    ASSERT_EQ(chord(HID_KEY_INSERT, SHIFT), HOSTKEY_CHORD);
    ASSERT_STREQ(typed(), "\33[200~text\33[201~k\33[200~text\33[201~");
    show("\33[?2004l");
}

/* A copy and a paste: what was selected comes back typed. */
UTEST(mouse, a_copy_and_a_paste_round_trip)
{
    start();
    show(TEXT);
    drag(0, 1, 6, 1);
    ASSERT_EQ(chord(HID_KEY_C, CTRL | SHIFT), HOSTKEY_CHORD);
    ASSERT_EQ(chord(HID_KEY_V, CTRL | SHIFT), HOSTKEY_CHORD);
    ASSERT_STREQ(typed(), "second");
}

/* ---- the line editor's selection (steps 26 and 27) ------------------------ */

static int line_count;
static char line_got[256];

static void line_read(bool timeout, const char *buf)
{
    (void)timeout;
    line_count++;
    strncpy(line_got, buf, sizeof line_got - 1);
    line_got[sizeof line_got - 1] = 0;
}

/* The main loop's turn for the mouse, the keys and the line editor. */
static void loop(void)
{
    for (int i = 0; i < 64; i++)
    {
        vterm_mouse_task();
        rln_task();
    }
}

/* The text, then a read on row 2 after a prompt of two columns, with
 * "abc def" typed, the cursor at its end, and nothing killed. */
static rln_kill_t line_kills;

static void editing(void)
{
    start();
    show(TEXT);
    show("> ");
    line_count = 0;
    line_got[0] = 0;
    line_kills.len = 0;
    rln_read_line(line_read, NULL, &line_kills);
    loop();
    typed_push("abc def", 7);
    loop();
}

/* Whether the highlight, the console's terminal's one selection, takes
 * column x of row y, as mode 0's row word has it. */
static bool hl(int x, int y)
{
    draw_publish();
    const uint32_t w = mode0_regs.row[y];
    return x >= MODE0_ROW_FROM(w) && x < MODE0_ROW_TO(w);
}

/* What the line editor's selection copies, the clipboard emptied first:
 * nothing with none. The copy is queued as a copy key queues it with no
 * highlight of the mouse's on the screen. */
static const char *line_copy(void)
{
    host_clipboard_set("");
    typed_put(TYPED_ENTRY(TYPED_COPY, 0));
    loop();
    return host_clipboard_get();
}

/* A drag in the line selects in it, as the terminal's one selection, and
 * fills the primary as a drag on the screen does; a click there ends the
 * selection and leaves the primary as it was. */
UTEST(mouse, a_drag_in_the_line_selects_in_the_line)
{
    editing();
    drag(0, 1, 6, 1);
    ASSERT_TRUE(selected(0, 1, 6, 1));
    drag(2, 2, 5, 2);
    loop();
    ASSERT_TRUE(selected(2, 2, 5, 2));
    ASSERT_STREQ(line_copy(), "abc");
    ASSERT_TRUE(hl(2, 2) && hl(4, 2));
    ASSERT_FALSE(hl(5, 2));
    ASSERT_STREQ(primary(), "abc");
    click(3, 2);
    loop();
    ASSERT_FALSE(vterm_selection.shows);
    ASSERT_STREQ(line_copy(), "");
    ASSERT_STREQ(primary(), "abc");
}

/* The line's selection fills the primary when the button comes up, not
 * while it drags; a word and the line fill it too. Shift with the arrows
 * selects without filling it, so a copy key copies that selection. */
UTEST(mouse, the_line_fills_the_primary_when_the_mouse_lets_go)
{
    editing();
    drag(0, 1, 6, 1);
    edge(2, 2, TABLET_FLAG_LEFT);
    edge(5, 2, TABLET_FLAG_LEFT);
    loop();
    ASSERT_STREQ(line_copy(), "abc");
    ASSERT_STREQ(primary(), "second");
    edge(5, 2, 0);
    loop();
    ASSERT_STREQ(primary(), "abc");
    machine_us(300 * 1000);
    click(7, 2);
    click(7, 2);
    loop();
    ASSERT_STREQ(primary(), "def");
    click(7, 2);
    loop();
    ASSERT_STREQ(primary(), "abc def");
    machine_us(300 * 1000);
    click(9, 2);
    loop();
    ASSERT_STREQ(primary(), "abc def");
    hostkey_press(HID_KEY_ARROW_LEFT, SHIFT, false, true);
    hostkey_press(HID_KEY_ARROW_LEFT, SHIFT, false, true);
    loop();
    ASSERT_STREQ(line_copy(), "ef");
    ASSERT_STREQ(primary(), "abc def");
}

/* The primary takes the line as the keys typed before the release left
 * it: here an 'X' still unread as the drag lets go, which the selection
 * then covers. */
UTEST(mouse, the_line_s_primary_follows_the_keys_before_it)
{
    editing();
    hostkey_press(HID_KEY_HOME, 0, false, true);
    loop();
    typed_push("X", 1);
    drag(2, 2, 5, 2);
    loop();
    ASSERT_STREQ(line_copy(), "Xab");
    ASSERT_STREQ(primary(), "Xab");
}

/* A drag that leaves the line stops at its first or last character. */
UTEST(mouse, a_drag_out_of_the_line_stops_at_its_ends)
{
    editing();
    drag(4, 2, 10, 0);
    loop();
    ASSERT_STREQ(line_copy(), "ab");
    ASSERT_TRUE(selected(2, 2, 4, 2));
    machine_us(300 * 1000);
    drag(4, 2, 30, 5);
    loop();
    ASSERT_STREQ(line_copy(), "c def");
}

/* A press outside the line is the primary's, even when its drag goes into
 * the line. */
UTEST(mouse, a_press_outside_the_line_is_the_primary)
{
    editing();
    drag(0, 1, 5, 2);
    loop();
    ASSERT_TRUE(vterm_selection.shows);
    ASSERT_STREQ(line_copy(), "");
    ASSERT_STREQ(primary(), "second line\r> abc");
}

/* One selection at a time: the primary's starting ends the line's, and
 * the line's, by the mouse or by Shift, ends the primary's. Shift selects
 * from the cursor, which the drag in the line left after "abc". */
UTEST(mouse, each_selection_ends_the_other)
{
    editing();
    drag(2, 2, 5, 2);
    loop();
    ASSERT_TRUE(hl(2, 2));
    drag(0, 1, 6, 1);
    loop();
    ASSERT_TRUE(selected(0, 1, 6, 1));
    ASSERT_FALSE(hl(2, 2));
    ASSERT_STREQ(line_copy(), "");
    hostkey_press(HID_KEY_ARROW_LEFT, KEYBOARD_MODIFIER_LEFTSHIFT, false, true);
    loop();
    ASSERT_TRUE(selected(4, 2, 5, 2));
    ASSERT_FALSE(hl(0, 1));
    ASSERT_STREQ(line_copy(), "c");
}

/* The line's cursor follows the mouse: a click puts it where what is typed
 * next goes, and a drag leaves it at the drag's end, backwards too, where
 * Shift goes on from it. */
UTEST(mouse, the_line_s_cursor_follows_the_mouse)
{
    editing();
    click(4, 2);
    loop();
    typed_push("X", 1);
    loop();
    machine_us(300 * 1000);
    click(2, 2);
    click(2, 2);
    click(2, 2);
    loop();
    ASSERT_STREQ(line_copy(), "abXc def");
    machine_us(300 * 1000);
    drag(9, 2, 6, 2);
    loop();
    ASSERT_STREQ(line_copy(), " de");
    hostkey_press(HID_KEY_ARROW_RIGHT, KEYBOARD_MODIFIER_LEFTSHIFT, false, true);
    loop();
    ASSERT_STREQ(line_copy(), "de");
}

/* The cursor follows the mouse in the line: a click puts it there, so a
 * key types there; a drag carries it to the drag's end, back or forth,
 * so Shift extends from there; a word or the line puts it at their end. */
UTEST(mouse, the_cursor_follows_the_mouse_in_the_line)
{
    editing();
    click(3, 2);
    loop();
    typed_push("X", 1);
    loop();
    hostkey_press(HID_KEY_ENTER, 0, false, true);
    loop();
    ASSERT_STREQ(line_got, "aXbc def");
    editing();
    drag(8, 2, 4, 2);
    loop();
    ASSERT_STREQ(line_copy(), "c de");
    hostkey_press(HID_KEY_ARROW_LEFT, KEYBOARD_MODIFIER_LEFTSHIFT, false, true);
    loop();
    ASSERT_STREQ(line_copy(), "bc de");
    typed_push("X", 1);
    loop();
    hostkey_press(HID_KEY_ENTER, 0, false, true);
    loop();
    ASSERT_STREQ(line_got, "aXf");
    editing();
    click(3, 2);
    click(3, 2);
    loop();
    typed_push("!", 1);
    loop();
    hostkey_press(HID_KEY_ENTER, 0, false, true);
    loop();
    ASSERT_STREQ(line_got, "! def");
}

/* A double click in the line takes a word, a triple click the line, and a
 * fourth click characters again. */
UTEST(mouse, clicks_in_the_line_take_a_word_and_the_line)
{
    editing();
    click(7, 2);
    click(7, 2);
    loop();
    ASSERT_STREQ(line_copy(), "def");
    click(7, 2);
    loop();
    ASSERT_STREQ(line_copy(), "abc def");
    click(7, 2);
    loop();
    ASSERT_STREQ(line_copy(), "");
}

/* A press while the line's echo is still going out at a slow speed is
 * judged against the line as the line editor has laid it out, and waits
 * for the echo as a key does: here an 'x' typed at the start of a long
 * line, whose rewrite of the line is still going out. */
UTEST(mouse, a_press_during_a_slow_echo_waits_for_it)
{
    editing();
    typed_push("ghijklmnopqrstuvwxyzghijklmnopqrstuvwxyz", 40);
    loop();
    show("\33[0;1*r");
    manifold_set_paced(true);
    hostkey_press(HID_KEY_HOME, 0, false, true);
    typed_push("x", 1);
    loop();
    ASSERT_TRUE(rln_replying());
    drag(4, 2, 6, 2);
    loop();
    ASSERT_TRUE(rln_replying());
    ASSERT_FALSE(rln_selected());
    manifold_set_paced(false);
    show("\33[0;0*r");
    loop();
    ASSERT_STREQ(line_copy(), "bc");
}

/* A drag in the line by characters selects as a drag on the screen does:
 * the press's character as soon as the pointer moves, but for a pixel of
 * margin, and up to the gap nearest the pointer. A press that does not
 * move selects nothing. */
UTEST(mouse, a_drag_in_the_line_ends_at_the_nearest_gap)
{
    /* "abc def" from column 2: c at column 4, the space at 5, d at 6. */
    editing();
    const int y = 2 * 16 + 7;
    static const struct
    {
        int x0, x1;
        const char *got;
    } drags[] = {
        {8 * 4 + 6, 8 * 4 + 6, ""},  {8 * 4 + 6, 8 * 4 + 7, "c"},  {8 * 4 + 6, 8 * 5 + 3, "c"},
        {8 * 4 + 6, 8 * 5 + 4, "c "}, {8 * 4 + 7, 8 * 5, ""},      {8 * 4 + 7, 8 * 5 + 4, " "},
        {8 * 6 + 1, 8 * 6, "d"},      {8 * 6 + 1, 8 * 5 + 4, "d"}, {8 * 6 + 1, 8 * 5 + 3, " d"},
        {8 * 6, 8 * 5 + 7, ""},       {8 * 6, 8 * 5 + 3, " "},
    };
    for (unsigned i = 0; i < sizeof drags / sizeof *drags; i++)
    {
        tablet_host_pointer(drags[i].x0, y, TABLET_FLAG_LEFT, true);
        vterm_mouse_task();
        tablet_host_pointer(drags[i].x1, y, TABLET_FLAG_LEFT, true);
        vterm_mouse_task();
        tablet_host_pointer(drags[i].x1, y, 0, true);
        vterm_mouse_task();
        loop();
        ASSERT_STREQ(line_copy(), drags[i].got);
        machine_us(300 * 1000);
    }
}

/* A click with the left button at pixel px of row y, as quick as the
 * mouse can. */
static void click_px(int px, int y)
{
    tablet_host_pointer(px, y * 16 + 7, TABLET_FLAG_LEFT, true);
    vterm_mouse_task();
    tablet_host_pointer(px, y * 16 + 7, 0, true);
    vterm_mouse_task();
}

/* What the line reads with "X" typed where the cursor is, ended with
 * Enter. */
static const char *typed_x(void)
{
    typed_push("X", 1);
    loop();
    hostkey_press(HID_KEY_ENTER, 0, false, true);
    loop();
    return line_got;
}

/* The Insert key, typed: overwrite mode, with its block cursor. */
#define OVERWRITE() typed_push("\33[2~", 4)

/* A click in the line in insert mode puts the bar at the gap nearest the
 * pointer, as xterm's click: before the character from its pixels 0 to 4,
 * after it from 5 to 7. In overwrite mode it puts the block on the
 * character, wherever in it the click is, the mode an Insert key typed
 * before the click and still to take sets. Past the line's end, both put
 * the cursor at the end. */
UTEST(mouse, a_click_in_the_line_places_the_cursor_by_the_mode)
{
    /* "abc def" from column 2: c at column 4. */
    for (int px = 0; px < 8; px++)
    {
        editing();
        click_px(8 * 4 + px, 2);
        loop();
        ASSERT_STREQ(typed_x(), px < 5 ? "abXc def" : "abcX def");
        editing();
        OVERWRITE();
        click_px(8 * 4 + px, 2);
        loop();
        ASSERT_STREQ(typed_x(), "abX def");
        editing();
        click_px(8 * 9 + px, 2);
        loop();
        ASSERT_STREQ(typed_x(), "abc defX");
        editing();
        OVERWRITE();
        click_px(8 * 9 + px, 2);
        loop();
        ASSERT_STREQ(typed_x(), "abc defX");
    }
}

/* A click at the last column of a row the line wraps from puts the bar
 * after its character at the first column of the next row, and the block
 * there on the character at that first column. */
UTEST(mouse, a_click_in_a_wrapped_line_places_the_cursor_across_rows)
{
    /* The line from column 2 of row 2: index 77 at column 79, index 78 at
     * column 0 of row 3. */
    static char line[91], want[92];
    for (int i = 0; i < 90; i++)
        line[i] = (char)('a' + i % 26);
    for (int mode = 0; mode < 3; mode++)
    {
        start();
        show(TEXT);
        show("> ");
        rln_read_line(line_read, NULL, NULL);
        loop();
        typed_push(line, 90);
        loop();
        int at = 78;
        if (mode == 0)
            click_px(8 * 79 + 5, 2);
        else if (mode == 1)
        {
            click_px(8 * 79 + 4, 2);
            at = 77;
        }
        else
        {
            OVERWRITE();
            click_px(8 * 0 + 7, 3);
        }
        loop();
        memcpy(want, line, (size_t)at);
        want[at] = 'X';
        strcpy(want + at + 1, line + at + (mode == 2));
        ASSERT_STREQ(typed_x(), want);
    }
}

/* On a row of double width a character is 16 pixels, and a click in insert
 * mode puts the bar after it from its pixel 9, as xterm's click. */
UTEST(mouse, a_click_in_a_wide_line_places_the_cursor_after_pixel_8)
{
    for (int px = 0; px < 16; px++)
        for (int over = 0; over < 2; over++)
        {
            start();
            show(TEXT);
            show("\33#6> ");
            line_count = 0;
            rln_read_line(line_read, NULL, NULL);
            loop();
            typed_push("abc def", 7);
            loop();
            if (over)
                OVERWRITE();
            click_px(16 * 4 + px, 2);
            loop();
            ASSERT_STREQ(typed_x(), over ? "abX def" : px < 9 ? "abXc def" : "abcX def");
        }
}

/* A double click on the right of a character takes its word, and a
 * triple click the line, whichever gap the click is nearer. */
UTEST(mouse, a_double_click_takes_the_word_on_either_side_of_its_character)
{
    for (int px = 0; px < 8; px++)
    {
        editing();
        click_px(8 * 4 + px, 2);
        click_px(8 * 4 + px, 2);
        loop();
        ASSERT_STREQ(line_copy(), "abc");
        click_px(8 * 4 + px, 2);
        loop();
        ASSERT_STREQ(line_copy(), "abc def");
        machine_us(300 * 1000);
    }
}

/* A drag in the line leaves the cursor at its end in either mode, as
 * before: from the last pixel of c a pixel on into the space selects
 * nothing and leaves the cursor at the gap after c, where overwrite
 * types over the space; from pixel 2 of c back a pixel it selects c,
 * which the typing replaces. */
UTEST(mouse, a_drag_in_the_line_is_the_same_in_either_mode)
{
    const int y = 2 * 16 + 7;
    for (int over = 0; over < 2; over++)
    {
        static const struct
        {
            int x0, x1;
            const char *got[2];
        } drags[] = {
            {8 * 4 + 7, 8 * 5, {"abcX def", "abcXdef"}},
            {8 * 4 + 2, 8 * 4 + 1, {"abX def", "abX def"}},
            {8 * 4 + 4, 8 * 4 + 5, {"abX def", "abX def"}},
        };
        for (unsigned i = 0; i < sizeof drags / sizeof *drags; i++)
        {
            editing();
            if (over)
                OVERWRITE();
            loop();
            tablet_host_pointer(drags[i].x0, y, TABLET_FLAG_LEFT, true);
            vterm_mouse_task();
            tablet_host_pointer(drags[i].x1, y, TABLET_FLAG_LEFT, true);
            vterm_mouse_task();
            tablet_host_pointer(drags[i].x1, y, 0, true);
            vterm_mouse_task();
            loop();
            ASSERT_STREQ(typed_x(), drags[i].got[over]);
        }
    }
}

/* The right button with the line's selection showing has no primary to
 * extend: it selects from the click, as with nothing selected, which ends
 * the line's. */
UTEST(mouse, extending_the_line_s_selection_starts_the_primary)
{
    editing();
    drag(2, 2, 5, 2);
    loop();
    ASSERT_TRUE(selected(2, 2, 5, 2));
    edge(6, 0, TABLET_FLAG_RIGHT);
    edge(11, 0, TABLET_FLAG_RIGHT);
    edge(11, 0, 0);
    ASSERT_TRUE(selected(6, 0, 11, 0));
    ASSERT_STREQ(line_copy(), "");
    ASSERT_STREQ(primary(), "world");
}

/* A drag whose press was in a read's line that has ended selects nothing
 * in the next read's line. */
UTEST(mouse, a_drag_ends_with_its_read)
{
    editing();
    ASSERT_EQ(rln_index_at(2, 2), 0);
    at(2, 2, 0);
    at(2, 2, TABLET_FLAG_LEFT);
    loop();
    hostkey_press(HID_KEY_ENTER, 0, false, true);
    loop();
    ASSERT_EQ(line_count, 1);
    show("> ");
    rln_read_line(line_read, NULL, NULL);
    loop();
    typed_push("xyz", 3);
    loop();
    at(4, 3, TABLET_FLAG_LEFT);
    at(4, 3, 0);
    loop();
    ASSERT_FALSE(hl(2, 3));
    ASSERT_STREQ(line_copy(), "");
    ASSERT_FALSE(vterm_selection.shows);
}

#define ARROW_LEFT(m) hostkey_press(HID_KEY_ARROW_LEFT, (m), false, true)

/* The copy keys copy the line's selection, in their place among the keys:
 * a Shift-Left still unread when the key is pressed makes the selection
 * that is copied. The selection stays as it was, its highlight too, for
 * the next key to act on. */
UTEST(mouse, the_copy_keys_copy_the_line_s_selection)
{
    editing();
    ASSERT_TRUE(host_clipboard_set(""));
    ARROW_LEFT(SHIFT);
    ARROW_LEFT(SHIFT);
    ASSERT_EQ(chord(HID_KEY_C, CTRL | SHIFT), HOSTKEY_CHORD);
    ASSERT_STREQ(host_clipboard_get(), "");
    loop();
    ASSERT_STREQ(host_clipboard_get(), "ef");
    ASSERT_TRUE(selected(7, 2, 9, 2));
    ASSERT_TRUE(hl(7, 2) && hl(8, 2));
    ASSERT_TRUE(host_clipboard_set(""));
    ASSERT_EQ(chord(HID_KEY_INSERT, CTRL), HOSTKEY_CHORD);
    loop();
    ASSERT_STREQ(host_clipboard_get(), "ef");
    ARROW_LEFT(SHIFT);
    tb_mac_keys = true;
    ASSERT_EQ(chord(HID_KEY_C, GUI), HOSTKEY_CHORD);
    tb_mac_keys = false;
    typed_push("X\r", 2);
    loop();
    ASSERT_STREQ(host_clipboard_get(), "def");
    ASSERT_STREQ(line_got, "abc X");
}

/* A copy key with nothing selected in the line does nothing, and the
 * clipboard keeps what it held. */
UTEST(mouse, a_copy_of_nothing_in_the_line_keeps_the_clipboard)
{
    editing();
    ASSERT_TRUE(host_clipboard_set("kept"));
    ASSERT_EQ(chord(HID_KEY_C, CTRL | SHIFT), HOSTKEY_CHORD);
    typed_push("X\r", 2);
    loop();
    ASSERT_STREQ(host_clipboard_get(), "kept");
    ASSERT_STREQ(line_got, "abc defX");
}

/* While the mouse's highlight shows on the screen, a copy key pressed
 * with a line being read copies the screen's text at the press, and
 * nothing waits for the line: a Shift-Left still unread, which will end
 * that highlight, does not change what was copied. */
UTEST(mouse, a_copy_key_takes_the_screen_s_highlight_over_the_line)
{
    editing();
    ARROW_LEFT(SHIFT);
    loop();
    drag(0, 1, 6, 1);
    loop();
    ASSERT_FALSE(rln_selected());
    ASSERT_TRUE(host_clipboard_set(""));
    ARROW_LEFT(SHIFT);
    ASSERT_EQ(chord(HID_KEY_INSERT, CTRL), HOSTKEY_CHORD);
    ASSERT_STREQ(host_clipboard_get(), "second");
    loop();
    ASSERT_STREQ(host_clipboard_get(), "second");
    ASSERT_TRUE(rln_selected());
    ASSERT_FALSE(hl(0, 1));
}

/* Shift-Delete cuts the line's selection to the clipboard, in its place
 * among the keys, and with none does nothing: what the key would type is
 * dropped, so it deletes nothing as Delete would. */
UTEST(mouse, shift_delete_cuts_the_line_s_selection)
{
    editing();
    ASSERT_TRUE(host_clipboard_set(""));
    ARROW_LEFT(SHIFT);
    ARROW_LEFT(SHIFT);
    loop();
    ASSERT_EQ(hostkey_press(HID_KEY_DELETE, SHIFT, false, true), HOSTKEY_CHORD);
    ASSERT_STREQ(host_clipboard_get(), "");
    loop();
    ASSERT_STREQ(host_clipboard_get(), "ef");
    ASSERT_FALSE(rln_selected());
    ARROW_LEFT(0);
    ARROW_LEFT(0);
    ASSERT_EQ(hostkey_press(HID_KEY_DELETE, SHIFT, false, true), HOSTKEY_CHORD);
    hostkey_press(HID_KEY_ENTER, 0, false, true);
    loop();
    ASSERT_EQ(line_count, 1);
    ASSERT_STREQ(line_got, "abc d");
    ASSERT_STREQ(host_clipboard_get(), "ef");
}

/* Shift-Left and then Shift-Delete pressed before the line editor has
 * taken the Shift-Left: the cut is taken after it, so it cuts what that
 * selected. */
UTEST(mouse, a_cut_follows_the_key_that_selects)
{
    editing();
    ASSERT_TRUE(host_clipboard_set(""));
    ARROW_LEFT(SHIFT);
    ASSERT_EQ(hostkey_press(HID_KEY_DELETE, SHIFT, false, true), HOSTKEY_CHORD);
    loop();
    ASSERT_STREQ(host_clipboard_get(), "f");
    hostkey_press(HID_KEY_ENTER, 0, false, true);
    loop();
    ASSERT_STREQ(line_got, "abc de");
}

/* A copy or cut key has its place among the keys: the keys typed before
 * it are taken first and the keys typed after it wait, however far behind
 * the line editor is. Here nothing runs between the presses: the cut takes
 * what the Shift-Left selected, and the 'x' is typed after it; and the 'x'
 * after a copy replaces what was copied. */
UTEST(mouse, a_key_typed_after_a_cut_or_a_copy_follows_it)
{
    editing();
    ASSERT_TRUE(host_clipboard_set(""));
    ARROW_LEFT(SHIFT);
    ASSERT_EQ(hostkey_press(HID_KEY_DELETE, SHIFT, false, true), HOSTKEY_CHORD);
    typed_push("x", 1);
    hostkey_press(HID_KEY_ENTER, 0, false, true);
    loop();
    ASSERT_STREQ(host_clipboard_get(), "f");
    ASSERT_STREQ(line_got, "abc dex");
    editing();
    ASSERT_TRUE(host_clipboard_set(""));
    ARROW_LEFT(SHIFT);
    ASSERT_EQ(chord(HID_KEY_INSERT, CTRL), HOSTKEY_CHORD);
    typed_push("x", 1);
    hostkey_press(HID_KEY_ENTER, 0, false, true);
    loop();
    ASSERT_STREQ(host_clipboard_get(), "f");
    ASSERT_STREQ(line_got, "abc dex");
}

/* A paste pressed right behind a cut or a copy pastes what that put in
 * the clipboard: each is taken in its turn, and the paste reads the
 * clipboard when its turn comes. So it is with the host's clipboard, and
 * on a machine with its own. */
UTEST(mouse, a_paste_behind_a_cut_or_a_copy_pastes_what_it_took)
{
    for (int own = 0; own < 2; own++)
    {
        tb_no_clipboard = own;
        editing();
        if (!own)
            ASSERT_TRUE(host_clipboard_set("old"));
        ARROW_LEFT(SHIFT);
        ASSERT_EQ(hostkey_press(HID_KEY_DELETE, SHIFT, false, true), HOSTKEY_CHORD);
        ASSERT_EQ(chord(HID_KEY_V, CTRL | SHIFT), HOSTKEY_CHORD);
        hostkey_press(HID_KEY_ENTER, 0, false, true);
        loop();
        ASSERT_STREQ(line_got, "abc def");
        editing();
        ARROW_LEFT(SHIFT);
        ASSERT_EQ(chord(HID_KEY_INSERT, CTRL), HOSTKEY_CHORD);
        hostkey_press(HID_KEY_END, 0, false, true);
        ASSERT_EQ(chord(HID_KEY_V, CTRL | SHIFT), HOSTKEY_CHORD);
        hostkey_press(HID_KEY_ENTER, 0, false, true);
        loop();
        ASSERT_STREQ(line_got, "abc deff");
    }
    tb_no_clipboard = false;
    ASSERT_STREQ(host_clipboard_get(), "f");
}

/* A web page's clipboard cannot be asked for: its text comes with the
 * paste event, and is typed as it was handed over, in the event's place
 * among the keys. So a paste pressed right behind a cut pastes what the
 * clipboard held at the press, not what the cut put there after, which
 * still reaches the page's clipboard. */
UTEST(mouse, a_web_page_s_paste_types_what_its_event_handed_over)
{
    tb_clipboard_unasked = true;
    editing();
    ASSERT_TRUE(host_clipboard_set("old"));
    typed_push("g", 1);
    ARROW_LEFT(SHIFT);
    ASSERT_EQ(hostkey_press(HID_KEY_DELETE, SHIFT, false, true), HOSTKEY_CHORD);
    paste_start(tb_clipboard_text());
    typed_push("h", 1);
    hostkey_press(HID_KEY_ENTER, 0, false, true);
    loop();
    tb_clipboard_unasked = false;
    ASSERT_STREQ(line_got, "abc defoldh");
    ASSERT_STREQ(host_clipboard_get(), "g");
}

/* A cut pressed while a paste is typed waits for the paste: it takes what
 * the Shift-Left after the paste selected. */
UTEST(mouse, a_cut_pressed_in_a_paste_waits_for_it)
{
    editing();
    ARROW_LEFT(SHIFT);
    loop();
    ASSERT_TRUE(host_clipboard_set("XY"));
    ASSERT_EQ(chord(HID_KEY_V, CTRL | SHIFT), HOSTKEY_CHORD);
    ARROW_LEFT(SHIFT);
    ASSERT_EQ(hostkey_press(HID_KEY_DELETE, SHIFT, false, true), HOSTKEY_CHORD);
    typed_push("x", 1);
    hostkey_press(HID_KEY_ENTER, 0, false, true);
    loop();
    ASSERT_STREQ(line_got, "abc deXx");
    ASSERT_STREQ(host_clipboard_get(), "Y");
}

/* A click in the line has its place among the keys too: the keys typed
 * before it go where the cursor was, and the key typed after it goes where
 * the click put the cursor. */
UTEST(mouse, a_key_typed_after_a_click_goes_where_it_put_the_cursor)
{
    editing();
    typed_push("gh", 2);
    click(3, 2);
    typed_push("x", 1);
    hostkey_press(HID_KEY_ENTER, 0, false, true);
    loop();
    ASSERT_STREQ(line_got, "axbc defgh");
}

/* A drag in the line has its place among the keys: the key typed after it
 * replaces what it selected, and the key typed before it is in the line it
 * selects from. */
UTEST(mouse, a_key_typed_after_a_drag_replaces_what_it_selected)
{
    editing();
    hostkey_press(HID_KEY_HOME, 0, false, true);
    typed_push("Z", 1);
    drag(2, 2, 5, 2);
    typed_push("x", 1);
    hostkey_press(HID_KEY_ENTER, 0, false, true);
    loop();
    ASSERT_STREQ(line_got, "xc def");
    ASSERT_STREQ(primary(), "Zab");
}

/* A read that ends before a cut behind its Enter is taken lets the cut's
 * bytes and the keys behind it go to whatever reads next. A copy there is
 * dropped. What the mouse did in the line after the Enter was typed is
 * taken back as the read ends, and a button that comes up after its read
 * ended queues nothing. */
UTEST(mouse, a_read_s_end_lets_the_keys_behind_it_go)
{
    editing();
    ASSERT_TRUE(host_clipboard_set("kept"));
    hostkey_press(HID_KEY_ENTER, 0, false, true);
    ASSERT_EQ(hostkey_press(HID_KEY_DELETE, SHIFT, false, true), HOSTKEY_CHORD);
    ASSERT_EQ(chord(HID_KEY_INSERT, CTRL), HOSTKEY_CHORD);
    typed_push("x", 1);
    loop();
    ASSERT_EQ(line_count, 1);
    ASSERT_STREQ(line_got, "abc def");
    ASSERT_STREQ(typed(), "\33[3;2~x");
    ASSERT_STREQ(host_clipboard_get(), "kept");
    editing();
    hostkey_press(HID_KEY_ENTER, 0, false, true);
    edge(2, 2, 0);
    edge(2, 2, TABLET_FLAG_LEFT);
    edge(5, 2, TABLET_FLAG_LEFT);
    ASSERT_EQ(typed_free(), (size_t)TYPED_QUEUE_SIZE - 4);
    typed_push("q", 1);
    loop();
    ASSERT_EQ(line_count, 1);
    edge(5, 2, 0);
    loop();
    ASSERT_EQ(typed_free(), (size_t)TYPED_QUEUE_SIZE - 4);
    ASSERT_STREQ(typed(), "q");
    ASSERT_EQ(typed_free(), (size_t)TYPED_QUEUE_SIZE);
}

/* A press in one read's line that is still unread when the next read
 * starts is not the next line's. */
UTEST(mouse, a_press_is_its_own_read_s)
{
    editing();
    hostkey_press(HID_KEY_ENTER, 0, false, true);
    click(3, 2);
    typed_push("xyz", 3);
    rln_task();
    ASSERT_EQ(line_count, 1);
    show("> ");
    rln_read_line(line_read, NULL, NULL);
    loop();
    hostkey_press(HID_KEY_ENTER, 0, false, true);
    loop();
    ASSERT_EQ(line_count, 2);
    ASSERT_STREQ(line_got, "xyz");
}

/* The mouse's selecting on the screen takes back what the mouse did in
 * the line before it and the line editor has yet to take, so the line's
 * selection does not come after it to end the screen's. */
UTEST(mouse, a_selection_on_the_screen_takes_back_the_line_s_still_to_take)
{
    editing();
    typed_push("g", 1);
    drag(2, 2, 5, 2);
    machine_us(300 * 1000);
    drag(0, 1, 6, 1);
    loop();
    ASSERT_TRUE(selected(0, 1, 6, 1));
    ASSERT_FALSE(rln_selected());
    hostkey_press(HID_KEY_ENTER, 0, false, true);
    loop();
    ASSERT_STREQ(line_got, "abc defg");
}

/* A copy and a cut pressed one after the other each act in their turn:
 * the copy copies and leaves the selection, the cut takes it, and the copy
 * after that finds nothing selected. */
UTEST(mouse, a_copy_and_a_cut_each_act_in_turn)
{
    editing();
    ASSERT_TRUE(host_clipboard_set(""));
    ARROW_LEFT(SHIFT);
    ASSERT_EQ(chord(HID_KEY_INSERT, CTRL), HOSTKEY_CHORD);
    ASSERT_EQ(hostkey_press(HID_KEY_DELETE, SHIFT, false, true), HOSTKEY_CHORD);
    ASSERT_EQ(chord(HID_KEY_C, CTRL | SHIFT), HOSTKEY_CHORD);
    loop();
    ASSERT_STREQ(host_clipboard_get(), "f");
    hostkey_press(HID_KEY_ENTER, 0, false, true);
    loop();
    ASSERT_STREQ(line_got, "abc de");
}

/* In VT52 mode the keyboard sends no modifiers, so Shift-Delete would
 * type Delete and Ctrl-Insert would type Insert. The line editor takes
 * the cut and the copy as what they are all the same: the cut cuts the
 * selection, and with none deletes nothing, and the copy copies it and
 * leaves the insert mode as it was. */
UTEST(mouse, the_cut_and_copy_keys_work_in_vt52_mode)
{
    editing();
    ASSERT_TRUE(host_clipboard_set(""));
    ARROW_LEFT(SHIFT);
    ARROW_LEFT(SHIFT);
    loop();
    show("\33[?2l");
    ASSERT_EQ(chord(HID_KEY_INSERT, CTRL), HOSTKEY_CHORD);
    loop();
    ASSERT_STREQ(host_clipboard_get(), "ef");
    ASSERT_EQ(hostkey_press(HID_KEY_DELETE, SHIFT, false, true), HOSTKEY_CHORD);
    loop();
    ASSERT_FALSE(rln_selected());
    hostkey_press(HID_KEY_ARROW_LEFT, 0, false, true);
    ASSERT_EQ(hostkey_press(HID_KEY_X, CTRL | SHIFT, false, true), HOSTKEY_CHORD);
    ASSERT_EQ(hostkey_press(HID_KEY_DELETE, SHIFT, false, true), HOSTKEY_CHORD);
    ASSERT_EQ(chord(HID_KEY_INSERT, CTRL), HOSTKEY_CHORD);
    typed_push("X\r", 2);
    loop();
    show("\33<");
    ASSERT_STREQ(line_got, "abc Xd");
    ASSERT_STREQ(host_clipboard_get(), "ef");
}

/* A word the mouse selects is the word a kill key kills, and the mouse
 * between two kills starts what Ctrl-Y puts back again, as a key does.
 * What a cut takes is the clipboard's, not Ctrl-Y's, and a cut with
 * nothing to take and a copy start it again too. */
UTEST(mouse, the_mouse_and_a_cut_start_the_kills_again)
{
    editing();
    typed_push("\27", 1);
    loop();
    click(3, 2);
    click(3, 2);
    loop();
    ASSERT_STREQ(line_copy(), "abc");
    typed_push("\27\31\r", 3);
    loop();
    ASSERT_STREQ(line_got, "abc ");
    editing();
    ASSERT_TRUE(host_clipboard_set(""));
    typed_push("\27", 1);
    ARROW_LEFT(SHIFT);
    ASSERT_EQ(hostkey_press(HID_KEY_DELETE, SHIFT, false, true), HOSTKEY_CHORD);
    loop();
    ASSERT_STREQ(host_clipboard_get(), " ");
    typed_push("\27\31\r", 3);
    loop();
    ASSERT_STREQ(line_got, "abc");
    for (int copy = 0; copy < 2; copy++)
    {
        editing();
        typed_push("\27", 1);
        loop();
        if (copy)
            ASSERT_EQ(chord(HID_KEY_INSERT, CTRL), HOSTKEY_CHORD);
        else
            ASSERT_EQ(hostkey_press(HID_KEY_DELETE, SHIFT, false, true), HOSTKEY_CHORD);
        loop();
        typed_push("\27\31\r", 3);
        loop();
        ASSERT_STREQ(line_got, "abc ");
    }
}

/* A double click takes a word by the line editor's rule: what a space, a
 * colon, a slash or a backslash parts, so a file's name with its
 * extension. */
UTEST(mouse, a_double_click_takes_a_file_s_name)
{
    editing();
    typed_push("\25load USB0:/a.b", 15);
    loop();
    click(14, 2);
    click(14, 2);
    loop();
    ASSERT_STREQ(line_copy(), "a.b");
    machine_us(1000 * 1000);
    click(8, 2);
    click(8, 2);
    loop();
    ASSERT_STREQ(line_copy(), "USB0");
}

/* Ctrl-Shift-X, and Cmd-X on a Mac, cut as Shift-Delete does, a held one
 * once, and with no selection do nothing: the Ctrl-X the key types is
 * dropped with its mark. Off a Mac, Cmd-X is no chord. */
UTEST(mouse, ctrl_shift_x_and_cmd_x_cut_the_line_s_selection)
{
    editing();
    ASSERT_TRUE(host_clipboard_set(""));
    ARROW_LEFT(SHIFT);
    ARROW_LEFT(SHIFT);
    loop();
    ASSERT_EQ(chord(HID_KEY_X, CTRL | SHIFT), HOSTKEY_CHORD);
    ASSERT_EQ(hostkey_press(HID_KEY_X, CTRL | SHIFT, true, true), HOSTKEY_CHORD);
    loop();
    ASSERT_STREQ(host_clipboard_get(), "ef");
    hostkey_press(HID_KEY_ENTER, 0, false, true);
    loop();
    ASSERT_STREQ(line_got, "abc d");
    editing();
    ARROW_LEFT(SHIFT);
    loop();
    tb_mac_keys = true;
    ASSERT_EQ(chord(HID_KEY_X, GUI), HOSTKEY_CHORD);
    tb_mac_keys = false;
    loop();
    ASSERT_STREQ(host_clipboard_get(), "f");
    hostkey_press(HID_KEY_ENTER, 0, false, true);
    loop();
    ASSERT_STREQ(line_got, "abc de");
    editing();
    ASSERT_TRUE(host_clipboard_set(""));
    ASSERT_EQ(chord(HID_KEY_X, CTRL | SHIFT), HOSTKEY_CHORD);
    loop();
    ASSERT_STREQ(host_clipboard_get(), "");
    ARROW_LEFT(SHIFT);
    loop();
    ASSERT_NE(chord(HID_KEY_X, GUI), HOSTKEY_CHORD);
    ASSERT_STREQ(host_clipboard_get(), "");
    hostkey_press(HID_KEY_ENTER, 0, false, true);
    loop();
    ASSERT_STREQ(line_got, "abc dex");
}

/* The paste keys paste the clipboard in place of the line's selection:
 * what they paste is typed, and typing replaces the selection. */
UTEST(mouse, the_paste_keys_replace_the_line_s_selection)
{
    editing();
    ASSERT_TRUE(host_clipboard_set("XY"));
    ARROW_LEFT(SHIFT);
    ARROW_LEFT(SHIFT);
    loop();
    ASSERT_EQ(chord(HID_KEY_V, CTRL | SHIFT), HOSTKEY_CHORD);
    loop();
    hostkey_press(HID_KEY_ENTER, 0, false, true);
    loop();
    ASSERT_EQ(line_count, 1);
    ASSERT_STREQ(line_got, "abc dXY");
}

/* A cut or a paste of the line's selection acts after the keys typed
 * before it: here a Shift-Left still unread when the key is pressed, which
 * takes the selection on to "def" first. */
UTEST(mouse, a_cut_and_a_paste_follow_the_keys_before_them)
{
    editing();
    ASSERT_TRUE(host_clipboard_set(""));
    ARROW_LEFT(SHIFT);
    ARROW_LEFT(SHIFT);
    loop();
    ARROW_LEFT(SHIFT);
    ASSERT_EQ(hostkey_press(HID_KEY_DELETE, SHIFT, false, true), HOSTKEY_CHORD);
    loop();
    ASSERT_STREQ(host_clipboard_get(), "def");
    typed_push("def", 3);
    ARROW_LEFT(SHIFT);
    ARROW_LEFT(SHIFT);
    loop();
    ASSERT_TRUE(host_clipboard_set("XY"));
    ARROW_LEFT(SHIFT);
    ASSERT_EQ(chord(HID_KEY_V, CTRL | SHIFT), HOSTKEY_CHORD);
    loop();
    hostkey_press(HID_KEY_ENTER, 0, false, true);
    loop();
    ASSERT_EQ(line_count, 1);
    ASSERT_STREQ(line_got, "abc XY");
}

/* What a paste key pastes is typed after the keys typed before it and
 * before the keys typed after it, in a read and with none. */
UTEST(mouse, a_paste_is_typed_between_the_keys_round_it)
{
    editing();
    ASSERT_TRUE(host_clipboard_set("XY"));
    typed_push("g", 1);
    ASSERT_EQ(chord(HID_KEY_V, CTRL | SHIFT), HOSTKEY_CHORD);
    typed_push("h", 1);
    ASSERT_EQ(chord(HID_KEY_INSERT, SHIFT), HOSTKEY_CHORD);
    hostkey_press(HID_KEY_ENTER, 0, false, true);
    loop();
    ASSERT_EQ(line_count, 1);
    ASSERT_STREQ(line_got, "abc defgXYhXY");
    typed_push("g", 1);
    ASSERT_EQ(chord(HID_KEY_V, CTRL | SHIFT), HOSTKEY_CHORD);
    typed_push("h", 1);
    ASSERT_STREQ(typed(), "gXYh");
}

/* A paste pressed with the line's selection showing and an Enter still
 * unread goes after the Enter, as typing does: the line is entered as it
 * is, and what is pasted is for whatever reads next. */
UTEST(mouse, a_paste_goes_after_an_enter_typed_before_it)
{
    editing();
    ASSERT_TRUE(host_clipboard_set("XY"));
    ARROW_LEFT(SHIFT);
    loop();
    hostkey_press(HID_KEY_ENTER, 0, false, true);
    ASSERT_EQ(chord(HID_KEY_V, CTRL | SHIFT), HOSTKEY_CHORD);
    loop();
    ASSERT_EQ(line_count, 1);
    ASSERT_STREQ(line_got, "abc def");
    ASSERT_STREQ(typed(), "XY");
}

/* The mouse and a paste key that come while the reply to the key before
 * them is still going out at a slow speed wait for that reply as typing
 * does, since the reply reads the line as it goes: here an 'x' typed at
 * the start of a long line, whose rewrite of the line is still going out,
 * then "bc" selected by the mouse and pasted over once the rewrite is
 * out. */
UTEST(mouse, a_paste_key_waits_for_a_slow_echo)
{
    editing();
    typed_push("ghijklmnopqrstuvwxyzghijklmnopqrstuvwxyz", 40);
    loop();
    show("\33[0;1*r");
    manifold_set_paced(true);
    hostkey_press(HID_KEY_HOME, 0, false, true);
    typed_push("x", 1);
    loop();
    drag(4, 2, 6, 2);
    ASSERT_TRUE(host_clipboard_set("XY"));
    ASSERT_EQ(chord(HID_KEY_V, CTRL | SHIFT), HOSTKEY_CHORD);
    loop();
    ASSERT_TRUE(rln_replying());
    ASSERT_FALSE(rln_selected());
    ASSERT_EQ(typed_free(), (size_t)TYPED_QUEUE_SIZE - 5);
    manifold_set_paced(false);
    show("\33[0;0*r");
    loop();
    ASSERT_FALSE(rln_replying());
    const char *want = "> xaXY defghijklmnopqrstuvwxyzghijklmnopqrstuvwxyz ";
    for (int x = 0; want[x]; x++)
        ASSERT_EQ(tb_screen_row(2)[x].glyph, (uint16_t)want[x]);
    hostkey_press(HID_KEY_ENTER, 0, false, true);
    loop();
    ASSERT_EQ(line_count, 1);
    ASSERT_STREQ(line_got, "xaXY defghijklmnopqrstuvwxyzghijklmnopqrstuvwxyz");
}

/* While a line is read, a change of the mouse's the terminal takes waits
 * for room in the keyboard's queue before anything of it is done, so a
 * press in the line is never lost to a full queue. */
UTEST(mouse, the_mouse_waits_for_room_in_a_full_queue)
{
    editing();
    char fill[TYPED_QUEUE_SIZE - 3];
    memset(fill, '\1', sizeof fill);
    typed_push(fill, sizeof fill);
    edge(4, 2, TABLET_FLAG_LEFT);
    ASSERT_EQ(RING_LEN(&vterm_mouse_queue), 1u);
    ASSERT_EQ(typed_free(), (size_t)3);
    loop();
    ASSERT_EQ(RING_LEN(&vterm_mouse_queue), 0u);
    edge(4, 2, 0);
    typed_push("X\r", 2);
    loop();
    ASSERT_STREQ(line_got, "abXc def");
}

/* The wheel's keys go into the queue before the room for the rest of the
 * change is judged. Here a notch of alternate scroll comes with the motion
 * of a drag in the line, and its keys leave room for one entry where the
 * drag needs two: the keys are pressed once, the drag waits whole, and it
 * selects when the queue has room again. */
UTEST(mouse, the_wheel_s_keys_count_against_the_mouse_s_room)
{
    start();
    show("\33[?1007;1049h> ");
    line_count = 0;
    line_got[0] = 0;
    rln_read_line(line_read, NULL, &line_kills);
    loop();
    typed_push("abc def", 7);
    loop();
    edge(4, 0, 0);
    edge(4, 0, TABLET_FLAG_LEFT);
    edge(7, 0, TABLET_FLAG_LEFT);
    loop();
    char fill[TYPED_QUEUE_SIZE - 16];
    memset(fill, '\1', sizeof fill);
    typed_push(fill, sizeof fill);
    tablet_host_pointer(8 * 8, 7, TABLET_FLAG_LEFT, true);
    tablet_host_wheel(1, 0);
    vterm_mouse_task();
    ASSERT_EQ(typed_free(), (size_t)1);
    ASSERT_EQ(RING_LEN(&vterm_mouse_queue), 1u);
    loop();
    ASSERT_EQ(RING_LEN(&vterm_mouse_queue), 0u);
    edge(8, 0, 0);
    loop();
    ASSERT_STREQ(primary(), "c de");
    show("\33[?1049;1007l");
}

/* ---- reports (step 31) ---------------------------------------------------- */

/* X10's events tell a press of a button, with no release and no motion,
 * and nothing is selected. */
UTEST(mouse, x10_events_tell_a_press)
{
    start();
    show(TEXT "\33[?9h");
    drag(2, 1, 5, 1);
    at(5, 1, TABLET_FLAG_RIGHT);
    at(5, 1, 0);
    ASSERT_STREQ(typed(), "\33[M #\"\33[M\"&\"");
    ASSERT_FALSE(vterm_selection.shows);
}

/* The normal events tell a press and its release, and no motion. */
UTEST(mouse, normal_events_tell_a_press_and_its_release)
{
    start();
    show(TEXT "\33[?1000h");
    drag(2, 1, 5, 1);
    ASSERT_STREQ(typed(), "\33[M #\"\33[M#&\"");
    ASSERT_FALSE(vterm_selection.shows);
}

/* The button events tell motion with a button held, once a cell, and a
 * drag out of mode 0 goes on at its last row and column. */
UTEST(mouse, button_events_tell_a_drag)
{
    start();
    show(TEXT "\33[?1002h");
    at(2, 1, 0);
    at(2, 1, TABLET_FLAG_LEFT);
    tablet_host_pointer(2 * 8 + 5, 16 + 7, TABLET_FLAG_LEFT, true);
    vterm_mouse_task();
    at(5, 1, TABLET_FLAG_LEFT);
    tablet_host_pointer(700, 600, TABLET_FLAG_LEFT, true);
    vterm_mouse_task();
    tablet_host_pointer(700, 600, 0, true);
    vterm_mouse_task();
    ASSERT_STREQ(typed(), "\33[M #\"\33[M@&\"\33[M@p>\33[M#p>");
}

/* The any events tell every motion, in SGR's form here, and the pixel
 * form puts the pointer's pixel in them. A press somewhere new is motion
 * there first. */
UTEST(mouse, any_events_tell_every_motion)
{
    start();
    show(TEXT "\33[?1003;1006h");
    at(2, 1, 0);
    at(3, 1, 0);
    at(3, 1, TABLET_FLAG_LEFT);
    at(3, 1, 0);
    ASSERT_STREQ(typed(), "\33[<35;3;2M\33[<35;4;2M\33[<0;4;2M\33[<0;4;2m");
    show("\33[?1016h");
    at(4, 1, 0);
    tablet_host_pointer(4 * 8 + 4, 16 + 7, 0, true);
    vterm_mouse_task();
    ASSERT_STREQ(typed(), "\33[<35;36;24M\33[<35;37;24M");
    show("\33[?1015h");
    click(0, 0);
    ASSERT_STREQ(typed(), "\33[67;1;1M\33[32;1;1M\33[35;1;1M");
}

/* The wheel is told a notch at a time, as buttons 4 to 7, from the normal
 * events on, and the terminal claims it only then. */
UTEST(mouse, the_wheel_is_told_a_notch_at_a_time)
{
    start();
    show(TEXT);
    at(2, 1, 0);
    ASSERT_FALSE(tablet_wheel_claimed());
    tablet_host_wheel(1, 0);
    vterm_mouse_task();
    ASSERT_STREQ(typed(), "");
    ASSERT_FALSE(vterm_selection.shows);
    show("\33[?9h");
    ASSERT_FALSE(tablet_wheel_claimed());
    tablet_host_wheel(1, 0);
    vterm_mouse_task();
    ASSERT_STREQ(typed(), "");
    show("\33[?1000h");
    ASSERT_TRUE(tablet_wheel_claimed());
    tablet_host_wheel(2, -1);
    vterm_mouse_task();
    tablet_host_wheel(-1, 1);
    vterm_mouse_task();
    ASSERT_STREQ(typed(), "\33[M`#\"\33[M`#\"\33[Mb#\"\33[Ma#\"\33[Mc#\"");
}

/* A report goes in whole or waits, with the edges after it, until the
 * keys before it are read; then they all go, in order. */
UTEST(mouse, a_full_queue_keeps_the_edges)
{
    start();
    show(TEXT "\33[?1000h");
    static char fill[TYPED_QUEUE_SIZE];
    memset(fill, 'k', sizeof fill);
    typed_push(fill, typed_free() - 7);
    at(2, 1, TABLET_FLAG_LEFT);
    at(2, 1, 0);
    at(2, 1, TABLET_FLAG_RIGHT);
    at(2, 1, 0);
    ASSERT_EQ(typed_free(), (size_t)1);
    const char *got = typed();
    ASSERT_EQ(strlen(got), TYPED_QUEUE_SIZE - 1);
    ASSERT_STREQ(got + TYPED_QUEUE_SIZE - 7, "\33[M #\"");
    vterm_mouse_task();
    ASSERT_STREQ(typed(), "\33[M##\"\33[M\"#\"\33[M##\"");
}

/* The longest report there is on the console, a drag of the fifth button
 * with Ctrl and Alt held past the corner in pixels, goes in whole once
 * there is room for all of it. Shift is never told, so 185 is the largest
 * code. */
UTEST(mouse, the_longest_report_arrives_whole)
{
    start();
    show(TEXT "\33[?1003;1016h");
    at(79, 29, TABLET_FLAG_BTN5);
    ASSERT_STREQ(typed(), "\33[<35;636;472M\33[<129;636;472M");
    static char fill[TYPED_QUEUE_SIZE];
    memset(fill, 'k', sizeof fill);
    const char *want = "\33[<185;640;480M";
    typed_push(fill, typed_free() - (strlen(want) - 1));
    keyboard_set_key(HID_KEY_CONTROL_LEFT, true);
    keyboard_set_key(HID_KEY_ALT_LEFT, true);
    tablet_host_pointer(700, 600, TABLET_FLAG_BTN5, true);
    keyboard_set_key(HID_KEY_CONTROL_LEFT, false);
    keyboard_set_key(HID_KEY_ALT_LEFT, false);
    vterm_mouse_task();
    ASSERT_EQ(typed_free(), strlen(want) - 1);
    char drop[TYPED_QUEUE_SIZE];
    ASSERT_EQ(com_read(COM_SOURCE_KEYBOARD, drop, 1), (size_t)1);
    vterm_mouse_task();
    const char *got = typed();
    ASSERT_STREQ(got + strlen(got) - strlen(want), want);
}

/* With a mouse mode set, Shift makes a press the terminal's, never told:
 * Shift with the left button selects, and with the right extends. */
UTEST(mouse, shift_selects_under_a_mouse_mode)
{
    start();
    show(TEXT "\33[?1000h");
    keyboard_set_key(HID_KEY_SHIFT_LEFT, true);
    drag(6, 0, 11, 0);
    ASSERT_TRUE(selected(6, 0, 11, 0));
    edge(15, 0, TABLET_FLAG_RIGHT);
    edge(15, 0, 0);
    ASSERT_TRUE(selected(6, 0, 15, 0));
    keyboard_set_key(HID_KEY_SHIFT_LEFT, false);
    ASSERT_STREQ(typed(), "");
    ASSERT_STREQ(primary(), "world foo");
}

/* A program that maps the tablet and clears its share bit is told nothing
 * by the terminal: the clicks are its own. */
UTEST(mouse, a_program_that_does_not_share_is_told_nothing)
{
    start();
    show(TEXT "\33[?1000h");
    ASSERT_TRUE(xreg0(0, 3, 0xFF00));
    xram[0xFF00 + TABLET_OFF_STATUS] = 0;
    drag(6, 0, 11, 0);
    ASSERT_STREQ(typed(), "");
    xram[0xFF00 + TABLET_OFF_STATUS] = TABLET_STATUS_SHARED;
    drag(6, 0, 11, 0);
    ASSERT_STREQ(typed(), "\33[M '!\33[M#,!");
    ASSERT_TRUE(xreg0(0, 3, 0xFFFF));
}

/* Under a mouse mode the line editor's line is the program's too: a click
 * there is told, and Shift selects the primary, never in the line. */
UTEST(mouse, the_line_s_selection_stays_off_under_a_mouse_mode)
{
    editing();
    show("\33[?1000h");
    drag(2, 2, 5, 2);
    ASSERT_STREQ(typed(), "\33[M ##\33[M#&#");
    keyboard_set_key(HID_KEY_SHIFT_LEFT, true);
    drag(2, 2, 5, 2);
    keyboard_set_key(HID_KEY_SHIFT_LEFT, false);
    loop();
    ASSERT_TRUE(selected(2, 2, 5, 2));
    ASSERT_STREQ(line_copy(), "");
}

/* ---- highlight tracking (step 32) ---------------------------------------- */

/* Highlight tracking tells a first click and waits for the program's
 * answer; an answer that starts the highlight makes the drag highlight from
 * where it said, and the release tells where it ends, CSI t, when it starts
 * where the program said and ends at the pointer. The highlight then goes,
 * and the primary takes what it highlighted, as xterm's does. */
UTEST(mouse, highlight_tracking_tells_where_the_highlight_ends)
{
    start();
    show(TEXT);
    drag(0, 0, 5, 0);
    show("\33[?1001h");
    machine_us(300 * 1000);
    at(2, 1, 0);
    at(2, 1, TABLET_FLAG_LEFT);
    ASSERT_STREQ(typed(), "\33[M #\"");
    show("\33[1;3;2;1;10T");
    ASSERT_STREQ(row(1), "second line");
    at(6, 1, TABLET_FLAG_LEFT);
    ASSERT_TRUE(selected(2, 1, 6, 1));
    at(6, 1, 0);
    ASSERT_STREQ(typed(), "\33[t'\"");
    ASSERT_FALSE(vterm_selection.shows);
    ASSERT_STREQ(primary(), "cond");
}

/* A drag back past where the program started the highlight ends where it
 * started, so the release tells all of it, CSI T: its start, its end and
 * the pointer, in the form the terminal is in. The drag is held to the rows
 * the program said, the last before the one it named, and past a row's
 * text it ends at the start of the next row, as a selection does. A press
 * after the click time is a first click again, told and answered. */
UTEST(mouse, highlight_tracking_tells_the_whole_highlight)
{
    start();
    show(TEXT "\33[?1001h");
    at(6, 1, TABLET_FLAG_LEFT);
    ASSERT_STREQ(typed(), "\33[M '\"");
    show("\33[1;7;2;2;3T");
    at(2, 0, TABLET_FLAG_LEFT);
    ASSERT_TRUE(selected(2, 1, 6, 1));
    at(2, 0, 0);
    ASSERT_STREQ(typed(), "\33[T#\"'\"#\"");
    show("\33[?1006h");
    machine_us(300 * 1000);
    at(6, 1, TABLET_FLAG_LEFT);
    ASSERT_STREQ(typed(), "\33[<0;7;2M");
    show("\33[1;7;2;1;10T");
    at(1, 3, TABLET_FLAG_LEFT);
    at(1, 3, 0);
    ASSERT_STREQ(typed(), "\33[<7;2;1;5;2;4T");
}

/* A press the program does not answer, or answers with 0, is a normal
 * event: its release is told, and nothing is highlighted. CSI 0 T is the
 * answer, not a scroll. */
UTEST(mouse, highlight_tracking_with_no_answer_is_a_normal_event)
{
    start();
    show(TEXT "\33[?1001h");
    drag(2, 1, 5, 1);
    ASSERT_STREQ(typed(), "\33[M #\"\33[M#&\"");
    ASSERT_FALSE(vterm_selection.shows);
    at(2, 1, TABLET_FLAG_LEFT);
    show("\33[0;3;2;1;10T");
    ASSERT_STREQ(row(0), "hello world foo.bar");
    at(5, 1, TABLET_FLAG_LEFT);
    ASSERT_FALSE(vterm_selection.shows);
    at(5, 1, 0);
    ASSERT_STREQ(typed(), "\33[M #\"\33[M#&\"");
    show("\33[1;3;2;1;10T");
    drag(2, 1, 5, 1);
    ASSERT_FALSE(vterm_selection.shows);
}

/* A highlight whose program stops tracking, by ?1001 reset or a RIS, or
 * changes to another mode, goes at the next change, which is told only as
 * the mode now set tells it: nothing reaches whatever reads the keys next. */
UTEST(mouse, highlight_tracking_ends_with_the_mode)
{
    start();
    show(TEXT "\33[?1001h");
    at(2, 1, TABLET_FLAG_LEFT);
    ASSERT_STREQ(typed(), "\33[M #\"");
    show("\33[1;3;2;1;10T");
    at(6, 1, TABLET_FLAG_LEFT);
    ASSERT_TRUE(vterm_selection.shows);
    show("\33[?1001l");
    at(6, 1, 0);
    ASSERT_STREQ(typed(), "");
    ASSERT_FALSE(vterm_selection.shows);
    show("\33[?1001h");
    at(2, 1, TABLET_FLAG_LEFT);
    ASSERT_STREQ(typed(), "\33[M #\"");
    show("\33[1;3;2;1;10T");
    show("\33c");
    at(6, 1, TABLET_FLAG_LEFT);
    ASSERT_FALSE(vterm_selection.shows);
    at(6, 1, 0);
    ASSERT_STREQ(typed(), "");
    show(TEXT "\33[?1001h");
    at(2, 1, TABLET_FLAG_LEFT);
    ASSERT_STREQ(typed(), "\33[M #\"");
    show("\33[1;3;2;1;10T\33[?1000h");
    at(6, 1, 0);
    ASSERT_STREQ(typed(), "\33[M#'\"");
    ASSERT_FALSE(vterm_selection.shows);
}

/* A press of the left button within the click time after a highlight's
 * release goes on from it, as xterm's TrackDown counts clicks, wherever it
 * is: it is told nothing and highlights at once, from where the program
 * started the first click's highlight, held to the rows the program said
 * then. The second goes by words, the third by lines, the fourth and the
 * fifth by characters and the sixth by words again, as xterm's default
 * clicks go. Each release tells all of the highlight, CSI T, or CSI t when
 * it starts there and ends at the pointer, and the primary takes it. A
 * press after the click time is a first click again, told and waiting for
 * the answer. */
UTEST(mouse, highlight_tracking_counts_clicks)
{
    start();
    show(TEXT "\33[?1001h");
    at(14, 0, TABLET_FLAG_LEFT);
    ASSERT_STREQ(typed(), "\33[M /!");
    show("\33[1;14;1;1;1T");
    at(14, 0, 0);
    ASSERT_STREQ(typed(), "\33[t/!");
    ASSERT_STREQ(primary(), "o");
    at(14, 0, TABLET_FLAG_LEFT);
    ASSERT_TRUE(selected(12, 0, 15, 0));
    ASSERT_STREQ(typed(), "");
    at(14, 0, 0);
    ASSERT_STREQ(typed(), "\33[T-!0!/!");
    ASSERT_FALSE(vterm_selection.shows);
    ASSERT_STREQ(primary(), "foo");
    at(14, 0, TABLET_FLAG_LEFT);
    ASSERT_TRUE(selected(0, 0, 0, 1));
    at(14, 1, TABLET_FLAG_LEFT);
    ASSERT_TRUE(selected(0, 0, 0, 1));
    at(14, 1, 0);
    ASSERT_STREQ(typed(), "\33[T!!!\"/!");
    ASSERT_STREQ(primary(), "hello world foo.bar\r");
    for (int i = 0; i < 2; i++)
    {
        at(14, 0, TABLET_FLAG_LEFT);
        ASSERT_TRUE(selected(13, 0, 14, 0));
        ASSERT_STREQ(typed(), "");
        at(14, 0, 0);
        ASSERT_STREQ(typed(), "\33[t/!");
        ASSERT_STREQ(primary(), "o");
    }
    at(5, 0, TABLET_FLAG_LEFT);
    ASSERT_TRUE(selected(5, 0, 15, 0));
    ASSERT_STREQ(typed(), "");
    at(5, 0, 0);
    ASSERT_STREQ(typed(), "\33[T&!0!&!");
    ASSERT_STREQ(primary(), " world foo");
    machine_us(300 * 1000);
    at(14, 1, TABLET_FLAG_LEFT);
    ASSERT_STREQ(typed(), "\33[M /\"");
    ASSERT_FALSE(vterm_selection.shows);
    at(14, 1, 0);
    ASSERT_STREQ(typed(), "\33[M#/\"");
}

/* A click goes on only from the release of a drag, a highlight's or a
 * selection's: the release of a press the program did not answer is none,
 * so after one the next press is a first click, told. A report of another
 * button between does not break the run, as in xterm. A selection made
 * with Shift goes on into highlighting, from where it started, and frees
 * the rows the program held the last highlight to. */
UTEST(mouse, highlight_tracking_counts_from_the_last_drag)
{
    start();
    show(TEXT "\33[?1001h");
    at(2, 1, TABLET_FLAG_LEFT);
    ASSERT_STREQ(typed(), "\33[M #\"");
    show("\33[1;3;2;2;3T");
    at(2, 1, 0);
    ASSERT_STREQ(typed(), "");
    machine_us(300 * 1000);
    click(2, 1);
    ASSERT_STREQ(typed(), "\33[M #\"\33[M##\"");
    at(2, 1, TABLET_FLAG_LEFT);
    ASSERT_STREQ(typed(), "\33[M #\"");
    show("\33[1;3;2;2;3T");
    at(6, 1, TABLET_FLAG_LEFT);
    ASSERT_TRUE(selected(2, 1, 6, 1));
    at(6, 1, 0);
    ASSERT_STREQ(typed(), "\33[t'\"");
    at(6, 1, TABLET_FLAG_RIGHT);
    at(6, 1, 0);
    ASSERT_STREQ(typed(), "\33[M\"'\"\33[M#'\"");
    at(4, 1, TABLET_FLAG_LEFT);
    ASSERT_STREQ(typed(), "");
    ASSERT_TRUE(selected(0, 1, 6, 1));
    at(4, 0, TABLET_FLAG_LEFT);
    ASSERT_TRUE(selected(0, 1, 6, 1));
    at(4, 0, 0);
    ASSERT_STREQ(typed(), "\33[T!\"'\"%\"");
    ASSERT_STREQ(primary(), "second");
    machine_us(300 * 1000);
    keyboard_set_key(HID_KEY_SHIFT_LEFT, true);
    drag(8, 1, 9, 1);
    keyboard_set_key(HID_KEY_SHIFT_LEFT, false);
    ASSERT_TRUE(selected(8, 1, 9, 1));
    ASSERT_STREQ(typed(), "");
    at(9, 1, TABLET_FLAG_LEFT);
    ASSERT_STREQ(typed(), "");
    ASSERT_TRUE(selected(7, 1, 11, 1));
    at(13, 0, TABLET_FLAG_LEFT);
    ASSERT_TRUE(selected(12, 0, 11, 1));
    at(13, 0, 0);
    ASSERT_STREQ(typed(), "\33[T-!,\".!");
    ASSERT_STREQ(primary(), "foo.bar\rsecond line");
}

/* A highlight held to rows past the last of mode 0's, which has become
 * fewer during the drag, is held to its last row. */
UTEST(mouse, highlight_tracking_holds_to_rows_that_are_left)
{
    start();
    show(TEXT "\33[?1001h");
    at(2, 1, TABLET_FLAG_LEFT);
    ASSERT_STREQ(typed(), "\33[M #\"");
    show("\33[1;3;2;20;26T");
    at(0, 1, TABLET_FLAG_LEFT);
    ASSERT_TRUE(selected(2, 1, 0, 19));
    /* Mode 0 on the console's canvas, its first 10 rows. */
    ASSERT_TRUE(xreg1(0, 0, vga_canvas_640_480));
    ASSERT_TRUE(xreg1(0, 3, 0) && xreg1(0, 4, 160) && xreg1(0, 1, 0));
    ASSERT_EQ(vterm_get_shown(&vterm)->height, 10);
    at(0, 5, TABLET_FLAG_LEFT);
    ASSERT_TRUE(selected(2, 1, 0, 9));
    at(0, 5, 0);
    ASSERT_STREQ(typed(), "\33[t!*");
}

/* ---- alternate scroll (step 34) ------------------------------------------ */

/* With alternate scroll, a notch of the wheel over the alternate screen
 * presses a cursor key five times, as the keys send it in the modes the
 * terminal is in; over the primary, without the mode, or with a mouse mode
 * set, it does not. */
UTEST(mouse, alternate_scroll_presses_the_cursor_keys)
{
    start();
    show(TEXT "\33[?1007h");
    at(2, 1, 0);
    ASSERT_FALSE(tablet_wheel_claimed());
    tablet_host_wheel(1, 0);
    vterm_mouse_task();
    ASSERT_STREQ(typed(), "");
    show("\33[?1049h");
    ASSERT_TRUE(tablet_wheel_claimed());
    tablet_host_wheel(1, 1);
    vterm_mouse_task();
    ASSERT_STREQ(typed(), "\33[A\33[A\33[A\33[A\33[A");
    show("\33[?1h");
    tablet_host_wheel(-2, 0);
    vterm_mouse_task();
    ASSERT_STREQ(typed(), "\33OB\33OB\33OB\33OB\33OB\33OB\33OB\33OB\33OB\33OB");
    show("\33[?2l");
    tablet_host_wheel(1, 0);
    vterm_mouse_task();
    ASSERT_STREQ(typed(), "\33A\33A\33A\33A\33A");
    show("\33<\33[?1l\33[?9h");
    ASSERT_FALSE(tablet_wheel_claimed());
    tablet_host_wheel(1, 0);
    vterm_mouse_task();
    ASSERT_STREQ(typed(), "");
    show("\33[?9l\33[?1007l");
    ASSERT_FALSE(tablet_wheel_claimed());
    tablet_host_wheel(1, 0);
    vterm_mouse_task();
    ASSERT_STREQ(typed(), "");
    show("\33[?1049l");
}

/* A notch waits whole for room behind the keys before it. */
UTEST(mouse, alternate_scroll_waits_for_room)
{
    start();
    show("\33[?1007;1049h");
    at(2, 1, 0);
    static char fill[TYPED_QUEUE_SIZE];
    memset(fill, 'k', sizeof fill);
    typed_push(fill, typed_free() - 14);
    tablet_host_wheel(1, 0);
    vterm_mouse_task();
    ASSERT_EQ(typed_free(), (size_t)14);
    const char *got = typed();
    ASSERT_EQ(strlen(got), TYPED_QUEUE_SIZE - 14);
    vterm_mouse_task();
    ASSERT_STREQ(typed(), "\33[A\33[A\33[A\33[A\33[A");
    show("\33[?1049;1007l");
}

/* ---- the DEC locator (step 35) ------------------------------------------- */

/* The locator tells the buttons the program chose, going down and coming
 * up, with the buttons held after each and where it is, in cells from 1;
 * and where it is when asked. Nothing is selected. */
UTEST(mouse, the_locator_tells_the_buttons_chosen)
{
    start();
    show(TEXT "\33[1'z");
    drag(2, 1, 5, 1);
    ASSERT_STREQ(typed(), "");
    show("\33[1;3'{");
    drag(2, 1, 5, 1);
    ASSERT_STREQ(typed(), "\33[2;4;2;3&w\33[3;0;2;6&w");
    at(5, 1, TABLET_FLAG_LEFT);
    at(5, 1, TABLET_FLAG_LEFT | TABLET_FLAG_RIGHT);
    ASSERT_STREQ(typed(), "\33[2;4;2;6&w\33[6;5;2;6&w");
    show("\33[2'{");
    at(5, 1, TABLET_FLAG_RIGHT);
    at(5, 1, 0);
    ASSERT_STREQ(typed(), "\33[3;1;2;6&w\33[7;0;2;6&w");
    ASSERT_FALSE(vterm_selection.shows);
    show("\33['|");
    ASSERT_STREQ(typed(), "\33[1;0;2;6&w");
    show("\33[1;1'z\33[1'|");
    ASSERT_STREQ(typed(), "\33[1;0;24;44&w");
    tablet_host_clear();
    vterm_mouse_task();
    show("\33['|");
    ASSERT_STREQ(typed(), "\33[0&w");
    show("\33['z\33['|");
    ASSERT_STREQ(typed(), "\33[0&w");
}

/* An answer to DECRQLP with no room waits whole for the keys before it to
 * be read, and the changes after it wait behind it. */
UTEST(mouse, a_locator_answer_waits_for_room)
{
    start();
    show(TEXT "\33[1'z");
    at(2, 1, 0);
    static char fill[TYPED_QUEUE_SIZE];
    memset(fill, 'k', sizeof fill);
    typed_push(fill, typed_free() - 4);
    show("\33['|");
    ASSERT_EQ(typed_free(), (size_t)4);
    at(3, 1, 0);
    const char *got = typed();
    ASSERT_EQ(strlen(got), TYPED_QUEUE_SIZE - 4);
    vterm_mouse_task();
    ASSERT_STREQ(typed(), "\33[1;0;2;3&w");
}

/* Each DECRQLP is answered, as xterm's: answers with no room wait in
 * order, up to four, and one asked for past those is dropped, as an
 * answer with no room is. */
UTEST(mouse, locator_answers_wait_in_order)
{
    start();
    show(TEXT "\33[1'z");
    at(2, 1, 0);
    static char fill[TYPED_QUEUE_SIZE];
    memset(fill, 'k', sizeof fill);
    typed_push(fill, typed_free() - 4);
    show("\33['|\33['|\33[0'z\33['|\33[1'z\33['|\33['|");
    ASSERT_EQ(typed_free(), (size_t)4);
    ASSERT_EQ(strlen(typed()), TYPED_QUEUE_SIZE - 4);
    vterm_mouse_task();
    ASSERT_STREQ(typed(), "\33[1;0;2;3&w\33[1;0;2;3&w\33[0&w\33[1;0;2;3&w");
    vterm_mouse_task();
    ASSERT_STREQ(typed(), "");
}

/* A one-shot locator goes off with its first report, and the terminal has
 * the mouse again. */
UTEST(mouse, a_one_shot_locator_tells_once)
{
    start();
    show(TEXT "\33[2'z\33[1'{");
    click(2, 1);
    ASSERT_STREQ(typed(), "\33[2;4;2;3&w");
    ASSERT_EQ(vterm.mouse_event, TERM_MOUSE_OFF);
    drag(6, 0, 11, 0);
    ASSERT_TRUE(selected(6, 0, 11, 0));
    ASSERT_STREQ(typed(), "");
    show("\33[2'z\33['|\33['|");
    ASSERT_STREQ(typed(), "\33[1;0;1;12&w\33[0&w");
}

/* As xterm's, the one-shot setting outlives the locator, even through a
 * DECELR 0: a DECRQLP then is answered unavailable and turns off the mouse
 * events set since. A DECELR 1 ends the setting. */
UTEST(mouse, a_one_shot_locator_turns_off_the_mouse_events)
{
    start();
    show(TEXT "\33[2'z\33[1'{");
    click(2, 1);
    ASSERT_STREQ(typed(), "\33[2;4;2;3&w");
    show("\33[?1000h\33['|");
    ASSERT_STREQ(typed(), "\33[0&w");
    ASSERT_EQ(vterm.mouse_event, TERM_MOUSE_OFF);
    show("\33[2'z\33[0'z\33[?1002h\33['|");
    ASSERT_STREQ(typed(), "\33[0&w");
    ASSERT_EQ(vterm.mouse_event, TERM_MOUSE_OFF);
    show("\33[1'z\33['z\33[?1000h\33['|");
    ASSERT_STREQ(typed(), "\33[0&w");
    ASSERT_EQ(vterm.mouse_event, TERM_MOUSE_NORMAL);
    click(2, 1);
    ASSERT_STREQ(typed(), "\33[M #\"\33[M##\"");
}

/* As xterm's, the filter rectangle's edges in cells are held to the row and
 * column before the last, so the locator on the last row or column is
 * outside it. */
UTEST(mouse, the_filter_rectangle_ends_before_the_last_cell)
{
    start();
    show(TEXT "\33[1'z");
    at(79, 29, 0);
    show("\33[1;1;99;99'w");
    ASSERT_STREQ(typed(), "\33[10;0;30;80&w");
    at(78, 28, 0);
    show("\33[1;1;99;99'w");
    ASSERT_STREQ(typed(), "");
    at(79, 28, 0);
    ASSERT_STREQ(typed(), "\33[10;0;29;80&w");
    at(78, 28, 0);
    show("\33[1;1;99;99'w");
    at(78, 29, 0);
    ASSERT_STREQ(typed(), "\33[10;0;30;79&w");
}

/* The filter rectangle: the locator leaving it is told once, and an edge
 * left out is where the locator was. One the locator is already outside
 * is told at once. A button told while it is armed turns the locator off,
 * as a request for the position does. */
UTEST(mouse, the_filter_rectangle_tells_the_locator_leaving)
{
    start();
    show(TEXT "\33[1'z");
    at(2, 1, 0);
    show("\33[1;1;3;5'w");
    at(4, 2, 0);
    ASSERT_STREQ(typed(), "");
    at(5, 2, 0);
    ASSERT_STREQ(typed(), "\33[10;0;3;6&w");
    at(9, 9, 0);
    ASSERT_STREQ(typed(), "");
    show("\33['w");
    at(9, 9, 0);
    ASSERT_STREQ(typed(), "");
    at(9, 8, 0);
    ASSERT_STREQ(typed(), "\33[10;0;9;10&w");
    show("\33[20;20;25;25'w");
    ASSERT_STREQ(typed(), "\33[10;0;9;10&w");
    show("\33[1;3'{\33['w");
    click(9, 8);
    ASSERT_STREQ(typed(), "\33[2;4;9;10&w");
    ASSERT_EQ(vterm.mouse_event, TERM_MOUSE_OFF);
    show("\33[1'z\33['w\33['|");
    ASSERT_STREQ(typed(), "\33[0&w");
    ASSERT_EQ(vterm.mouse_event, TERM_MOUSE_OFF);
}

/* A button told while the locator is unavailable, here as the program
 * stops sharing the tablet, is told as unavailable and leaves the filter
 * rectangle armed, as xterm's SendLocatorPosition. */
UTEST(mouse, an_unavailable_button_leaves_the_filter_rectangle)
{
    start();
    show(TEXT "\33[1'z\33[3'{");
    ASSERT_TRUE(xreg0(0, 3, 0xFF00));
    xram[0xFF00 + TABLET_OFF_STATUS] = TABLET_STATUS_SHARED;
    at(2, 1, 0);
    show("\33[1;1;3;5'w");
    at(2, 1, TABLET_FLAG_LEFT);
    ASSERT_STREQ(typed(), "");
    xram[0xFF00 + TABLET_OFF_STATUS] = 0;
    at(2, 1, 0);
    ASSERT_STREQ(typed(), "\33[0&w");
    ASSERT_TRUE(vterm.locator.filter);
    ASSERT_EQ(vterm.mouse_event, TERM_MOUSE_LOCATOR);
    xram[0xFF00 + TABLET_OFF_STATUS] = TABLET_STATUS_SHARED;
    at(9, 9, 0);
    ASSERT_STREQ(typed(), "\33[10;0;10;10&w");
    ASSERT_TRUE(xreg0(0, 3, 0xFFFF));
}

/* ---- save states (step 37) ---------------------------------------------- */

static uint8_t blob[1 << 20];

/* A save, then a machine that has lost the console's mouse, its selection,
 * its buffers and its screen, then a load of the save. The host's
 * clipboard, set between them, stays as the host has it. */
static void round_trip(int *utest_result)
{
    ASSERT_EQ(sst_save(blob, sizeof blob, 0), (const char *)NULL);
    ASSERT_TRUE(host_clipboard_set("later"));
    vterm_mouse_reset();
    vterm_unselect();
    clip_reset();
    show("\33c");
    ASSERT_FALSE(vterm_selection.shows);
    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    ASSERT_STREQ(host_clipboard_get(), "later");
}

/* A load keeps a cut still to take in its place among the keys: the
 * Shift-Left typed before it is taken first, and the 'x' typed after it
 * comes behind it. */
UTEST(mouse, a_load_keeps_a_cut_in_its_place)
{
    start();
    show(TEXT);
    show("> ");
    rln_read_line(console_rln_reader(), NULL, console_rln_kill());
    loop();
    typed_push("abc def", 7);
    loop();
    ASSERT_TRUE(host_clipboard_set(""));
    ARROW_LEFT(SHIFT);
    ASSERT_EQ(hostkey_press(HID_KEY_DELETE, SHIFT, false, true), HOSTKEY_CHORD);
    typed_push("x", 1);
    ASSERT_EQ(sst_save(blob, sizeof blob, 0), (const char *)NULL);
    rln_read_cancel();
    typed_clear();
    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    loop();
    ASSERT_STREQ(host_clipboard_get(), "f");
    ASSERT_EQ(strncmp(rln_line(), "abc dex", 7), 0);
    rln_read_cancel();
}

/* A load in the middle of a drag goes on with it: the selection shows as it
 * did, the release fills the primary, and the drag's start is kept on its
 * text again, so a row that goes out at the top takes it to the first cell
 * of row 0, as without the load. */
UTEST(mouse, a_load_mid_drag_goes_on_with_it)
{
    start();
    show(TEXT);
    edge(6, 0, 0);
    edge(6, 0, TABLET_FLAG_LEFT);
    edge(4, 1, TABLET_FLAG_LEFT);
    ASSERT_TRUE(selected(6, 0, 4, 1));
    round_trip(utest_result);
    ASSERT_TRUE(selected(6, 0, 4, 1));
    show("\33[30;1H\n");
    edge(11, 0, TABLET_FLAG_LEFT);
    edge(11, 0, 0);
    ASSERT_TRUE(selected(0, 0, 11, 0));
    ASSERT_STREQ(primary(), "second line");
}

/* The press keeps its place in its character while output scrolls the
 * screen under the drag, and in a save state: a press in a character's
 * last pixel column still starts the drag on at the next character. */
UTEST(mouse, the_press_keeps_its_place_in_its_character)
{
    start();
    show(TEXT);
    tablet_host_pointer(6 * 8 + 7, 16 + 7, 0, true);
    vterm_mouse_task();
    tablet_host_pointer(6 * 8 + 7, 16 + 7, TABLET_FLAG_LEFT, true);
    vterm_mouse_task();
    show("\33[30;1H\n");
    edge(9, 0, TABLET_FLAG_LEFT);
    ASSERT_TRUE(selected(7, 0, 9, 0));
    round_trip(utest_result);
    edge(10, 0, TABLET_FLAG_LEFT);
    ASSERT_TRUE(selected(7, 0, 10, 0));
    tablet_host_pointer(6 * 8 + 6, 7, TABLET_FLAG_LEFT, true);
    vterm_mouse_task();
    ASSERT_TRUE(selected(6, 0, 7, 0));
    edge(10, 0, 0);
    ASSERT_STREQ(primary(), "lin");
}

/* A load between the clicks of a double click keeps the first, so the
 * second selects the word. */
UTEST(mouse, a_load_between_two_clicks_keeps_the_first)
{
    start();
    show(TEXT);
    click(8, 0);
    round_trip(utest_result);
    click(8, 0);
    ASSERT_TRUE(selected(6, 0, 11, 0));
    ASSERT_STREQ(primary(), "world");
}

/* A load keeps the selection and the primary it filled, for the middle
 * button, and leaves the host's clipboard as the host has it. */
UTEST(mouse, a_load_keeps_the_selection_and_the_primary)
{
    start();
    show(TEXT);
    drag(6, 0, 11, 0);
    ASSERT_EQ(chord(HID_KEY_C, CTRL | SHIFT), HOSTKEY_CHORD);
    drag(0, 1, 6, 1);
    round_trip(utest_result);
    ASSERT_TRUE(selected(0, 1, 6, 1));
    at(30, 5, TABLET_FLAG_MIDDLE);
    at(30, 5, 0);
    ASSERT_STREQ(typed(), "second");
}

/* A drag in the line that has let go but that the line editor has still
 * to take keeps its release across a load, and then fills the primary.
 * The read is the console's, which a save can name. */
UTEST(mouse, a_load_keeps_the_line_s_release_still_to_take)
{
    start();
    show(TEXT);
    show("> ");
    rln_read_line(console_rln_reader(), NULL, console_rln_kill());
    loop();
    typed_push("abc def", 7);
    loop();
    edge(2, 2, TABLET_FLAG_LEFT);
    edge(5, 2, TABLET_FLAG_LEFT);
    edge(5, 2, 0);
    ASSERT_EQ(sst_save(blob, sizeof blob, 0), (const char *)NULL);
    rln_unselect();
    clip_reset();
    ASSERT_EQ(sst_load(blob, sst_size(), 0), (const char *)NULL);
    loop();
    ASSERT_STREQ(line_copy(), "abc");
    ASSERT_STREQ(primary(), "abc");
    rln_read_cancel();
}

UTEST_MAIN();
