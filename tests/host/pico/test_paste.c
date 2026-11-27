/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The keyboard's queue as the RIA has it: 32 entries, read through the
 * keyboard row, KEYMAP_COM_SOURCE, with keymap_abandon its clear, pastes
 * of the machine's own buffers in code page bytes (clip_byte.c), read where
 * they are, and the text cells of its terminal. Keys are typed through
 * keymap.c and vtkey.c on a layout of the letters alone, and their chords
 * go through key.c. The line editor is not in this test: a line is read
 * where the test says, and the test takes its entries as the line editor
 * would. */

#include "core/aud/bel.h"
#include "core/com/rln.h"
#include "core/hid/clip.h"
#include "core/hid/keyboard.h"
#include "core/hid/keymap.h"
#include "core/hid/layout.h"
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
void vol_key(uint8_t keycode) { (void)keycode; }
uint64_t host_clock_us(void) { return 1; }

static unsigned sigints;
void ria_trigger_sigint(void) { sigints++; }

/* What keymap.c, key.c and typed.c ask of the keyboard, the USB host, the
 * programs, the line editor, the console's mouse and the clock: no locks
 * and no program's chords. A key is held, and its repeat due, where the
 * test says. The line editor reads a line where line_reads, and the
 * mouse's highlight shows the first highlight columns of row 0. */
bool hid_boot_enumerating(void) { return false; }
void hid_set_leds(uint8_t leds) { (void)leds; }
bool proc_key_chord(uint8_t keycode, bool ctrl, bool alt)
{
    (void)keycode, (void)ctrl, (void)alt;
    return false;
}
static bool line_reads;
bool rln_reading(void) { return line_reads; }
static uint8_t highlight;
bool vterm_mouse_copy(void)
{
    if (highlight)
        clip_copy(CLIP_CLIPBOARD, vterm_get_shown(&vterm)->shown, 0, 0, 0, highlight);
    return highlight;
}
uint8_t keyboard_get_locks(void) { return 0; }
void keyboard_set_locks(uint8_t locks) { (void)locks; }
static uint8_t held_modifier, held_key;
uint8_t keyboard_get_modifier(void) { return held_modifier; }
bool keyboard_key_down(uint8_t keycode) { return keycode == held_key; }
uint16_t oem_get_code_page_run(void) { return 437; }
timer_deadline_t timer_in_us(uint64_t us) { return us; }
bool timer_passed(timer_deadline_t d)
{
    (void)d;
    return held_key;
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
    line_reads = false;
    highlight = 0;
    held_modifier = held_key = 0;
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

/* What the keyboard row gives, read a few bytes at a time. */
static const char *keyboard(void)
{
    static char out[1024];
    size_t got = 0, n;
    while (got + 5 < sizeof out && (n = row.read(out + got, 5)))
        got += n;
    out[got] = 0;
    return out;
}

/* What the line editor would take from the queue now, as text: 'C' for the
 * copy key, which copies "line" to the clipboard, 'X' for a cut key, which
 * copies "cut" there, and 'M' for an entry of the mouse's. */
static const char *line_takes(void)
{
    static char took[16];
    size_t n = 0;
    typed_entry_t e;
    while (n < sizeof took - 1 && typed_take(&e))
    {
        const unsigned kind = TYPED_KIND(e);
        took[n++] = kind == TYPED_COPY ? 'C' : kind == TYPED_CUT ? 'X' : 'M';
        if (kind == TYPED_COPY)
            clip_copy_bytes(CLIP_CLIPBOARD, "line", 4);
        if (kind == TYPED_CUT)
            clip_copy_bytes(CLIP_CLIPBOARD, "cut", 3);
    }
    took[n] = 0;
    return took;
}

#define CTRL KEYBOARD_MODIFIER_LEFTCTRL
#define SHIFT KEYBOARD_MODIFIER_LEFTSHIFT

/* Keys typed before the paste go first, then the paste, bracketed, then
 * the keys typed after it. The paste takes one entry, however long. */
UTEST(paste, the_primary_goes_whole_past_a_small_queue)
{
    fresh();
    say("\33[?2004hthe first line of the text\r\nand the second one");
    clip_copy(CLIP_PRIMARY, shown(), 0, 0, 1, 80);
    press("ab", false);
    clip_paste(CLIP_PRIMARY);
    ASSERT_EQ(row.peek(0), 'a');
    ASSERT_EQ(row.peek(2), -1);
    press("cd", false);
    ASSERT_EQ(typed_free(), (size_t)TYPED_QUEUE_SIZE - 5);
    ASSERT_STREQ(keyboard(), "ab\33[200~the first line of the text\rand the second one"
                             "\33[201~cd");
    ASSERT_EQ(row.peek(0), -1);
}

/* A peek reads through the paste being typed, its brackets and its text,
 * to the keys behind it. */
UTEST(paste, a_peek_reads_through_the_paste_being_typed)
{
    fresh();
    say("\33[?2004hxy");
    clip_copy(CLIP_PRIMARY, shown(), 0, 0, 0, 2);
    clip_paste(CLIP_PRIMARY);
    press("k", false);
    static const char want[] = "\33[200~xy\33[201~k";
    for (size_t at = 0; at < sizeof want - 1; at++)
    {
        for (size_t i = 0; at + i < sizeof want - 1; i++)
            ASSERT_EQ(row.peek(i), want[at + i]);
        ASSERT_EQ(row.peek(sizeof want - 1 - at), -1);
        char one[1];
        ASSERT_EQ(row.read(one, 1), (size_t)1);
        ASSERT_EQ(one[0], want[at]);
    }
}

/* Keys typed behind a paste fill the queue, and then are dropped. */
UTEST(paste, keys_past_a_full_queue_are_dropped)
{
    fresh();
    say("0123456789012345678901234567890123456789");
    clip_copy(CLIP_PRIMARY, shown(), 0, 0, 0, 40);
    clip_paste(CLIP_PRIMARY);
    press("abcdefghijklmnopqrstuvwxyzabcdefg", false);
    ASSERT_STREQ(keyboard(), "0123456789012345678901234567890123456789"
                             "abcdefghijklmnopqrstuvwxyzabcde");
}

/* A Ctrl-C raises the SIGINT at once and ends the paste being typed: no
 * more of its text, then its closing bracket, then the keys behind it and
 * the Ctrl-C. Every paste still waiting is dropped. */
UTEST(paste, a_ctrl_c_ends_the_paste_and_drops_those_waiting)
{
    fresh();
    say("\33[?2004h0123456789012345678901234567890123456789");
    clip_copy(CLIP_PRIMARY, shown(), 0, 0, 0, 40);
    clip_copy(CLIP_CLIPBOARD, shown(), 0, 0, 0, 3);
    clip_paste(CLIP_PRIMARY);
    press("k", false);
    clip_paste(CLIP_CLIPBOARD);
    clip_paste(CLIP_PRIMARY);
    char head[9] = {0};
    ASSERT_EQ(row.read(head, 8), (size_t)8);
    ASSERT_STREQ(head, "\33[200~01");
    press("c", true);
    ASSERT_EQ(sigints, 1u);
    ASSERT_STREQ(keyboard(), "\33[201~k\3");
    clip_paste(CLIP_CLIPBOARD);
    ASSERT_STREQ(keyboard(), "\33[200~012\33[201~");
}

/* The row's clear drops the paste being typed, its brackets and everything
 * behind it. */
UTEST(paste, the_row_s_clear_drops_the_paste)
{
    fresh();
    say("\33[?2004h0123456789012345678901234567890123456789");
    clip_copy(CLIP_PRIMARY, shown(), 0, 0, 0, 40);
    clip_paste(CLIP_PRIMARY);
    char head[3];
    ASSERT_EQ(row.read(head, 3), (size_t)3);
    press("ab", false);
    clip_paste(CLIP_PRIMARY);
    row.clear();
    ASSERT_EQ(row.peek(0), -1);
    ASSERT_EQ(typed_free(), (size_t)TYPED_QUEUE_SIZE);
    ASSERT_STREQ(keyboard(), "");
}

/* A copy into the buffer a paste is being typed from ends that paste, with
 * its closing bracket. */
UTEST(paste, a_copy_into_the_buffer_ends_its_paste)
{
    fresh();
    say("\33[?2004h0123456789012345678901234567890123456789");
    clip_copy(CLIP_PRIMARY, shown(), 0, 0, 0, 40);
    clip_paste(CLIP_PRIMARY);
    char head[9] = {0};
    ASSERT_EQ(row.read(head, 8), (size_t)8);
    ASSERT_STREQ(head, "\33[200~01");
    clip_copy(CLIP_PRIMARY, shown(), 0, 0, 0, 2);
    ASSERT_STREQ(keyboard(), "\33[201~");
}

/* Pastes wait their turn, and each is typed whole: a second pressed while
 * the first is typed cuts nothing of it. */
UTEST(paste, two_pastes_both_finish)
{
    fresh();
    say("\33[?2004h0123456789012345678901234567890123456789");
    clip_copy(CLIP_PRIMARY, shown(), 0, 0, 0, 40);
    clip_copy(CLIP_CLIPBOARD, shown(), 0, 0, 0, 3);
    clip_paste(CLIP_PRIMARY);
    char head[9] = {0};
    ASSERT_EQ(row.read(head, 8), (size_t)8);
    clip_paste(CLIP_CLIPBOARD);
    ASSERT_STREQ(keyboard(), "23456789012345678901234567890123456789\33[201~"
                             "\33[200~012\33[201~");
}

/* A paste types its buffer as it is when the paste's turn comes: what is
 * copied there while the paste waits is what it types. */
UTEST(paste, a_waiting_paste_types_its_buffer_as_it_is_at_its_turn)
{
    fresh();
    say("copy me\r\n");
    clip_copy(CLIP_PRIMARY, shown(), 0, 0, 0, 4);
    press("a", false);
    clip_paste(CLIP_PRIMARY);
    clip_copy(CLIP_PRIMARY, shown(), 0, 5, 0, 7);
    ASSERT_STREQ(keyboard(), "ame");
}

/* An empty buffer pastes nothing, not even brackets, and holds nothing
 * up. */
UTEST(paste, an_empty_buffer_pastes_nothing)
{
    fresh();
    say("\33[?2004h");
    press("ab", false);
    clip_paste(CLIP_PRIMARY);
    press("cd", false);
    ASSERT_STREQ(keyboard(), "abcd");
    ASSERT_EQ(typed_free(), (size_t)TYPED_QUEUE_SIZE);
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

/* While the mouse's highlight shows, the copy keys put its text in the
 * machine's clipboard at the press and type nothing, so Ctrl-Shift-C
 * raises no SIGINT; the paste keys paste it, bracketed under ?2004 and not
 * without it. */
UTEST(paste, the_copy_and_paste_keys_through_the_keymap)
{
    fresh();
    say("copy me\r\n");
    highlight = 7;
    chord(CTRL | SHIFT, HID_KEY_C);
    ASSERT_EQ(sigints, 0u);
    ASSERT_EQ(typed_free(), (size_t)TYPED_QUEUE_SIZE);
    chord(CTRL | SHIFT, HID_KEY_V);
    ASSERT_STREQ(keyboard(), "copy me");
    say("\33[?2004h");
    chord(SHIFT, HID_KEY_INSERT);
    ASSERT_STREQ(keyboard(), "\33[200~copy me\33[201~");
    highlight = 4;
    chord(CTRL, HID_KEY_INSERT);
    chord(SHIFT, HID_KEY_INSERT);
    ASSERT_STREQ(keyboard(), "\33[200~copy\33[201~");
    say("\33[?2004l");
    press("c", true);
    ASSERT_EQ(sigints, 1u);
    ASSERT_STREQ(keyboard(), "\3");
}

/* A paste goes between the keys typed before it and the keys typed after
 * it, for a program that reads bytes. A copy of the highlight is taken at
 * its press, so the paste behind it types that. */
UTEST(paste, a_paste_key_lands_in_its_place)
{
    fresh();
    say("copy me\r\n");
    highlight = 7;
    press("ab", false);
    chord(CTRL | SHIFT, HID_KEY_C);
    chord(CTRL | SHIFT, HID_KEY_V);
    press("cd", false);
    ASSERT_STREQ(keyboard(), "abcopy mecd");
}

/* With no highlight and no line being read, a copy key does nothing: it
 * types nothing, raises no SIGINT, and the keys behind it are read. */
UTEST(paste, a_copy_key_with_no_line_read_is_dropped)
{
    fresh();
    clip_copy_bytes(CLIP_CLIPBOARD, "kept", 4);
    press("a", false);
    chord(CTRL | SHIFT, HID_KEY_C);
    chord(CTRL, HID_KEY_INSERT);
    press("b", false);
    ASSERT_EQ(row.peek(1), -1);
    ASSERT_STREQ(keyboard(), "ab");
    ASSERT_EQ(sigints, 0u);
    chord(SHIFT, HID_KEY_INSERT);
    ASSERT_STREQ(keyboard(), "kept");
}

/* With a line being read, a copy key waits in its place for the line
 * editor: the keys before it are read, nothing behind it is, and once it
 * is taken the keys behind it come. The paste behind it types what the
 * copy put in the clipboard. */
UTEST(paste, a_copy_key_waits_for_the_line_editor)
{
    fresh();
    line_reads = true;
    press("a", false);
    chord(CTRL, HID_KEY_INSERT);
    chord(SHIFT, HID_KEY_INSERT);
    press("b", false);
    ASSERT_STREQ(line_takes(), "");
    ASSERT_STREQ(keyboard(), "a");
    ASSERT_EQ(row.peek(0), -1);
    ASSERT_STREQ(line_takes(), "C");
    ASSERT_STREQ(keyboard(), "lineb");
}

/* A cut key is the program's: with no line being read it types what the
 * key types. The RIA is no Mac, so GUI-X is no cut key. */
UTEST(paste, a_cut_key_with_no_line_read_types_its_bytes)
{
    fresh();
    press("a", false);
    chord(SHIFT, HID_KEY_DELETE);
    chord(CTRL | SHIFT, HID_KEY_X);
    press("b", false);
    ASSERT_EQ(row.peek(1), -1);
    ASSERT_STREQ(keyboard(), "a\33[3;2~\x18" "b");
    ASSERT_EQ(typed_free(), (size_t)TYPED_QUEUE_SIZE);
    chord(KEYBOARD_MODIFIER_LEFTGUI, HID_KEY_X);
    ASSERT_STREQ(keyboard(), "x");
    /* A VT52's Delete has no modifiers. */
    say("\33[?2l");
    chord(SHIFT, HID_KEY_DELETE);
    say("\33<");
    ASSERT_STREQ(keyboard(), "\33[3~");
}

/* With a line being read, a cut key waits in its place for the line
 * editor, which takes its bytes with its mark: the key before it is read,
 * the cut is taken, and the key after it is read. The paste pressed behind
 * the cut types what the cut put in the clipboard. */
UTEST(paste, a_cut_key_waits_for_the_line_editor)
{
    fresh();
    clip_copy_bytes(CLIP_CLIPBOARD, "old", 3);
    line_reads = true;
    press("a", false);
    chord(SHIFT, HID_KEY_DELETE);
    press("b", false);
    chord(CTRL | SHIFT, HID_KEY_X);
    chord(CTRL | SHIFT, HID_KEY_V);
    press("c", false);
    ASSERT_STREQ(keyboard(), "a");
    ASSERT_STREQ(line_takes(), "X");
    ASSERT_STREQ(keyboard(), "b");
    ASSERT_STREQ(line_takes(), "X");
    ASSERT_STREQ(keyboard(), "cutc");
    ASSERT_STREQ(line_takes(), "");
}

/* A cut key behind a paste is not taken while the paste is typed. */
UTEST(paste, a_cut_key_behind_a_paste_waits_for_its_text)
{
    fresh();
    clip_copy_bytes(CLIP_CLIPBOARD, "text", 4);
    line_reads = true;
    chord(SHIFT, HID_KEY_INSERT);
    chord(SHIFT, HID_KEY_DELETE);
    char head[3] = {0};
    ASSERT_EQ(row.read(head, 2), (size_t)2);
    ASSERT_STREQ(line_takes(), "");
    ASSERT_STREQ(keyboard(), "xt");
    ASSERT_STREQ(line_takes(), "X");
}

/* The copy, cut and paste keys act once for a press: held, they do not
 * repeat, where a key that types does. */
UTEST(paste, the_clipboard_keys_do_not_repeat)
{
    static const struct
    {
        uint8_t modifier, keycode;
        const char *typed;
    } keys[] = {
        {SHIFT, HID_KEY_INSERT, "kept"},
        {SHIFT, HID_KEY_DELETE, "\33[3;2~"},
        {CTRL | SHIFT, HID_KEY_X, "\x18"},
        {CTRL, HID_KEY_INSERT, ""},
        {0, HID_KEY_A, "aaa"},
    };
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
    {
        fresh();
        clip_copy_bytes(CLIP_CLIPBOARD, "kept", 4);
        held_modifier = keys[i].modifier;
        held_key = keys[i].keycode;
        chord(held_modifier, held_key);
        keymap_task();
        keymap_task();
        ASSERT_STREQ(keyboard(), keys[i].typed);
    }
    /* The 'a' is let go. */
    held_key = HID_KEY_A + 1;
    keymap_task();
}

/* The mouse's entries in the line are the line editor's: it takes them in
 * their place among the keys, and with no line being read they are dropped
 * and the keys behind them are read. A drag's motion merges into the pair
 * queued last. */
UTEST(paste, the_mouse_s_entries_are_the_line_editor_s)
{
    fresh();
    line_reads = true;
    press("a", false);
    ASSERT_TRUE(typed_put(TYPED_ENTRY(TYPED_PRESS, 3)));
    typed_drag(3, 4);
    typed_drag(3, 5);
    ASSERT_TRUE(typed_put(TYPED_ENTRY(TYPED_RELEASE, 0)));
    press("b", false);
    ASSERT_EQ(typed_free(), (size_t)TYPED_QUEUE_SIZE - 6);
    ASSERT_STREQ(keyboard(), "a");
    ASSERT_STREQ(line_takes(), "MMMM");
    ASSERT_STREQ(keyboard(), "b");
    press("a", false);
    ASSERT_TRUE(typed_put(TYPED_ENTRY(TYPED_PRESS, 3)));
    typed_drag(3, 4);
    press("b", false);
    typed_void_mouse();
    ASSERT_STREQ(keyboard(), "ab");
    ASSERT_STREQ(line_takes(), "");
    press("a", false);
    ASSERT_TRUE(typed_put(TYPED_ENTRY(TYPED_PRESS, 3)));
    typed_drag(3, 4);
    ASSERT_TRUE(typed_put(TYPED_ENTRY(TYPED_RELEASE, 0)));
    press("b", false);
    line_reads = false;
    ASSERT_STREQ(keyboard(), "ab");
    ASSERT_EQ(typed_free(), (size_t)TYPED_QUEUE_SIZE);
}

UTEST_MAIN();
