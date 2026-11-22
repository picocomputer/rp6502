/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "host/sokol/app/input.h"
#include "host/sokol/app/entry.h"
#include "host/sokol/app/gfx.h"
#include "core/com/com.h"
#include "core/hid/keyboard.h"
#include "core/hid/usage.h"
#include "core/term/cells.h"
#include "sokol/sokol_app.h"

#include "utest.h"

#include <string.h>

/* The system's locks, which the window reads and never writes. */
static uint8_t fake_locks;

uint8_t host_locks(void) { return fake_locks; }

/* What input.c calls in a window, which this test has none of. */
void sapp_lock_mouse(bool lock) { (void)lock; }
bool sapp_mouse_locked(void) { return false; }
sapp_desc sapp_query_desc(void) { return (sapp_desc){0}; }
const char *sapp_get_clipboard_string(void) { return ""; }
void sapp_cancel_quit(void) {}
void sapp_set_mouse_cursor(sapp_mouse_cursor cursor) { (void)cursor; }
void sapp_show_mouse(bool show) { (void)show; }
float sapp_dpi_scale(void) { return 1.0f; }
float gfx_canvas_scale(void) { return 1.0f; }
bool gfx_canvas_from_fb(float fx, float fy, int *cx, int *cy)
{
    (void)fx, (void)fy, (void)cx, (void)cy;
    return false;
}

/* A key going down and up, with the character the system sends for it
 * between, if it sends one. */
static void key(sapp_keycode code, uint32_t modifiers, uint32_t ch)
{
    sapp_event e = {.type = SAPP_EVENTTYPE_KEY_DOWN, .key_code = code, .modifiers = modifiers};
    input_event(&e);
    if (ch)
    {
        e.type = SAPP_EVENTTYPE_CHAR;
        e.char_code = ch;
        input_event(&e);
    }
    e.type = SAPP_EVENTTYPE_KEY_UP;
    input_event(&e);
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

static void start(uint8_t locks)
{
    fake_locks = locks;
    cells_term.app_keypad = false;
    com_rx_clear();
}

UTEST(input, the_locks_are_the_systems)
{
    start(KEYBOARD_LED_CAPSLOCK);
    input_locks();
    ASSERT_EQ(keyboard_get_locks(), KEYBOARD_LED_CAPSLOCK);

    /* A lock key changes nothing until the system says the lock changed. */
    key(SAPP_KEYCODE_NUM_LOCK, 0, 0);
    key(SAPP_KEYCODE_CAPS_LOCK, 0, 0);
    key(SAPP_KEYCODE_SCROLL_LOCK, 0, 0);
    ASSERT_EQ(keyboard_get_locks(), KEYBOARD_LED_CAPSLOCK);
    ASSERT_STREQ(typed(), "");

    fake_locks = KEYBOARD_LED_NUMLOCK | KEYBOARD_LED_SCROLLLOCK;
    input_locks();
    ASSERT_EQ(keyboard_get_locks(), KEYBOARD_LED_NUMLOCK | KEYBOARD_LED_SCROLLLOCK);
}

UTEST(input, a_lock_key_shows_in_the_bitmap)
{
    start(0);
    sapp_event e = {.type = SAPP_EVENTTYPE_KEY_DOWN, .key_code = SAPP_KEYCODE_NUM_LOCK};
    input_event(&e);
    ASSERT_TRUE(keyboard_key_down(HID_KEY_NUM_LOCK));
    e.type = SAPP_EVENTTYPE_KEY_UP;
    input_event(&e);
    ASSERT_FALSE(keyboard_key_down(HID_KEY_NUM_LOCK));
}

/* The lock is read as a key goes down, so a key pressed in the frame the lock
 * changed is typed by the new lock. */
UTEST(input, a_key_is_typed_by_the_lock_it_went_down_with)
{
    start(0);
    input_locks();
    fake_locks = KEYBOARD_LED_NUMLOCK;
    key(SAPP_KEYCODE_KP_7, 0, '7');
    ASSERT_STREQ(typed(), "7");
}

UTEST(input, keypad_with_numlock_on)
{
    start(KEYBOARD_LED_NUMLOCK);
    /* The system's character is the only thing typed. */
    key(SAPP_KEYCODE_KP_5, 0, '5');
    key(SAPP_KEYCODE_KP_DECIMAL, 0, ',');
    key(SAPP_KEYCODE_KP_ADD, 0, '+');
    key(SAPP_KEYCODE_KP_ENTER, 0, '\r');
    ASSERT_STREQ(typed(), "5,+\r");

    /* Shift undoes NumLock, and the system sends no digit then. */
    key(SAPP_KEYCODE_KP_1, SAPP_MODIFIER_SHIFT, 0);
    ASSERT_STREQ(typed(), "\33[F");
    /* A system that sends the digit anyway does not get it typed too. */
    key(SAPP_KEYCODE_KP_1, SAPP_MODIFIER_SHIFT, '1');
    ASSERT_STREQ(typed(), "\33[F");

    /* The key after a keypad key types as usual. */
    key(SAPP_KEYCODE_A, 0, 'a');
    ASSERT_STREQ(typed(), "a");
}

UTEST(input, keypad_with_numlock_off)
{
    start(0);
    key(SAPP_KEYCODE_KP_1, 0, 0);
    key(SAPP_KEYCODE_KP_5, 0, 0);
    key(SAPP_KEYCODE_KP_DECIMAL, 0, 0);
    ASSERT_STREQ(typed(), "\33[F\33[E\33[3~");

    /* The operators still type, from the system's character. */
    key(SAPP_KEYCODE_KP_DIVIDE, 0, '/');
    key(SAPP_KEYCODE_KP_ENTER, 0, '\r');
    ASSERT_STREQ(typed(), "/\r");

    /* A navigation key that got no character does not swallow the next
     * key's. */
    key(SAPP_KEYCODE_KP_1, 0, 0);
    key(SAPP_KEYCODE_A, 0, 'a');
    ASSERT_STREQ(typed(), "\33[Fa");
}

UTEST(input, the_application_keypad)
{
    start(0);
    cells_term.app_keypad = true;
    key(SAPP_KEYCODE_KP_5, 0, 0);
    key(SAPP_KEYCODE_KP_SUBTRACT, 0, '-');
    key(SAPP_KEYCODE_KP_ENTER, 0, '\r');
    ASSERT_STREQ(typed(), "\33Ou\33Om\33OM");

    /* NumLock on types the characters whatever the keypad mode. */
    fake_locks = KEYBOARD_LED_NUMLOCK;
    key(SAPP_KEYCODE_KP_5, 0, '5');
    key(SAPP_KEYCODE_KP_SUBTRACT, 0, '-');
    ASSERT_STREQ(typed(), "5-");
    key(SAPP_KEYCODE_KP_5, SAPP_MODIFIER_SHIFT, 0);
    ASSERT_STREQ(typed(), "\33Ou");
    cells_term.app_keypad = false;
}

/* The system sends no character with Ctrl or Alt held, or one this drops. */
UTEST(input, keypad_chords)
{
    start(KEYBOARD_LED_NUMLOCK);
    key(SAPP_KEYCODE_KP_5, SAPP_MODIFIER_CTRL, 0);
    key(SAPP_KEYCODE_KP_5, SAPP_MODIFIER_ALT, '5');
    key(SAPP_KEYCODE_KP_DECIMAL, SAPP_MODIFIER_CTRL, 0);
    key(SAPP_KEYCODE_KP_MULTIPLY, SAPP_MODIFIER_ALT, '*');
    ASSERT_STREQ(typed(), "55.*");

    fake_locks = 0;
    key(SAPP_KEYCODE_KP_5, SAPP_MODIFIER_CTRL, 0);
    key(SAPP_KEYCODE_KP_8, SAPP_MODIFIER_ALT, 0);
    key(SAPP_KEYCODE_KP_MULTIPLY, SAPP_MODIFIER_CTRL, 0);
    ASSERT_STREQ(typed(), "\33[1;5E\33[1;3A*");
}

UTEST_MAIN()
