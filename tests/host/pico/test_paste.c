/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* A paste from the machine's own buffers as the RIA has it: the buffers in
 * code page bytes (clip_byte.c), the text cells of its terminal, and the
 * keyboard row, KEYMAP_COM_SOURCE, reading through the paste stage in front
 * of a typed queue of 32 bytes, with keymap_abandon its clear. Keys are
 * typed through keymap.c and vtkey.c on a layout of the letters alone. */

#include "core/aud/bel.h"
#include "core/com/rln.h"
#include "core/hid/clip.h"
#include "core/hid/keyboard.h"
#include "core/hid/keymap.h"
#include "core/hid/layout.h"
#include "core/hid/paste.h"
#include "core/hid/typed.h"
#include "core/hid/usage.h"
#include "core/sys/timer.h"
#include "core/term/grid.h"
#include "core/term/vterm.h"
#include "core/term/vterm_mouse.h"
#include "utest.h"
#include <string.h>

/* The RIA's machine.h, which a host build cannot include. */
_Static_assert(TERM_MAX_HEIGHT == 32 && TERM_CELL_TEXT == 1 && TYPED_QUEUE_SIZE == 32,
               "the RIA's terminal and typed queue");

bool vga_connected(void) { return false; }
unsigned long vga_frame_count(void) { return 0; }
void com_reply(const char *s, size_t n) { (void)s, (void)n; }
/* The console's mouse is not in this test: what a program asks of it does
 * nothing. */
void vterm_mouse_track(int func, int col, int row, int first, int last)
{
    (void)func, (void)col, (void)row, (void)first, (void)last;
}
void vterm_mouse_locate(void) {}
void vterm_mouse_filter(void) {}
void draw_reset(void) {}
void draw_job_started(void) {}
void aud_bel_put(unsigned i, uint8_t val) { (void)i, (void)val; }
uint64_t host_clock_us(void) { return 1; }

static unsigned sigints;
void ria_trigger_sigint(void) { sigints++; }

/* What keymap.c and key.c ask of the keyboard, the USB host, the programs,
 * the line editor and the clock: no locks, no program's chords and no
 * repeat. The line editor's selection shows where line_selects; its copy
 * puts "line" in the clipboard, and its cut and paste keys are counted. */
bool hid_boot_enumerating(void) { return false; }
void hid_set_leds(uint8_t leds) { (void)leds; }
bool proc_key_chord(uint8_t keycode, bool ctrl, bool alt)
{
    (void)keycode, (void)ctrl, (void)alt;
    return false;
}
static bool line_selects;
static unsigned line_cuts, line_pastes;
bool rln_copy(void)
{
    if (line_selects)
        clip_copy_bytes(CLIP_CLIPBOARD, "line", 4);
    return line_selects;
}
bool rln_selected(void) { return line_selects; }
bool rln_cut(void)
{
    line_cuts += line_selects;
    return line_selects;
}
bool rln_paste(void)
{
    line_pastes += line_selects;
    return line_selects;
}
uint8_t keyboard_get_locks(void) { return 0; }
void keyboard_set_locks(uint8_t locks) { (void)locks; }
uint8_t keyboard_get_modifier(void) { return 0; }
bool keyboard_key_down(uint8_t keycode)
{
    (void)keycode;
    return false;
}
uint16_t oem_get_code_page_run(void) { return 437; }
timer_deadline_t timer_in_us(uint64_t us) { return us; }
bool timer_passed(timer_deadline_t d)
{
    (void)d;
    return false;
}

/* A layout of the letters, with no dead keys. */
uint16_t layout_code_point(int idx, uint8_t keycode, unsigned col)
{
    (void)idx;
    if (keycode < HID_KEY_A || keycode > HID_KEY_Z)
        return 0;
    return (uint16_t)(((col & LAYOUT_SHIFT) ? 'A' : 'a') + keycode - HID_KEY_A);
}
bool layout_use_caps(int idx, uint8_t keycode)
{
    (void)idx, (void)keycode;
    return true;
}
unsigned layout_dead2_count(int idx)
{
    (void)idx;
    return 0;
}
unsigned layout_dead3_count(int idx)
{
    (void)idx;
    return 0;
}
uint16_t layout_dead2(int idx, unsigned entry, unsigned field)
{
    (void)idx, (void)entry, (void)field;
    return 0;
}
uint16_t layout_dead3(int idx, unsigned entry, unsigned field)
{
    (void)idx, (void)entry, (void)field;
    return 0;
}
void layout_list_cycle(void) {}

/* The RIA's keyboard row. */
static const com_source_driver_t row = KEYMAP_COM_SOURCE;

static cell_t cells[VTERM_CELLS];

static void say(const char *s)
{
    while (*s)
        term_write(&vterm, (uint8_t)*s++);
}

static void fresh(void)
{
    vterm_init(cells);
    vterm_load_code_page(437);
    vterm_RIS();
    row.clear();
    clip_reset();
    sigints = 0;
    line_selects = false;
    line_cuts = line_pastes = 0;
}

static screen_t *shown(void)
{
    return vterm_get_shown(&vterm)->shown;
}

/* A key pressed with the modifiers of a HID report's byte. */
static void chord(uint8_t modifier, uint8_t keycode)
{
    keyboard_on_press(modifier, keycode);
}

/* Letters typed on the keyboard, a press each, Ctrl held where ctrl. */
static void press(const char *letters, bool ctrl)
{
    for (; *letters; letters++)
        keyboard_on_press(ctrl ? KEYBOARD_MODIFIER_LEFTCTRL : 0,
                          (uint8_t)(HID_KEY_A + *letters - 'a'));
}

/* What the keyboard row gives, the stage filled as the main loop fills it
 * between reads of a few bytes. */
static const char *keyboard(void)
{
    static char out[1024];
    size_t got = 0;
    for (int i = 0; i < 1000 && got + 5 < sizeof out; i++)
    {
        paste_task();
        got += row.read(out + got, 5);
        if (!paste_busy() && row.peek(0) < 0)
            break;
    }
    out[got] = 0;
    return out;
}

/* Keys typed before the paste go first, then the paste, bracketed, then
 * the keys typed while it went in, which wait in the queue behind it. */
UTEST(paste, the_primary_goes_whole_past_a_small_queue)
{
    fresh();
    say("\33[?2004hthe first line of the text\r\nand the second one");
    clip_copy(CLIP_PRIMARY, shown(), 0, 0, 1, 80);
    press("ab", false);
    clip_paste(CLIP_PRIMARY);
    ASSERT_EQ(row.peek(0), 'a');
    press("cd", false);
    ASSERT_STREQ(keyboard(), "ab\33[200~the first line of the text\rand the second one"
                             "\33[201~cd");
    ASSERT_FALSE(paste_busy());
}

/* Keys typed during a paste fill the queue behind it, and then are
 * dropped. */
UTEST(paste, keys_past_a_full_queue_are_dropped)
{
    fresh();
    say("0123456789012345678901234567890123456789");
    clip_copy(CLIP_PRIMARY, shown(), 0, 0, 0, 40);
    clip_paste(CLIP_PRIMARY);
    press("abcdefghijklmnopqrstuvwxyzabcdefg", false);
    ASSERT_STREQ(keyboard(), "0123456789012345678901234567890123456789"
                             "abcdefghijklmnopqrstuvwxyzabcdef");
}

/* A Ctrl-C raises the SIGINT at once and ends the paste: what was staged
 * goes, then the closing bracket, then the Ctrl-C. */
UTEST(paste, a_ctrl_c_ends_the_paste_with_its_closing_bracket)
{
    fresh();
    say("\33[?2004h0123456789012345678901234567890123456789");
    clip_copy(CLIP_PRIMARY, shown(), 0, 0, 0, 40);
    clip_paste(CLIP_PRIMARY);
    paste_task();
    char head[7] = {0};
    ASSERT_EQ(row.read(head, 6), (size_t)6);
    ASSERT_STREQ(head, "\33[200~");
    press("c", true);
    ASSERT_EQ(sigints, 1u);
    ASSERT_STREQ(keyboard(), "01234567890123456789012345678901\33[201~\3");
}

/* The row's clear drops the paste, its brackets and the keys behind it. */
UTEST(paste, the_row_s_clear_drops_the_paste)
{
    fresh();
    say("\33[?2004h0123456789012345678901234567890123456789");
    clip_copy(CLIP_PRIMARY, shown(), 0, 0, 0, 40);
    clip_paste(CLIP_PRIMARY);
    paste_task();
    press("ab", false);
    row.clear();
    ASSERT_FALSE(paste_busy());
    ASSERT_EQ(row.peek(0), -1);
    ASSERT_STREQ(keyboard(), "");
}

/* A copy into the buffer a paste is reading ends that paste, with its
 * closing bracket. */
UTEST(paste, a_copy_into_the_buffer_ends_its_paste)
{
    fresh();
    say("\33[?2004h0123456789012345678901234567890123456789");
    clip_copy(CLIP_PRIMARY, shown(), 0, 0, 0, 40);
    clip_paste(CLIP_PRIMARY);
    paste_task();
    clip_copy(CLIP_PRIMARY, shown(), 0, 0, 0, 2);
    ASSERT_STREQ(keyboard(), "\33[200~01234567890123456789012345678901\33[201~");
}

/* A paste started while another goes in cuts that one short, and starts
 * once all of it, its closing bracket too, has been read. */
UTEST(paste, a_second_paste_follows_the_first_one_s_closing_bracket)
{
    fresh();
    say("\33[?2004h0123456789012345678901234567890123456789");
    clip_copy(CLIP_PRIMARY, shown(), 0, 0, 0, 40);
    clip_copy(CLIP_CLIPBOARD, shown(), 0, 0, 0, 3);
    clip_paste(CLIP_PRIMARY);
    paste_task();
    clip_paste(CLIP_CLIPBOARD);
    ASSERT_STREQ(keyboard(), "\33[200~01234567890123456789012345678901\33[201~\33[200~012\33[201~");
}

/* An empty buffer pastes nothing, not even brackets, but still ends the
 * paste going in, as empty host text does. */
UTEST(paste, an_empty_buffer_pastes_nothing)
{
    fresh();
    say("\33[?2004h");
    clip_paste(CLIP_PRIMARY);
    press("ab", false);
    ASSERT_STREQ(keyboard(), "ab");
    say("0123456789012345678901234567890123456789");
    clip_copy(CLIP_PRIMARY, shown(), 0, 0, 0, 40);
    clip_paste(CLIP_PRIMARY);
    paste_task();
    clip_paste(CLIP_CLIPBOARD);
    ASSERT_STREQ(keyboard(), "\33[200~01234567890123456789012345678901\33[201~");
}

/* DEC Special Graphics is kept as the code page's nearest character. */
UTEST(paste, a_line_drawing_copies_as_the_code_page_s)
{
    fresh();
    say("\33(0lqk\33(B");
    clip_copy(CLIP_PRIMARY, shown(), 0, 0, 0, 3);
    clip_paste(CLIP_PRIMARY);
    ASSERT_STREQ(keyboard(), "\xDA\xC4\xBF");
}

#define CTRL KEYBOARD_MODIFIER_LEFTCTRL
#define SHIFT KEYBOARD_MODIFIER_LEFTSHIFT

/* The copy keys put the primary in the machine's clipboard and type
 * nothing, so Ctrl-Shift-C raises no SIGINT; the paste keys paste it,
 * bracketed under ?2004 and not without it. */
UTEST(paste, the_copy_and_paste_keys_through_the_keymap)
{
    fresh();
    say("copy me\r\n");
    clip_copy(CLIP_PRIMARY, shown(), 0, 0, 0, 7);
    chord(CTRL | SHIFT, HID_KEY_C);
    ASSERT_EQ(sigints, 0u);
    ASSERT_STREQ(keyboard(), "");
    chord(CTRL | SHIFT, HID_KEY_V);
    ASSERT_STREQ(keyboard(), "copy me");
    say("\33[?2004h");
    chord(SHIFT, HID_KEY_INSERT);
    ASSERT_STREQ(keyboard(), "\33[200~copy me\33[201~");
    clip_copy(CLIP_PRIMARY, shown(), 0, 0, 0, 4);
    chord(CTRL, HID_KEY_INSERT);
    chord(SHIFT, HID_KEY_INSERT);
    ASSERT_STREQ(keyboard(), "\33[200~copy\33[201~");
    say("\33[?2004l");
    press("c", true);
    ASSERT_EQ(sigints, 1u);
    ASSERT_STREQ(keyboard(), "\3");
}

/* The copy and paste keys act at once, with keys typed before them still
 * unread: a copy takes the primary as it is at the press, and a paste goes
 * between the keys typed before it and the keys typed after it. */
UTEST(paste, the_copy_and_paste_keys_act_at_once)
{
    fresh();
    say("copy me\r\n");
    clip_copy(CLIP_PRIMARY, shown(), 0, 0, 0, 7);
    press("ab", false);
    chord(CTRL | SHIFT, HID_KEY_C);
    clip_copy(CLIP_PRIMARY, shown(), 0, 0, 0, 4);
    chord(CTRL | SHIFT, HID_KEY_V);
    ASSERT_TRUE(paste_busy());
    press("cd", false);
    ASSERT_STREQ(keyboard(), "abcopy mecd");
}

/* Shift-Delete with no line selection showing types its sequence; with
 * one, it and the paste keys are the line editor's and type nothing, and
 * the copy keys copy the line's selection in place of the primary. */
UTEST(paste, the_line_s_selection_takes_the_keys_through_the_keymap)
{
    fresh();
    say("copy me\r\n");
    clip_copy(CLIP_PRIMARY, shown(), 0, 0, 0, 7);
    chord(SHIFT, HID_KEY_DELETE);
    ASSERT_STREQ(keyboard(), "\33[3;2~");
    ASSERT_EQ(line_cuts, 0u);
    line_selects = true;
    chord(SHIFT, HID_KEY_DELETE);
    chord(CTRL | SHIFT, HID_KEY_V);
    chord(SHIFT, HID_KEY_INSERT);
    ASSERT_FALSE(paste_busy());
    ASSERT_STREQ(keyboard(), "");
    ASSERT_EQ(line_cuts, 1u);
    ASSERT_EQ(line_pastes, 2u);
    chord(CTRL, HID_KEY_INSERT);
    line_selects = false;
    chord(SHIFT, HID_KEY_INSERT);
    ASSERT_STREQ(keyboard(), "line");
}

/* Ctrl-Shift-X is a cut key as Shift-Delete is: with no line selection
 * showing it types Ctrl-X, and with one it is the line editor's and types
 * nothing. The RIA is no Mac, so GUI-X is never one. */
UTEST(paste, ctrl_shift_x_cuts_through_the_keymap)
{
    fresh();
    line_selects = false;
    line_cuts = 0;
    chord(CTRL | SHIFT, HID_KEY_X);
    ASSERT_STREQ(keyboard(), "\x18");
    ASSERT_EQ(line_cuts, 0u);
    line_selects = true;
    chord(CTRL | SHIFT, HID_KEY_X);
    ASSERT_STREQ(keyboard(), "");
    ASSERT_EQ(line_cuts, 1u);
    chord(KEYBOARD_MODIFIER_LEFTGUI, HID_KEY_X);
    ASSERT_EQ(line_cuts, 1u);
    line_selects = false;
}

UTEST_MAIN();
