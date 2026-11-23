/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The console's mouse selecting (core/term/vterm_mouse.h), and the copy and
 * paste keys (core/hid/key.h), from the tablet's host pointer and the host's
 * keys to the selection, the primary and the clipboard. */

#include "core/api/xreg.h"
#include "core/com/com.h"
#include "core/com/manifold.h"
#include "core/hid/clip.h"
#include "core/hid/hostkey.h"
#include "core/hid/keyboard.h"
#include "core/hid/paste.h"
#include "core/hid/tablet.h"
#include "core/hid/usage.h"
#include "core/sys/ria.h"
#include "core/sys/sys.h"
#include "core/sys/timer.h"
#include "core/sys/xram.h"
#include "core/term/vterm.h"
#include "core/term/vterm_mouse.h"
#include "core/vga/vga_emu.h"
#include "host/host.h"
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

/* A drag with the left button from one cell to another. */
static void drag(int x0, int y0, int x1, int y1)
{
    at(x0, y0, 0);
    at(x0, y0, TABLET_FLAG_LEFT);
    at(x1, y1, TABLET_FLAG_LEFT);
    at(x1, y1, 0);
}

/* A click with the left button, as quick as the mouse can. */
static void click(int x, int y)
{
    at(x, y, TABLET_FLAG_LEFT);
    at(x, y, 0);
}

/* The machine's clock moves on by us, which runs the beam. */
static void machine_us(uint64_t us)
{
    timer_mach_t d = timer_mach_in_us(us);
    while (!timer_mach_passed(d))
        vga_task();
}

/* All the keyboard row gives, the paste staged as the main loop stages it. */
static const char *typed(void)
{
    static char out[512];
    size_t got = 0;
    for (int i = 0; i < 10000 && got < sizeof out - 1; i++)
    {
        paste_task();
        size_t n = com_read(COM_SOURCE_KEYBOARD, out + got, sizeof out - 1 - got);
        got += n;
        if (!n && !paste_busy())
            break;
    }
    out[got] = 0;
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

/* Past the end of a row's text is the start of the next row, so a drag
 * there takes the line end and none of the row's empty cells. */
UTEST(mouse, past_the_text_is_the_next_row)
{
    start();
    show(TEXT);
    drag(7, 1, 60, 1);
    ASSERT_TRUE(selected(7, 1, 0, 2));
    ASSERT_STREQ(primary(), "line\r");
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
    at(6, 1, 0);
    at(6, 1, TABLET_FLAG_LEFT);
    show("\33[30;1H\n");
    at(4, 3, TABLET_FLAG_LEFT);
    ASSERT_TRUE(selected(6, 0, 0, 4));
    show("\n\n\n");
    at(5, 3, TABLET_FLAG_LEFT);
    ASSERT_TRUE(selected(0, 0, 0, 4));
    for (int i = 0; i < 40; i++)
        show("\n");
    show("\33[15;1Hlate text");
    at(4, 14, TABLET_FLAG_LEFT);
    at(4, 14, 0);
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
    tablet_point_t pt = {6 * 8 + 3, 7};
    tablet_host_touch(&pt, 1);
    vterm_mouse_task();
    pt.x = 11 * 8 + 3;
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
    ASSERT_TRUE(selected(2, 0, 37, 25));
    /* Every row is a line, as xterm copies them: the first's text and the
     * empty ones after it. */
    char want[6 + 26];
    memcpy(want, "p row", 5);
    memset(want + 5, '\r', 26);
    want[31] = 0;
    ASSERT_STREQ(primary(), want);

    tablet_host_pointer(10, 8, TABLET_FLAG_LEFT, true);
    vterm_mouse_task();
    tablet_host_pointer(100, 100, TABLET_FLAG_LEFT, true);
    vterm_mouse_task();
    tablet_host_pointer(100, 100, 0, true);
    vterm_mouse_task();
    ASSERT_TRUE(selected(2, 0, 37, 25));
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
 * joins to the next, with the line end after it; a fourth goes back to
 * characters. */
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
    at(2, 0, TABLET_FLAG_RIGHT);
    at(2, 0, 0);
    ASSERT_TRUE(selected(2, 0, 11, 0));
    ASSERT_STREQ(primary(), "llo world");
    at(15, 0, TABLET_FLAG_RIGHT);
    at(15, 0, 0);
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
    at(6, 0, TABLET_FLAG_RIGHT);
    at(11, 0, TABLET_FLAG_RIGHT);
    at(11, 0, 0);
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

/* The copy keys put the primary in the host's clipboard, and Ctrl-Shift-C
 * raises no SIGINT. */
UTEST(mouse, the_copy_keys_copy_the_primary)
{
    start();
    show(TEXT);
    drag(6, 0, 11, 0);
    ria_get_sigint();
    ASSERT_TRUE(host_clipboard_set(""));
    ASSERT_EQ(chord(HID_KEY_C, CTRL | SHIFT), HOSTKEY_CHORD);
    ASSERT_FALSE(ria_get_sigint());
    ASSERT_STREQ(host_clipboard_get(), "world");
    ASSERT_STREQ(typed(), "");
    ASSERT_TRUE(host_clipboard_set(""));
    ASSERT_EQ(chord(HID_KEY_INSERT, CTRL), HOSTKEY_CHORD);
    ASSERT_STREQ(host_clipboard_get(), "world");
    ASSERT_TRUE(host_clipboard_set(""));
    ASSERT_EQ(chord(HID_KEY_C, GUI), HOSTKEY_CHORD);
    ASSERT_STREQ(host_clipboard_get(), "world");
    /* A held chord acts once. */
    ASSERT_TRUE(host_clipboard_set(""));
    ASSERT_EQ(hostkey_press(HID_KEY_C, CTRL | SHIFT, true, true), HOSTKEY_CHORD);
    ASSERT_STREQ(host_clipboard_get(), "");
}

/* With nothing selected yet, a copy key leaves the clipboard as it was. */
UTEST(mouse, a_copy_of_nothing_keeps_the_clipboard)
{
    start();
    clip_reset();
    ASSERT_STREQ(primary(), "");
    ASSERT_TRUE(host_clipboard_set("kept"));
    ASSERT_EQ(chord(HID_KEY_C, CTRL | SHIFT), HOSTKEY_CHORD);
    ASSERT_STREQ(host_clipboard_get(), "kept");
}

/* The paste keys paste the clipboard, bracketed under ?2004 and not
 * without it; plain Ctrl-V and Ctrl-C stay the program's. */
UTEST(mouse, the_paste_keys_paste_the_clipboard)
{
    start();
    ASSERT_TRUE(host_clipboard_set("text"));
    ASSERT_EQ(chord(HID_KEY_V, CTRL | SHIFT), HOSTKEY_CHORD);
    ASSERT_STREQ(typed(), "text");
    show("\33[?2004h");
    ASSERT_EQ(chord(HID_KEY_INSERT, SHIFT), HOSTKEY_CHORD);
    ASSERT_STREQ(typed(), "\33[200~text\33[201~");
    ASSERT_EQ(chord(HID_KEY_V, GUI), HOSTKEY_CHORD);
    ASSERT_STREQ(typed(), "\33[200~text\33[201~");
    show("\33[?2004l");
    ASSERT_EQ(chord(HID_KEY_V, CTRL), HOSTKEY_TYPED);
    ASSERT_STREQ(typed(), "\x16");
    ria_get_sigint();
    ASSERT_EQ(chord(HID_KEY_C, CTRL), HOSTKEY_TYPED);
    ASSERT_TRUE(ria_get_sigint());
    ASSERT_STREQ(typed(), "\x03");
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

UTEST_MAIN();
