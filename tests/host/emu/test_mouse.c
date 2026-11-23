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
#include "core/hid/typed.h"
#include "core/com/com.h"
#include "core/com/manifold.h"
#include "core/hid/clip.h"
#include "tb_seed.h"
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
 * raises no SIGINT; Cmd-C is one on a Mac. */
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
    /* Cmd-C copies on a Mac alone. */
    ASSERT_TRUE(host_clipboard_set(""));
    ASSERT_NE(chord(HID_KEY_C, GUI), HOSTKEY_CHORD);
    ASSERT_STREQ(host_clipboard_get(), "");
    tb_mac_keys = true;
    ASSERT_EQ(chord(HID_KEY_C, GUI), HOSTKEY_CHORD);
    tb_mac_keys = false;
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
        paste_task();
        rln_task();
    }
}

/* The text, then a read on row 2 after a prompt of two columns, with
 * "abc def" typed, the cursor at its end. */
static void editing(void)
{
    start();
    show(TEXT);
    show("> ");
    line_count = 0;
    line_got[0] = 0;
    rln_read_line(line_read, NULL);
    loop();
    typed_push("abc def", 7);
    loop();
}

static bool rev(int x, int y)
{
    return (tb_screen_row((uint8_t)y)[x].attributes & MODE0_ATTR_REVERSE) != 0;
}

/* What the line editor's selection copies, the clipboard emptied first. */
static const char *line_copy(void)
{
    host_clipboard_set("");
    rln_copy();
    return host_clipboard_get();
}

/* A drag in the line selects in it, a click there ends that, and neither
 * touches the primary. */
UTEST(mouse, a_drag_in_the_line_selects_in_the_line)
{
    editing();
    drag(0, 1, 6, 1);
    ASSERT_TRUE(vterm_selection.shows);
    drag(2, 2, 5, 2);
    loop();
    ASSERT_FALSE(vterm_selection.shows);
    ASSERT_STREQ(line_copy(), "abc");
    ASSERT_TRUE(rev(2, 2) && rev(4, 2));
    ASSERT_FALSE(rev(5, 2));
    ASSERT_STREQ(primary(), "second");
    click(3, 2);
    loop();
    ASSERT_FALSE(rev(2, 2));
    ASSERT_STREQ(line_copy(), "");
}

/* A drag that leaves the line stops at its first or last character. */
UTEST(mouse, a_drag_out_of_the_line_stops_at_its_ends)
{
    editing();
    drag(4, 2, 10, 0);
    loop();
    ASSERT_STREQ(line_copy(), "ab");
    ASSERT_FALSE(vterm_selection.shows);
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
    ASSERT_TRUE(rev(2, 2));
    drag(0, 1, 6, 1);
    loop();
    ASSERT_TRUE(vterm_selection.shows);
    ASSERT_FALSE(rev(2, 2));
    ASSERT_STREQ(line_copy(), "");
    hostkey_press(HID_KEY_ARROW_LEFT, KEYBOARD_MODIFIER_LEFTSHIFT, false, true);
    loop();
    ASSERT_FALSE(vterm_selection.shows);
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

/* A press while the line's echo is still going out at a slow speed, with
 * no key waiting, is judged against the line as the line editor has laid
 * it out, and selects at once: here an 'x' typed at the start of a long
 * line, whose rewrite of the line is still going out. */
UTEST(mouse, a_press_during_a_slow_echo_selects_in_the_line)
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
    ASSERT_STREQ(line_copy(), "bc");
    manifold_set_paced(false);
    show("\33[0;0*r");
    loop();
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
    rln_read_line(line_read, NULL);
    loop();
    typed_push("xyz", 3);
    loop();
    at(4, 3, TABLET_FLAG_LEFT);
    at(4, 3, 0);
    loop();
    ASSERT_FALSE(rev(2, 3));
    ASSERT_STREQ(line_copy(), "");
    ASSERT_FALSE(vterm_selection.shows);
}

#define ARROW_LEFT(m) hostkey_press(HID_KEY_ARROW_LEFT, (m), false, true)

/* A copy key copies at once what is selected as it is pressed, keys typed
 * before it still unread: the primary, here past a typed key no program
 * reads. */
UTEST(mouse, a_copy_of_the_primary_does_not_wait_for_typed_keys)
{
    start();
    show(TEXT);
    drag(6, 0, 11, 0);
    typed_push("abc", 3);
    ASSERT_TRUE(host_clipboard_set(""));
    ASSERT_EQ(chord(HID_KEY_C, CTRL | SHIFT), HOSTKEY_CHORD);
    ASSERT_STREQ(host_clipboard_get(), "world");
    drag(0, 1, 6, 1);
    ASSERT_EQ(chord(HID_KEY_INSERT, CTRL), HOSTKEY_CHORD);
    ASSERT_STREQ(host_clipboard_get(), "second");
    ASSERT_STREQ(typed(), "abc");
}

/* The copy keys copy the line's selection showing at once, and with none
 * in the line, the primary. A selection that keys still unread would make
 * is not the one showing, so the primary is copied. */
UTEST(mouse, the_copy_keys_copy_the_line_s_selection)
{
    editing();
    ARROW_LEFT(SHIFT);
    ARROW_LEFT(SHIFT);
    loop();
    ASSERT_TRUE(host_clipboard_set(""));
    ASSERT_EQ(chord(HID_KEY_C, CTRL | SHIFT), HOSTKEY_CHORD);
    ASSERT_STREQ(host_clipboard_get(), "ef");
    ARROW_LEFT(0);
    drag(0, 1, 6, 1);
    loop();
    ASSERT_EQ(chord(HID_KEY_INSERT, CTRL), HOSTKEY_CHORD);
    ASSERT_STREQ(host_clipboard_get(), "second");
    ARROW_LEFT(SHIFT);
    ASSERT_EQ(chord(HID_KEY_INSERT, CTRL), HOSTKEY_CHORD);
    ASSERT_STREQ(host_clipboard_get(), "second");
    loop();
    ASSERT_EQ(chord(HID_KEY_INSERT, CTRL), HOSTKEY_CHORD);
    ASSERT_STREQ(host_clipboard_get(), " ");
}

/* Shift-Delete cuts the selection showing to the clipboard, and with
 * none, is no chord and deletes as Delete does. */
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
    ARROW_LEFT(0);
    ASSERT_EQ(hostkey_press(HID_KEY_DELETE, SHIFT, false, true), HOSTKEY_TYPED);
    hostkey_press(HID_KEY_ENTER, 0, false, true);
    loop();
    ASSERT_EQ(line_count, 1);
    ASSERT_STREQ(line_got, "abc ");
    ASSERT_STREQ(host_clipboard_get(), "ef");
}

/* The paste keys paste the clipboard in place of the selection showing. */
UTEST(mouse, the_paste_keys_replace_the_line_s_selection)
{
    editing();
    ASSERT_TRUE(host_clipboard_set("XY"));
    ARROW_LEFT(SHIFT);
    ARROW_LEFT(SHIFT);
    loop();
    ASSERT_EQ(chord(HID_KEY_V, CTRL | SHIFT), HOSTKEY_CHORD);
    ASSERT_FALSE(paste_busy());
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

/* With no line selection showing, a paste key pastes at once through the
 * paste stage, after the keys typed before it, in a read and with none. */
UTEST(mouse, a_paste_with_no_line_selection_pastes_at_once)
{
    editing();
    ASSERT_TRUE(host_clipboard_set("XY"));
    typed_push("g", 1);
    ASSERT_EQ(chord(HID_KEY_V, CTRL | SHIFT), HOSTKEY_CHORD);
    ASSERT_TRUE(paste_busy());
    typed_push("h", 1);
    hostkey_press(HID_KEY_ENTER, 0, false, true);
    loop();
    ASSERT_EQ(line_count, 1);
    ASSERT_STREQ(line_got, "abc defgXYh");
    ASSERT_EQ(chord(HID_KEY_V, CTRL | SHIFT), HOSTKEY_CHORD);
    ASSERT_TRUE(paste_busy());
    ASSERT_STREQ(typed(), "XY");
}

/* A paste of the line's selection still to take when the read ends, here
 * by an Enter typed before it, pastes as it would with no read. */
UTEST(mouse, a_paste_still_to_take_pastes_when_the_read_ends)
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

/* A paste key that comes while the reply to the key before it is still
 * going out at a slow speed waits in the line editor for that reply, which
 * reads the line as it goes, before it edits the line: here an 'x' typed
 * at the start of a long line, whose rewrite of the line is still going
 * out, then "bc" selected by the mouse at once and pasted over once the
 * rewrite is out. */
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
    loop();
    ASSERT_STREQ(line_copy(), "bc");
    ASSERT_TRUE(host_clipboard_set("XY"));
    ASSERT_EQ(chord(HID_KEY_V, CTRL | SHIFT), HOSTKEY_CHORD);
    loop();
    ASSERT_TRUE(rln_replying());
    ASSERT_FALSE(paste_busy());
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
    at(15, 0, TABLET_FLAG_RIGHT);
    at(15, 0, 0);
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
    ASSERT_FALSE(rev(2, 2));
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

UTEST_MAIN();
