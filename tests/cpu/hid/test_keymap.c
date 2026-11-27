/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* The keyboard driver of a machine with no system under it, core/hid/keymap.c,
 * as a USB keyboard drives it: boot reports into keyboard_report, and what
 * the keys typed read back from the keyboard's row of the console. These pin
 * what it types today, whatever shape its insides take. */

#include "core/com/com.h"
#include "core/com/manifold.h"
#include "core/com/rln.h"
#include "core/hid/clip.h"
#include "core/hid/hid.h"
#include "core/hid/keyboard.h"
#include "core/hid/keymap.h"
#include "core/hid/layout.h"
#include "core/hid/typed.h"
#include "core/hid/usage.h"
#include "core/str/oem.h"
#include "core/sys/sys.h"
#include "core/term/vterm.h"
#include "host/host.h"
#include "emu_boot.h"
#include <string.h>

/* The clipboard as this machine has it: the host's own, in UTF-8, or the
 * bytes the host keeps for the machine (core/hid/clip.h). The tests' text
 * is ASCII on one line, the same in both. */
static void clipboard_set(const char *text)
{
#if CLIP_CODEPAGE
    host_clipboard_len = (uint16_t)strlen(text);
    memcpy(host_clipboard_bytes, text, host_clipboard_len);
#else
    host_clipboard_set(text);
#endif
}

static const char *clipboard(void)
{
#if CLIP_CODEPAGE
    static char text[CLIP_SIZE + 1];
    memcpy(text, host_clipboard_bytes, host_clipboard_len);
    text[host_clipboard_len] = 0;
    return text;
#else
    return host_clipboard_get();
#endif
}

#define SLOT 3
#define LCTRL KEYBOARD_MODIFIER_LEFTCTRL
#define LSHIFT KEYBOARD_MODIFIER_LEFTSHIFT
#define LALT KEYBOARD_MODIFIER_LEFTALT
#define RALT KEYBOARD_MODIFIER_RIGHTALT
#define LGUI KEYBOARD_MODIFIER_LEFTGUI

/* Keys by their place on a US keyboard, which is what a HID usage names. */
#define KEY_B 0x05
#define KEY_E 0x08
#define KEY_S 0x16
#define KEY_Y 0x1C
#define KEY_1 0x1E
#define KEY_APOSTROPHE 0x34

/* emu_core's machine types through its host's system, so its library has
 * core/hid/hid_null.c, whose keyboard hooks are keymap.c's here. Defining
 * the rest of that file's symbols keeps the linker from pulling it in. */
void hid_remapped(void)
{
}

/* What keymap.c asks of the USB stack: the LEDs, recorded so a test can
 * see them, and whether the boot keyboards are still enumerating, which
 * they are not. */
static int leds_calls;
static uint8_t leds_last;

void hid_set_leds(uint8_t leds)
{
    leds_calls++;
    leds_last = leds;
}

bool hid_boot_enumerating(void)
{
    return false;
}

/* The SET KB setting's storage, which emu_core's config has not got, kept
 * as core/sys/config.c keeps it, so GUI-Space walks a real list. */
static char list_store[LAYOUT_LIST_SIZE];

const char *layout_get_list(void)
{
    return list_store;
}

bool layout_set_list(const char *v)
{
    char checked[LAYOUT_LIST_SIZE];
    if (!layout_check_list(v, checked))
        return false;
    strcpy(list_store, checked);
    layout_apply_list(list_store, true);
    return true;
}

static int layout_named(const char *name)
{
    for (int i = 0; i < layout_count(); i++)
        if (!strcmp(layout_name(i), name))
            return i;
    return -1;
}

/* A boot keyboard: modifier byte, reserved byte, six keycodes. */
static keyboard_connection_t boot_keyboard(void)
{
    keyboard_connection_t desc;
    memset(&desc, 0, sizeof desc);
    desc.valid = true;
    desc.codes_offset = 16;
    desc.codes_count = 6;
    desc.runs[0].bit_pos = 0;
    desc.runs[0].usage_min = HID_KEY_CONTROL_LEFT;
    desc.runs[0].count = 8;
    return desc;
}

/* One report: these modifiers and at most this one key down. */
static void keys(uint8_t modifier, uint8_t keycode)
{
    uint8_t report[8] = {modifier, 0, keycode, 0, 0, 0, 0, 0};
    keyboard_report(SLOT, report, sizeof report);
}

/* A key pressed and let go, with the modifiers held through both. */
static void tap(uint8_t modifier, uint8_t keycode)
{
    keys(modifier, keycode);
    keys(modifier, 0);
}

/* What the keys since the last call typed, as a string. */
static const char *typed(void)
{
    static char buf[64];
    int n = 0, c;
    com_source_t src = COM_SOURCE_KEYBOARD;
    while (n < (int)sizeof buf - 1 && (c = com_getchar(&src)) >= 0)
    {
        buf[n++] = (char)c;
        src = COM_SOURCE_KEYBOARD;
    }
    buf[n] = 0;
    return buf;
}

/* A machine as it boots, with one keyboard plugged in, typing through the
 * named layout of the database. NumLock comes up on, as keymap_init sets
 * it, and the terminal is in its usual modes. */
static void start(const char *layout)
{
    keyboard_umount(SLOT);
    keyboard_init();
    keymap_init();
    keymap_abandon();
    com_rx_clear();
    vterm.app_cursor = false;
    vterm.app_keypad = false;
    vterm.newline_mode = false;
    vterm.vt52 = false;
    keymap_use_layout(layout_named(layout));
    keyboard_connection_t desc = boot_keyboard();
    keyboard_mount(SLOT, &desc, 0, 0);
    keys(0, 0);
    leds_calls = 0;
}

/* The line editor's turn. */
static void line_loop(void)
{
    for (int i = 0; i < 64; i++)
        rln_task();
}

static char line_got[RLN_LINE_MAX];

static void line_read(bool timeout, const char *buf)
{
    (void)timeout;
    strcpy(line_got, buf);
}

/* A line being read on a machine as it boots, with text typed into it. */
static void line_start(const char *text)
{
    sys_init();
    manifold_set_paced(false);
    start("US");
    rln_read_line(line_read, NULL, NULL);
    line_loop();
    typed_push(text, strlen(text));
    line_loop();
}

/* Enter ends the line, and what it holds is returned. */
static const char *line_end(void)
{
    line_got[0] = 0;
    typed_push("\r", 1);
    line_loop();
    return line_got;
}

/* The bytes below are CP437's, the code page the machine boots in. */
UTEST(keymap, the_machine_types_in_code_page_437)
{
    start("US");
    ASSERT_EQ(oem_get_code_page_run(), 437);
}

UTEST(keymap, letters_and_digits_type_on_us)
{
    start("US");
    tap(0, HID_KEY_A);
    tap(0, HID_KEY_Z);
    tap(0, KEY_1);
    tap(0, 0x27); /* 0 */
    tap(0, HID_KEY_SPACE);
    ASSERT_STREQ(typed(), "az10 ");
}

UTEST(keymap, shift_types_the_shifted_column)
{
    start("US");
    tap(LSHIFT, HID_KEY_A);
    tap(KEYBOARD_MODIFIER_RIGHTSHIFT, HID_KEY_Z);
    tap(LSHIFT, KEY_1);
    tap(LSHIFT, KEY_APOSTROPHE);
    ASSERT_STREQ(typed(), "AZ!\"");
}

/* A key goes down with the modifiers of the report it is first seen in,
 * so Shift let go while a key is held changes nothing already typed. */
UTEST(keymap, a_key_is_typed_once_as_it_goes_down)
{
    start("US");
    keys(LSHIFT, HID_KEY_A);
    keys(0, HID_KEY_A);
    keys(0, 0);
    ASSERT_STREQ(typed(), "A");
}

/* CapsLock shifts only the keys the layout says it does, and Shift then
 * undoes it. The digits are not among them. */
UTEST(keymap, caps_lock_shifts_only_the_keys_it_affects)
{
    start("US");
    tap(0, HID_KEY_CAPS_LOCK);
    ASSERT_TRUE(layout_use_caps(layout_named("US"), HID_KEY_A));
    ASSERT_FALSE(layout_use_caps(layout_named("US"), KEY_1));
    tap(0, HID_KEY_A);
    tap(LSHIFT, HID_KEY_A);
    tap(0, KEY_1);
    tap(LSHIFT, KEY_1);
    tap(0, KEY_APOSTROPHE);
    ASSERT_STREQ(typed(), "Aa1!'");
}

/* NumLock starts on, where the keypad types digits; off, the digits are
 * the navigation keys they share. */
UTEST(keymap, num_lock_picks_digits_or_navigation_on_the_keypad)
{
    start("US");
    tap(0, HID_KEY_KEYPAD_1);
    tap(0, HID_KEY_KEYPAD_8);
    tap(0, HID_KEY_KEYPAD_0);
    ASSERT_STREQ(typed(), "180");
    tap(0, HID_KEY_NUM_LOCK);
    tap(0, HID_KEY_KEYPAD_1);
    tap(0, HID_KEY_KEYPAD_8);
    tap(0, HID_KEY_KEYPAD_0);
    ASSERT_STREQ(typed(), "\33[F\33[A\33[2~");
}

/* Shift over the keypad with NumLock on is NumLock off, with the Shift
 * used up. */
UTEST(keymap, shift_over_the_keypad_navigates)
{
    start("US");
    tap(LSHIFT, HID_KEY_KEYPAD_8);
    ASSERT_STREQ(typed(), "\33[A");
}

UTEST(keymap, ctrl_with_a_letter_types_its_control_byte)
{
    start("US");
    tap(LCTRL, HID_KEY_A);
    tap(KEYBOARD_MODIFIER_RIGHTCTRL, HID_KEY_Z);
    tap(LCTRL, 0x10); /* m */
    ASSERT_STREQ(typed(), "\x01\x1A\r");
}

/* Left Alt is Meta: ESC before the plain or shifted character. */
UTEST(keymap, alt_with_a_letter_types_meta)
{
    start("US");
    tap(LALT, HID_KEY_A);
    tap(LALT | LSHIFT, HID_KEY_A);
    tap(LALT, KEY_1);
    ASSERT_STREQ(typed(), "\33a\33A\0331");
}

/* On a layout with no AltGr column, right Alt is Meta as left Alt is. */
UTEST(keymap, right_alt_is_meta_where_the_layout_has_no_altgr)
{
    start("US");
    tap(RALT, HID_KEY_A);
    ASSERT_STREQ(typed(), "\33a");
}

/* Right Alt is AltGr on US-INTL: e acute and, with Shift, E acute, which
 * CP437 has at 0x82 and 0x90. Left Alt there is still Meta. */
UTEST(keymap, altgr_types_the_layouts_third_column)
{
    start("US-INTL");
    tap(RALT, KEY_E);
    tap(RALT | LSHIFT, KEY_E);
    tap(LALT, KEY_E);
    ASSERT_STREQ(typed(), "\x82\x90\33e");
}

/* A key AltGr has nothing on is Meta on its plain character. */
UTEST(keymap, altgr_on_a_key_without_a_third_column_is_meta)
{
    start("US-INTL");
    tap(RALT, KEY_B);
    ASSERT_STREQ(typed(), "\33b");
}

/* AltGr's character that the code page has not got is Meta on the plain
 * character too: acute accent, U+00B4, is not in CP437. */
UTEST(keymap, altgr_on_a_character_the_code_page_lacks_is_meta)
{
    start("US-INTL");
    tap(RALT, KEY_APOSTROPHE);
    ASSERT_STREQ(typed(), "\33'");
}

/* The apostrophe is a dead key on US-INTL, and types nothing until the
 * key after it. */
UTEST(keymap, a_dead_key_composes_with_the_next_key)
{
    start("US-INTL");
    tap(0, KEY_APOSTROPHE);
    ASSERT_STREQ(typed(), "");
    tap(0, KEY_E);
    ASSERT_STREQ(typed(), "\x82");
    /* Shift's apostrophe, the quote, is a dead key too: a umlaut. */
    tap(LSHIFT, KEY_APOSTROPHE);
    tap(0, HID_KEY_A);
    ASSERT_STREQ(typed(), "\x84");
}

UTEST(keymap, space_after_a_dead_key_types_the_mark_alone)
{
    start("US-INTL");
    tap(0, KEY_APOSTROPHE);
    tap(0, HID_KEY_SPACE);
    ASSERT_STREQ(typed(), "'");
}

/* Backspace sends DEL, which takes the dead key back. */
UTEST(keymap, backspace_after_a_dead_key_cancels_it)
{
    start("US-INTL");
    tap(0, KEY_APOSTROPHE);
    tap(0, HID_KEY_BACKSPACE);
    ASSERT_STREQ(typed(), "");
    tap(0, KEY_E);
    ASSERT_STREQ(typed(), "e");
}

UTEST(keymap, a_pair_that_composes_nothing_types_both)
{
    start("US-INTL");
    tap(0, KEY_APOSTROPHE);
    tap(0, KEY_S);
    tap(0, KEY_APOSTROPHE);
    tap(0, KEY_APOSTROPHE);
    ASSERT_STREQ(typed(), "'s''");
}

/* y acute is in the layout's table but not in CP437, so the pair types
 * both, as one the table has not got does. */
UTEST(keymap, a_pair_whose_result_the_code_page_lacks_types_both)
{
    start("US-INTL");
    tap(0, KEY_APOSTROPHE);
    tap(0, KEY_Y);
    ASSERT_STREQ(typed(), "'y");
}

/* A key with a sequence of its own types the dead key first. */
UTEST(keymap, a_sequence_after_a_dead_key_follows_the_mark)
{
    start("US-INTL");
    tap(0, KEY_APOSTROPHE);
    tap(0, HID_KEY_ARROW_LEFT);
    ASSERT_STREQ(typed(), "'\33[D");
}

/* No layout in the database has three-press dead keys, so there is
 * nothing of keymap.c's three-press composer to reach. */
UTEST(keymap, no_layout_has_three_press_dead_keys)
{
    for (int i = 0; i < layout_count(); i++)
        ASSERT_EQ(layout_dead3_count(i), 0u);
}

/* Alt held over the keypad's digits types the character of that code in
 * the code page, as Alt goes up. */
UTEST(keymap, an_alt_code_is_typed_as_alt_goes_up)
{
    start("US");
    keys(LALT, 0);
    tap(LALT, HID_KEY_KEYPAD_6);
    tap(LALT, HID_KEY_KEYPAD_5);
    ASSERT_STREQ(typed(), "");
    keys(0, 0);
    ASSERT_STREQ(typed(), "A");
    /* Right Alt does it too, and a leading zero is just a digit. */
    keys(RALT, 0);
    tap(RALT, HID_KEY_KEYPAD_0);
    tap(RALT, HID_KEY_KEYPAD_1);
    tap(RALT, HID_KEY_KEYPAD_3);
    tap(RALT, HID_KEY_KEYPAD_0);
    keys(0, 0);
    ASSERT_STREQ(typed(), "\x82");
}

/* The code is a byte, so it wraps past 255: 321 is 65. */
UTEST(keymap, an_alt_code_past_255_wraps)
{
    start("US");
    keys(LALT, 0);
    tap(LALT, HID_KEY_KEYPAD_3);
    tap(LALT, HID_KEY_KEYPAD_2);
    tap(LALT, HID_KEY_KEYPAD_1);
    keys(0, 0);
    ASSERT_STREQ(typed(), "A");
}

/* Once a code has begun, every key until Alt goes up belongs to it, and a
 * code of zero types nothing. */
UTEST(keymap, keys_inside_an_alt_code_type_nothing)
{
    start("US");
    keys(LALT, 0);
    tap(LALT, HID_KEY_KEYPAD_0);
    tap(LALT, HID_KEY_A);
    tap(LALT, HID_KEY_ENTER);
    keys(0, 0);
    ASSERT_STREQ(typed(), "");
}

/* With NumLock off the keypad is navigation, and Alt is a modifier of it. */
UTEST(keymap, no_alt_code_with_num_lock_off)
{
    start("US");
    tap(0, HID_KEY_NUM_LOCK);
    keys(LALT, 0);
    tap(LALT, HID_KEY_KEYPAD_8);
    keys(0, 0);
    ASSERT_STREQ(typed(), "\33[1;3A");
}

UTEST(keymap, arrows_and_function_keys_send_the_terminals_sequences)
{
    start("US");
    tap(0, HID_KEY_ARROW_UP);
    tap(0, HID_KEY_ARROW_DOWN);
    tap(0, HID_KEY_ARROW_RIGHT);
    tap(0, HID_KEY_ARROW_LEFT);
    ASSERT_STREQ(typed(), "\33[A\33[B\33[C\33[D");
    tap(LSHIFT, HID_KEY_ARROW_UP);
    tap(LCTRL, HID_KEY_ARROW_LEFT);
    ASSERT_STREQ(typed(), "\33[1;2A\33[1;5D");
    tap(0, HID_KEY_F1);
    tap(0, HID_KEY_F5);
    tap(0, HID_KEY_F12);
    ASSERT_STREQ(typed(), "\33OP\33[15~\33[24~");
    tap(0, HID_KEY_HOME);
    tap(0, HID_KEY_DELETE);
    tap(0, HID_KEY_PAGE_DOWN);
    ASSERT_STREQ(typed(), "\33[H\33[3~\33[6~");
}

/* Cursor key mode is the terminal's, and reaches these keys through the
 * encoder. */
UTEST(keymap, arrows_follow_cursor_key_mode)
{
    start("US");
    vterm.app_cursor = true;
    tap(0, HID_KEY_ARROW_UP);
    vterm.app_cursor = false;
    ASSERT_STREQ(typed(), "\33OA");
}

/* The machine boots with NumLock on and says so to the keyboard. */
UTEST(keymap, num_lock_starts_on_and_lights)
{
    leds_calls = 0;
    keymap_init();
    ASSERT_EQ(keyboard_get_locks(), KEYBOARD_LED_NUMLOCK);
    ASSERT_EQ(leds_calls, 1);
    ASSERT_EQ(leds_last, KEYBOARD_LED_NUMLOCK);
}

/* Each lock key flips its lock and its LED, and types nothing. */
UTEST(keymap, lock_keys_flip_their_lock_and_led)
{
    start("US");
    tap(0, HID_KEY_CAPS_LOCK);
    ASSERT_EQ(keyboard_get_locks(), KEYBOARD_LED_NUMLOCK | KEYBOARD_LED_CAPSLOCK);
    ASSERT_EQ(leds_last, KEYBOARD_LED_NUMLOCK | KEYBOARD_LED_CAPSLOCK);
    tap(0, HID_KEY_SCROLL_LOCK);
    ASSERT_EQ(leds_last, KEYBOARD_LED_NUMLOCK | KEYBOARD_LED_CAPSLOCK |
                             KEYBOARD_LED_SCROLLLOCK);
    tap(0, HID_KEY_NUM_LOCK);
    tap(0, HID_KEY_CAPS_LOCK);
    ASSERT_EQ(keyboard_get_locks(), KEYBOARD_LED_SCROLLLOCK);
    ASSERT_EQ(leds_last, KEYBOARD_LED_SCROLLLOCK);
    tap(0, HID_KEY_SCROLL_LOCK);
    ASSERT_EQ(keyboard_get_locks(), 0);
    ASSERT_EQ(leds_calls, 5);
    ASSERT_STREQ(typed(), "");
}

/* A lock key flips its lock whatever modifiers are held with it. */
UTEST(keymap, a_lock_key_with_modifiers_still_flips)
{
    start("US");
    tap(LCTRL | LSHIFT, HID_KEY_CAPS_LOCK);
    ASSERT_EQ(keyboard_get_locks(), KEYBOARD_LED_NUMLOCK | KEYBOARD_LED_CAPSLOCK);
    ASSERT_STREQ(typed(), "");
}

/* With no line being read, a copy key does nothing and types nothing:
 * neither Ctrl-Shift-C's character nor Ctrl-Insert's sequence. */
UTEST(keymap, a_copy_chord_with_no_line_read_types_nothing)
{
    start("US");
    tap(LCTRL | LSHIFT, HID_KEY_C);
    tap(LCTRL, HID_KEY_INSERT);
    ASSERT_STREQ(typed(), "");
}

/* Each copy key puts the line's selection in the clipboard, in its place
 * among the keys, and types nothing into the line. */
UTEST(keymap, the_copy_chords_copy_the_lines_selection)
{
    line_start("copied");
    tap(LSHIFT, HID_KEY_HOME);
    clipboard_set("");
    tap(LCTRL | LSHIFT, HID_KEY_C);
    line_loop();
    ASSERT_STREQ(clipboard(), "copied");
    clipboard_set("");
    tap(LCTRL, HID_KEY_INSERT);
    line_loop();
    ASSERT_STREQ(clipboard(), "copied");
    ASSERT_STREQ(line_end(), "copied");
    clipboard_set("");
}

/* A paste key types the host's clipboard, and not its own character or
 * sequence. */
UTEST(keymap, the_paste_chords_type_the_clipboard)
{
    start("US");
    clipboard_set("hi");
    tap(LCTRL | LSHIFT, HID_KEY_V);
    ASSERT_STREQ(typed(), "hi");
    tap(LSHIFT, HID_KEY_INSERT);
    ASSERT_STREQ(typed(), "hi");
    clipboard_set("");
}

/* With no line being read, a cut key types what the key types: the
 * Ctrl-X byte for Ctrl-Shift-X, and Shift-Delete's sequence. */
UTEST(keymap, a_cut_chord_with_no_line_read_types_its_bytes)
{
    start("US");
    tap(LCTRL | LSHIFT, HID_KEY_X);
    tap(LSHIFT, HID_KEY_DELETE);
    ASSERT_STREQ(typed(), "\x18\33[3;2~");
}

/* Each cut key cuts the line's selection to the clipboard, and what it
 * would type is not typed. */
UTEST(keymap, the_cut_chords_cut_the_lines_selection)
{
    line_start("abcdef");
    tap(LSHIFT, HID_KEY_ARROW_LEFT);
    tap(LCTRL | LSHIFT, HID_KEY_X);
    line_loop();
    ASSERT_STREQ(clipboard(), "f");
    tap(LSHIFT, HID_KEY_ARROW_LEFT);
    tap(LSHIFT, HID_KEY_DELETE);
    line_loop();
    ASSERT_STREQ(clipboard(), "e");
    ASSERT_STREQ(line_end(), "abcd");
    clipboard_set("");
}

/* A cut key types any dead key waiting before its own mark. */
UTEST(keymap, a_cut_chord_types_a_waiting_dead_key_first)
{
    start("US-INTL");
    tap(0, KEY_APOSTROPHE);
    tap(LSHIFT, HID_KEY_DELETE);
    ASSERT_STREQ(typed(), "'\33[3;2~");
}

/* Alt-F4 and Ctrl-Alt-Delete act only with a program to end or a monitor
 * to break to. This machine has neither running, so they type. */
UTEST(keymap, process_chords_type_when_there_is_nothing_to_end)
{
    start("US");
    tap(LALT, HID_KEY_F4);
    tap(LCTRL | LALT, HID_KEY_DELETE);
    ASSERT_STREQ(typed(), "\33[1;3S\33[3;7~");
}

/* GUI-Space types through the next layout of the SET KB list, going
 * around at the end, and types nothing itself. HID usage 0x1C is y on US
 * and z on DE. */
UTEST(keymap, gui_space_cycles_the_layout_list)
{
    start("US");
    ASSERT_TRUE(layout_set_list("US DE"));
    tap(0, KEY_Y);
    keys(LGUI, 0);
    tap(LGUI, HID_KEY_SPACE);
    keys(0, 0);
    tap(0, KEY_Y);
    tap(LGUI, HID_KEY_SPACE);
    tap(0, KEY_Y);
    ASSERT_STREQ(typed(), "yzy");
    ASSERT_TRUE(layout_set_list("US"));
}

/* The GUI key changes nothing but its own chord. */
UTEST(keymap, gui_with_another_key_types_the_key)
{
    start("US");
    tap(LGUI, HID_KEY_A);
    tap(LGUI, HID_KEY_ARROW_UP);
    ASSERT_STREQ(typed(), "a\33[A");
}

/* keymap_abandon drops what was typed and any dead key or Alt code half
 * composed. */
UTEST(keymap, abandon_drops_what_was_half_typed)
{
    start("US-INTL");
    tap(0, HID_KEY_A);
    tap(0, KEY_APOSTROPHE);
    keymap_abandon();
    tap(0, KEY_E);
    ASSERT_STREQ(typed(), "e");
    keys(LALT, 0);
    tap(LALT, HID_KEY_KEYPAD_6);
    keymap_abandon();
    keys(0, 0);
    ASSERT_STREQ(typed(), "");
}

UTEST_MAIN_EMU()
