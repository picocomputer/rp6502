/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "host/sokol/app/input.h"
#include "host/sokol/app/entry.h"
#include "host/sokol/app/gfx.h"
#include "core/com/com.h"
#include "core/com/manifold.h"
#include "core/hid/clip.h"
#include "core/hid/paste.h"
#include "host/host.h"
#include "core/hid/keyboard.h"
#include "core/hid/usage.h"
#include "core/term/vterm.h"
#include "core/term/vterm_mouse.h"
#include "core/hid/mouse.h"
#include "core/hid/tablet.h"
#include "core/sys/sys.h"
#include "core/vga/vga.h"
#include "sokol/sokol_app.h"

#include "utest.h"

#include <string.h>

/* The system's locks, which the window reads and never writes. */
static uint8_t fake_locks;

uint8_t host_locks(void) { return fake_locks; }

/* Whether the window has captured the pointer, and the pointer it shows, as
 * input.c last set them. */
static bool captured;
static sapp_mouse_cursor shown_cursor;
static bool shown;

/* What input.c calls in a window, which this test has none of. */
void sapp_lock_mouse(bool lock) { captured = lock; }
bool sapp_mouse_locked(void) { return captured; }
sapp_desc sapp_query_desc(void) { return (sapp_desc){0}; }
const char *sapp_get_clipboard_string(void) { return ""; }
void sapp_cancel_quit(void) {}
void sapp_set_mouse_cursor(sapp_mouse_cursor cursor) { shown_cursor = cursor; }
void sapp_show_mouse(bool show) { shown = show; }
float sapp_dpi_scale(void) { return 1.0f; }
float gfx_canvas_scale(void) { return 1.0f; }

/* The window is the canvas, a pixel of each a pixel of the other. */
bool gfx_canvas_from_fb(float fx, float fy, int *cx, int *cy)
{
    int w, h;
    vga_canvas_size(&w, &h);
    if (fx < 0 || fy < 0 || fx >= (float)w || fy >= (float)h)
        return false;
    *cx = (int)fx;
    *cy = (int)fy;
    return true;
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
    vterm.app_keypad = false;
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
    vterm.app_keypad = true;
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
    vterm.app_keypad = false;
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

/* What the keys since the last call typed and pasted, the paste staged as
 * the main loop stages it. */
static const char *pasted(void)
{
    static char buf[64];
    size_t n = 0;
    for (int i = 0; i < 1000 && n < sizeof buf - 1; i++)
    {
        paste_task();
        const size_t got = com_read(COM_SOURCE_KEYBOARD, buf + n, sizeof buf - 1 - n);
        n += got;
        if (!got && !paste_busy())
            break;
    }
    buf[n] = 0;
    return buf;
}

/* The paste sokol makes of Ctrl+V on X11 and Windows, and of Cmd+V on
 * macOS, after the key went down. */
static void sokol_pasted(void)
{
    sapp_event e = {.type = SAPP_EVENTTYPE_CLIPBOARD_PASTED};
    input_event(&e);
}

/* Plain Ctrl+V is the program's 0x16, and the paste sokol makes of it is
 * no paste. */
UTEST(input, ctrl_v_types_syn)
{
    start(0);
    ASSERT_TRUE(host_clipboard_set("text"));
    key(SAPP_KEYCODE_V, SAPP_MODIFIER_CTRL, 0);
    sokol_pasted();
    ASSERT_STREQ(pasted(), "\x16");
}

/* The paste keys paste the clipboard as they go down; Cmd+V's paste from
 * sokol after it pastes nothing more. */
UTEST(input, the_paste_keys_paste_the_clipboard)
{
    start(0);
    ASSERT_TRUE(host_clipboard_set("text"));
    key(SAPP_KEYCODE_V, SAPP_MODIFIER_CTRL | SAPP_MODIFIER_SHIFT, 0);
    ASSERT_STREQ(pasted(), "text");
    key(SAPP_KEYCODE_INSERT, SAPP_MODIFIER_SHIFT, 0);
    ASSERT_STREQ(pasted(), "text");
    sapp_event e = {.type = SAPP_EVENTTYPE_KEY_DOWN, .key_code = SAPP_KEYCODE_V,
                    .modifiers = SAPP_MODIFIER_SUPER};
    input_event(&e);
    e.type = SAPP_EVENTTYPE_CHAR;
    e.char_code = 'v';
    input_event(&e);
    sokol_pasted();
    e.type = SAPP_EVENTTYPE_KEY_UP;
    input_event(&e);
    ASSERT_STREQ(pasted(), "text");
}

/* A key down as the web player sees it, a repeat or not. */
static sapp_event key_down(sapp_keycode code, uint32_t modifiers, bool repeat)
{
    return (sapp_event){.type = SAPP_EVENTTYPE_KEY_DOWN, .key_code = code,
                        .modifiers = modifiers, .key_repeat = repeat};
}

/* The web player leaves a paste key's press to the browser, which makes the
 * paste event that pastes, and cancels what must make none: plain Ctrl+V,
 * which types 0x16, and a held paste key's repeat, which acts once. */
UTEST(input, the_web_player_lets_a_paste_key_paste_once)
{
    static const struct
    {
        sapp_keycode code;
        uint32_t modifiers;
    } paste_keys[] = {
        {SAPP_KEYCODE_V, SAPP_MODIFIER_CTRL | SAPP_MODIFIER_SHIFT},
        {SAPP_KEYCODE_INSERT, SAPP_MODIFIER_SHIFT},
        {SAPP_KEYCODE_V, SAPP_MODIFIER_SUPER},
    };
    for (size_t i = 0; i < sizeof paste_keys / sizeof *paste_keys; i++)
    {
        sapp_event e = key_down(paste_keys[i].code, paste_keys[i].modifiers, false);
        ASSERT_TRUE(input_paste_key(&e));
        ASSERT_FALSE(input_key_withheld(&e));
        e = key_down(paste_keys[i].code, paste_keys[i].modifiers, true);
        ASSERT_FALSE(input_paste_key(&e));
        ASSERT_TRUE(input_key_withheld(&e));
    }
    for (int repeat = 0; repeat < 2; repeat++)
    {
        sapp_event e = key_down(SAPP_KEYCODE_V, SAPP_MODIFIER_CTRL, repeat);
        ASSERT_FALSE(input_paste_key(&e));
        ASSERT_TRUE(input_key_withheld(&e));
        e = key_down(SAPP_KEYCODE_V, 0, repeat);
        ASSERT_FALSE(input_paste_key(&e));
        ASSERT_FALSE(input_key_withheld(&e));
    }
    const struct
    {
        sapp_keycode code;
        uint32_t modifiers;
    } copy_keys[] = {
        {SAPP_KEYCODE_C, SAPP_MODIFIER_CTRL | SAPP_MODIFIER_SHIFT},
        {SAPP_KEYCODE_INSERT, SAPP_MODIFIER_CTRL},
        {SAPP_KEYCODE_C, SAPP_MODIFIER_SUPER},
    };
    for (size_t i = 0; i < sizeof copy_keys / sizeof *copy_keys; i++)
        for (int repeat = 0; repeat < 2; repeat++)
        {
            sapp_event e = key_down(copy_keys[i].code, copy_keys[i].modifiers, repeat);
            ASSERT_FALSE(input_paste_key(&e));
            ASSERT_TRUE(input_key_withheld(&e));
        }
    sapp_event c = key_down(SAPP_KEYCODE_C, SAPP_MODIFIER_CTRL, false);
    ASSERT_FALSE(input_key_withheld(&c));
    sapp_event e = {.type = SAPP_EVENTTYPE_CLIPBOARD_PASTED};
    ASSERT_FALSE(input_paste_key(&e));
    ASSERT_FALSE(input_key_withheld(&e));
}

/* The copy keys put the primary in the clipboard, and type nothing. */
UTEST(input, the_copy_keys_set_the_clipboard)
{
    sys_init();
    manifold_set_paced(false);
    start(0);
    const char *show = "\33[H\33[2Jcopied\r\n";
    manifold_write_to(COM_TERM_SCREEN, show, strlen(show));
    clip_copy(CLIP_PRIMARY, vterm_get_shown(&vterm)->shown, 0, 0, 0, 6);
    ASSERT_TRUE(host_clipboard_set(""));
    key(SAPP_KEYCODE_C, SAPP_MODIFIER_CTRL | SAPP_MODIFIER_SHIFT, 0);
    ASSERT_STREQ(host_clipboard_get(), "copied");
    ASSERT_TRUE(host_clipboard_set(""));
    key(SAPP_KEYCODE_INSERT, SAPP_MODIFIER_CTRL, 0);
    ASSERT_STREQ(host_clipboard_get(), "copied");
    ASSERT_TRUE(host_clipboard_set(""));
    key(SAPP_KEYCODE_C, SAPP_MODIFIER_SUPER, 'c');
    ASSERT_STREQ(host_clipboard_get(), "copied");
    ASSERT_STREQ(pasted(), "");
}

static void pointer_to(float x, float y)
{
    sapp_event e = {.type = SAPP_EVENTTYPE_MOUSE_MOVE, .mouse_x = x, .mouse_y = y};
    input_event(&e);
}

/* With no program on the tablet, the console's terminal has the pointer
 * over the console, as the arrow; a canvas with no mode 0 on it takes no
 * pointer, and the window's own shows. */
UTEST(input, unmapped_over_the_console_the_pointer_feeds_the_tablet)
{
    sys_init();
    tablet_host_clear();
    vterm_mouse_task();
    pointer_to(100, 100);
    ASSERT_EQ(RING_LEN(&vterm_mouse_queue), 1u);
    vterm_mouse_task();
    ASSERT_TRUE(vterm_mouse_acts());
    input_update_cursor();
    ASSERT_TRUE(shown);
    ASSERT_EQ(shown_cursor, SAPP_MOUSECURSOR_ARROW);
    /* The terminal does nothing with the wheel, so a page keeps it, and
     * nothing of it reaches the tablet. */
    ASSERT_TRUE(input_pointer_to_tablet());
    ASSERT_FALSE(input_wheel_to_tablet());
    sapp_event w = {.type = SAPP_EVENTTYPE_MOUSE_SCROLL, .scroll_y = 1.0f};
    input_event(&w);
    ASSERT_EQ(RING_LEN(&vterm_mouse_queue), 0u);

    ASSERT_TRUE(vga_canvas_select(vga_canvas_320_240));
    pointer_to(50, 50);
    ASSERT_EQ(RING_LEN(&vterm_mouse_queue), 0u);
    input_update_cursor();
    ASSERT_TRUE(shown);
    ASSERT_EQ(shown_cursor, SAPP_MOUSECURSOR_DEFAULT);
    ASSERT_TRUE(vga_canvas_select(vga_canvas_console));
    tablet_host_clear();
    vterm_mouse_task();
}

/* A click that comes and goes inside one frame's events reaches the console's
 * mouse as both its press and its release, and the motion between them as
 * one change. */
UTEST(input, a_press_and_release_in_one_burst_give_both_edges)
{
    sys_init();
    tablet_host_clear();
    vterm_mouse_task();
    pointer_to(100, 100);
    sapp_event e = {.type = SAPP_EVENTTYPE_MOUSE_DOWN, .mouse_button = SAPP_MOUSEBUTTON_LEFT,
                    .mouse_x = 100, .mouse_y = 100};
    input_event(&e);
    e.type = SAPP_EVENTTYPE_MOUSE_MOVE;
    e.modifiers = SAPP_MODIFIER_LMB;
    e.mouse_x = 104;
    input_event(&e);
    e.mouse_x = 108;
    input_event(&e);
    e.type = SAPP_EVENTTYPE_MOUSE_UP;
    input_event(&e);
    ASSERT_EQ(RING_LEN(&vterm_mouse_queue), 4u);
    const vterm_mouse_event_t *c = RING_AT(&vterm_mouse_queue, 1);
    ASSERT_TRUE(c->edge);
    ASSERT_EQ(c->flags, TABLET_FLAG_HOVER | TABLET_FLAG_LEFT);
    ASSERT_EQ(c->x, 100);
    c = RING_AT(&vterm_mouse_queue, 2);
    ASSERT_FALSE(c->edge);
    ASSERT_TRUE(c->moved);
    ASSERT_EQ(c->x, 108);
    c = RING_AT(&vterm_mouse_queue, 3);
    ASSERT_TRUE(c->edge);
    ASSERT_EQ(c->flags, TABLET_FLAG_HOVER);
    vterm_mouse_task();
    ASSERT_TRUE(vterm_mouse_acts());
    tablet_host_clear();
    vterm_mouse_task();
}

/* A program that maps only the mouse is not on the tablet, even over the
 * console: the pointer is the mouse's, captured by the first click, and the
 * terminal has none of it. A program that maps the tablet too has it
 * uncaptured, and the terminal shares it. */
UTEST(input, a_program_that_maps_only_the_mouse_captures_it)
{
    sys_init();
    captured = false;
    pointer_to(100, 100);
    input_pointer_task();
    vterm_mouse_task();
    ASSERT_TRUE(input_pointer_to_tablet());

    /* A press held over the console as the mouse is mapped is let go on the
     * tablet, so the terminal is not left in a drag. */
    sapp_event e = {.type = SAPP_EVENTTYPE_MOUSE_DOWN, .mouse_button = SAPP_MOUSEBUTTON_LEFT,
                    .mouse_x = 100, .mouse_y = 100};
    input_event(&e);
    vterm_mouse_task();
    ASSERT_TRUE(vterm_mouse_acts());
    ASSERT_TRUE(mouse_xreg(0xFE00));
    ASSERT_FALSE(input_pointer_to_tablet());
    input_pointer_task();
    ASSERT_EQ(RING_LEN(&vterm_mouse_queue), 1u);
    ASSERT_EQ(RING_AT(&vterm_mouse_queue, 0)->flags, 0);
    vterm_mouse_task();
    /* It is an edge: once let go, the tablet is left to whatever else puts
     * a contact there, as a script does. */
    tablet_host_pointer(100, 100, TABLET_FLAG_LEFT, true);
    input_pointer_task();
    ASSERT_EQ(RING_LEN(&vterm_mouse_queue), 1u);
    ASSERT_EQ(RING_AT(&vterm_mouse_queue, 0)->flags, TABLET_FLAG_HOVER | TABLET_FLAG_LEFT);
    tablet_host_clear();
    vterm_mouse_task();
    input_update_cursor();
    ASSERT_TRUE(shown);
    ASSERT_EQ(shown_cursor, SAPP_MOUSECURSOR_DEFAULT);

    /* The first click only captures. */
    e.type = SAPP_EVENTTYPE_MOUSE_UP;
    input_event(&e);
    pointer_to(120, 100);
    ASSERT_EQ(RING_LEN(&vterm_mouse_queue), 0u);
    ASSERT_FALSE(captured);
    e.type = SAPP_EVENTTYPE_MOUSE_DOWN;
    input_event(&e);
    ASSERT_TRUE(captured);
    ASSERT_EQ(RING_LEN(&vterm_mouse_queue), 0u);
    input_pointer_task();
    ASSERT_TRUE(captured);

    /* Mapping the tablet as well lets the capture go and feeds the tablet,
     * the wheel included. */
    ASSERT_TRUE(tablet_xreg(0xFF00));
    ASSERT_TRUE(input_pointer_to_tablet());
    ASSERT_TRUE(input_wheel_to_tablet());
    input_pointer_task();
    ASSERT_FALSE(captured);
    pointer_to(130, 100);
    ASSERT_EQ(RING_LEN(&vterm_mouse_queue), 1u);

    ASSERT_TRUE(tablet_xreg(0xFFFF));
    ASSERT_TRUE(mouse_xreg(0xFFFF));
    tablet_host_clear();
    input_pointer_task();
    vterm_mouse_task();
}

UTEST_MAIN()
