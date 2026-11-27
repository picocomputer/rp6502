/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "host/sokol/app/input.h"
#include "host/sokol/app/evq_machine.h"
#include "host/sokol/app/evq.h"
#include "host/sokol/app/entry.h"
#include "host/sokol/app/gfx.h"
#include "core/com/com.h"
#include "core/com/rln.h"
#include "core/hid/typed.h"
#include "core/com/manifold.h"
#include "core/hid/clip.h"
#include "host/host.h"
#include "tb_seed.h"
#include "core/hid/keyboard.h"
#include "core/hid/usage.h"
#include "core/term/vterm.h"
#include "core/term/vterm_mouse.h"
#include "core/hid/mouse.h"
#include "core/hid/tablet.h"
#include "core/sys/sys.h"
#include "core/sys/xram.h"
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
/* The desktop's clipboard is the bench's (tb_seed.h). */
const char *sapp_get_clipboard_string(void) { return tb_clipboard_text(); }
void sapp_cancel_quit(void) {}
void sapp_set_mouse_cursor(sapp_mouse_cursor cursor) { shown_cursor = cursor; }
void sapp_show_mouse(bool show) { shown = show; }
float sapp_dpi_scale(void) { return 1.0f; }
/* The canvas fills its width of the window, a pixel of each a pixel of the
 * other. */
float gfx_canvas_width(void)
{
    int w, h;
    vga_canvas_size(&w, &h);
    return (float)w;
}

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

/* What crossed the event queue, both ways, as the two threads would take it: the
 * machine takes what the window sent and says what changed, and the window
 * takes that. */
static void pump(void)
{
    evq_msg_t m;
    while (evq_machine_take(&m))
        machine_take(&m);
    machine_status(false, 0);
    while (evq_window_take(&m))
        if (m.kind == EVQ_STATUS)
            input_set_status(m.body);
}

/* An event as the window hands it over (app.c), and what it asks of the OS
 * done after. */
static void event(const sapp_event *e)
{
    pump();
    input_event(e);
    input_apply();
    pump();
}

/* The window's locks, as a frame reads and sends them. */
static void frame_locks(void)
{
    input_locks();
    pump();
}

/* The cursor, as a frame chooses and sets it. */
static void frame_cursor(void)
{
    pump();
    input_cursor_task();
    input_apply();
}

/* The capture a frame lets go, as the machine last said. */
static void frame_pointer(void)
{
    pump();
    input_pointer_task();
}

static bool to_tablet(void)
{
    pump();
    return input_pointer_to_tablet();
}

static bool wheel_to_tablet(void)
{
    pump();
    return input_wheel_to_tablet();
}

/* A key going down and up, with the character the system sends for it
 * between, if it sends one. */
static void key(sapp_keycode code, uint32_t modifiers, uint32_t ch)
{
    sapp_event e = {.type = SAPP_EVENTTYPE_KEY_DOWN, .key_code = code, .modifiers = modifiers};
    event(&e);
    if (ch)
    {
        e.type = SAPP_EVENTTYPE_CHAR;
        e.char_code = ch;
        event(&e);
    }
    e.type = SAPP_EVENTTYPE_KEY_UP;
    event(&e);
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
    frame_locks();
    ASSERT_EQ(keyboard_get_locks(), KEYBOARD_LED_CAPSLOCK);

    /* A lock key changes nothing until the system says the lock changed. */
    key(SAPP_KEYCODE_NUM_LOCK, 0, 0);
    key(SAPP_KEYCODE_CAPS_LOCK, 0, 0);
    key(SAPP_KEYCODE_SCROLL_LOCK, 0, 0);
    ASSERT_EQ(keyboard_get_locks(), KEYBOARD_LED_CAPSLOCK);
    ASSERT_STREQ(typed(), "");

    fake_locks = KEYBOARD_LED_NUMLOCK | KEYBOARD_LED_SCROLLLOCK;
    frame_locks();
    ASSERT_EQ(keyboard_get_locks(), KEYBOARD_LED_NUMLOCK | KEYBOARD_LED_SCROLLLOCK);
}

UTEST(input, a_lock_key_shows_in_the_bitmap)
{
    start(0);
    sapp_event e = {.type = SAPP_EVENTTYPE_KEY_DOWN, .key_code = SAPP_KEYCODE_NUM_LOCK};
    event(&e);
    ASSERT_TRUE(keyboard_key_down(HID_KEY_NUM_LOCK));
    e.type = SAPP_EVENTTYPE_KEY_UP;
    event(&e);
    ASSERT_FALSE(keyboard_key_down(HID_KEY_NUM_LOCK));
}

/* The lock is read as a key goes down, so a key pressed in the frame the lock
 * changed is typed by the new lock. */
UTEST(input, a_key_is_typed_by_the_lock_it_went_down_with)
{
    start(0);
    frame_locks();
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

/* What the keys since the last call typed and pasted, a paste's text
 * among them. */
static const char *pasted(void)
{
    static char buf[64];
    buf[com_read(COM_SOURCE_KEYBOARD, buf, sizeof buf - 1)] = 0;
    return buf;
}

/* The paste sokol makes of Ctrl+V on X11 and Windows, and of Cmd+V on
 * macOS, after the key went down. */
static void sokol_pasted(void)
{
    sapp_event e = {.type = SAPP_EVENTTYPE_CLIPBOARD_PASTED};
    event(&e);
}

/* Plain Ctrl+V is the program's 0x16, and the paste sokol makes of it is
 * no paste. */
UTEST(input, ctrl_v_types_syn)
{
    start(0);
    host_clipboard_set("text");
    key(SAPP_KEYCODE_V, SAPP_MODIFIER_CTRL, 0);
    sokol_pasted();
    ASSERT_STREQ(pasted(), "\x16");
}

/* The paste keys paste the clipboard, read as the key goes down; on a Mac,
 * Cmd+V's paste from sokol after it pastes nothing more. */
UTEST(input, the_paste_keys_paste_the_clipboard)
{
    start(0);
    tb_mac_keys = true;
    host_clipboard_set("text");
    key(SAPP_KEYCODE_V, SAPP_MODIFIER_CTRL | SAPP_MODIFIER_SHIFT, 0);
    ASSERT_STREQ(pasted(), "text");
    key(SAPP_KEYCODE_INSERT, SAPP_MODIFIER_SHIFT, 0);
    ASSERT_STREQ(pasted(), "text");
    sapp_event e = {.type = SAPP_EVENTTYPE_KEY_DOWN, .key_code = SAPP_KEYCODE_V,
                    .modifiers = SAPP_MODIFIER_SUPER};
    event(&e);
    e.type = SAPP_EVENTTYPE_CHAR;
    e.char_code = 'v';
    event(&e);
    sokol_pasted();
    e.type = SAPP_EVENTTYPE_KEY_UP;
    event(&e);
    tb_mac_keys = false;
    ASSERT_STREQ(pasted(), "text");
}

/* A paste key held pastes once: its repeats are the chord still held. */
UTEST(input, a_held_paste_key_pastes_once)
{
    start(0);
    host_clipboard_set("text");
    sapp_event e = {.type = SAPP_EVENTTYPE_KEY_DOWN, .key_code = SAPP_KEYCODE_INSERT,
                    .modifiers = SAPP_MODIFIER_SHIFT};
    event(&e);
    e.key_repeat = true;
    event(&e);
    event(&e);
    e.type = SAPP_EVENTTYPE_KEY_UP;
    event(&e);
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
 * which types 0x16, and a held paste key's repeat, which acts once. The
 * copy and cut keys are the player's alone, always, their repeats too. */
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
    tb_mac_keys = true;
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
    } own_keys[] = {
        {SAPP_KEYCODE_C, SAPP_MODIFIER_CTRL | SAPP_MODIFIER_SHIFT},
        {SAPP_KEYCODE_INSERT, SAPP_MODIFIER_CTRL},
        {SAPP_KEYCODE_C, SAPP_MODIFIER_SUPER},
        {SAPP_KEYCODE_X, SAPP_MODIFIER_CTRL | SAPP_MODIFIER_SHIFT},
        {SAPP_KEYCODE_DELETE, SAPP_MODIFIER_SHIFT},
        {SAPP_KEYCODE_X, SAPP_MODIFIER_SUPER},
    };
    for (size_t i = 0; i < sizeof own_keys / sizeof *own_keys; i++)
        for (int repeat = 0; repeat < 2; repeat++)
        {
            sapp_event e = key_down(own_keys[i].code, own_keys[i].modifiers, repeat);
            ASSERT_FALSE(input_paste_key(&e));
            ASSERT_TRUE(input_key_withheld(&e));
        }
    tb_mac_keys = false;
    sapp_event c = key_down(SAPP_KEYCODE_C, SAPP_MODIFIER_CTRL, false);
    ASSERT_FALSE(input_key_withheld(&c));
    c = key_down(SAPP_KEYCODE_X, SAPP_MODIFIER_CTRL, false);
    ASSERT_FALSE(input_key_withheld(&c));
    c = key_down(SAPP_KEYCODE_DELETE, 0, false);
    ASSERT_FALSE(input_key_withheld(&c));
    /* Off a Mac, Cmd+C, Cmd+X and Cmd+V are the browser's. */
    c = key_down(SAPP_KEYCODE_C, SAPP_MODIFIER_SUPER, false);
    ASSERT_FALSE(input_key_withheld(&c));
    c = key_down(SAPP_KEYCODE_X, SAPP_MODIFIER_SUPER, false);
    ASSERT_FALSE(input_key_withheld(&c));
    c = key_down(SAPP_KEYCODE_V, SAPP_MODIFIER_SUPER, false);
    ASSERT_FALSE(input_paste_key(&c));
    ASSERT_FALSE(input_key_withheld(&c));
    sapp_event e = {.type = SAPP_EVENTTYPE_CLIPBOARD_PASTED};
    ASSERT_FALSE(input_paste_key(&e));
    ASSERT_FALSE(input_key_withheld(&e));
}

static char line_got[RLN_LINE_MAX];

static void line_read(bool timeout, const char *buf)
{
    (void)timeout;
    strcpy(line_got, buf);
}

/* The line editor's turn. */
static void line_loop(void)
{
    for (int i = 0; i < 64; i++)
        rln_task();
}

/* A line being read, with text typed into it. */
static void line_start(const char *text)
{
    sys_init();
    manifold_set_paced(false);
    start(0);
    rln_read_line(line_read, NULL, NULL);
    line_loop();
    typed_push(text, strlen(text));
    line_loop();
}

/* A cut key with no line being read types what the key types, once, and
 * its repeat types nothing. Cmd+X types nothing at all. */
UTEST(input, a_cut_key_with_no_line_read_types_its_bytes)
{
    sys_init();
    start(0);
    tb_mac_keys = true;
    sapp_event e = key_down(SAPP_KEYCODE_X, SAPP_MODIFIER_CTRL | SAPP_MODIFIER_SHIFT, false);
    event(&e);
    e.key_repeat = true;
    event(&e);
    ASSERT_STREQ(pasted(), "\x18");
    key(SAPP_KEYCODE_DELETE, SAPP_MODIFIER_SHIFT, 0);
    ASSERT_STREQ(pasted(), "\33[3;2~");
    key(SAPP_KEYCODE_X, SAPP_MODIFIER_SUPER, 'x');
    ASSERT_STREQ(pasted(), "");
    tb_mac_keys = false;
}

/* Each cut key cuts the line's selection to the clipboard, in its place
 * among the keys, and what it would type is not typed. */
UTEST(input, the_cut_keys_cut_in_the_line)
{
    static const struct
    {
        sapp_keycode code;
        uint32_t modifiers;
    } cut_keys[] = {
        {SAPP_KEYCODE_X, SAPP_MODIFIER_CTRL | SAPP_MODIFIER_SHIFT},
        {SAPP_KEYCODE_DELETE, SAPP_MODIFIER_SHIFT},
        {SAPP_KEYCODE_X, SAPP_MODIFIER_SUPER},
    };
    static const char *const cut[] = {"f", "e", "d"};
    line_start("abcdef");
    tb_mac_keys = true;
    for (size_t i = 0; i < sizeof cut_keys / sizeof *cut_keys; i++)
    {
        key(SAPP_KEYCODE_LEFT, SAPP_MODIFIER_SHIFT, 0);
        key(cut_keys[i].code, cut_keys[i].modifiers, 0);
        line_loop();
        ASSERT_STREQ(host_clipboard_get(), cut[i]);
    }
    tb_mac_keys = false;
    typed_push("\r", 1);
    line_loop();
    ASSERT_STREQ(line_got, "abc");
}

/* Each copy key puts the line's selection in the clipboard, and types
 * nothing. */
UTEST(input, the_copy_keys_set_the_clipboard)
{
    line_start("copied");
    key(SAPP_KEYCODE_HOME, SAPP_MODIFIER_SHIFT, 0);
    host_clipboard_set("");
    key(SAPP_KEYCODE_C, SAPP_MODIFIER_CTRL | SAPP_MODIFIER_SHIFT, 0);
    line_loop();
    ASSERT_STREQ(host_clipboard_get(), "copied");
    host_clipboard_set("");
    key(SAPP_KEYCODE_INSERT, SAPP_MODIFIER_CTRL, 0);
    line_loop();
    ASSERT_STREQ(host_clipboard_get(), "copied");
    host_clipboard_set("");
    tb_mac_keys = true;
    key(SAPP_KEYCODE_C, SAPP_MODIFIER_SUPER, 'c');
    tb_mac_keys = false;
    line_loop();
    ASSERT_STREQ(host_clipboard_get(), "copied");
    typed_push("\r", 1);
    line_loop();
    ASSERT_STREQ(line_got, "copied");
}

/* With no line being read, a copy key does nothing and types nothing. */
UTEST(input, a_copy_key_with_no_line_read_is_dropped)
{
    sys_init();
    start(0);
    host_clipboard_set("kept");
    key(SAPP_KEYCODE_C, SAPP_MODIFIER_CTRL | SAPP_MODIFIER_SHIFT, 0);
    key(SAPP_KEYCODE_A, 0, 'a');
    ASSERT_STREQ(pasted(), "a");
    ASSERT_STREQ(host_clipboard_get(), "kept");
}

static void pointer_to(float x, float y)
{
    sapp_event e = {.type = SAPP_EVENTTYPE_MOUSE_MOVE, .mouse_x = x, .mouse_y = y};
    event(&e);
}

/* With no program on the tablet, the console's terminal has the pointer
 * over the console, as the arrow. A canvas with no mode 0 on it does not
 * want the pointer, so the window's own cursor shows, but the tablet still
 * has it. */
UTEST(input, unmapped_over_the_console_the_pointer_feeds_the_tablet)
{
    sys_init();
    tablet_host_clear();
    vterm_mouse_task();
    pointer_to(100, 100);
    ASSERT_EQ(RING_LEN(&vterm_mouse_queue), 1u);
    vterm_mouse_task();
    ASSERT_TRUE(vterm_mouse_acts());
    frame_cursor();
    ASSERT_TRUE(shown);
    ASSERT_EQ(shown_cursor, SAPP_MOUSECURSOR_ARROW);
    /* The terminal does nothing with the wheel, so a page keeps it, and the
     * tablet has it too, as it has every pointer. */
    ASSERT_TRUE(to_tablet());
    ASSERT_FALSE(wheel_to_tablet());
    sapp_event w = {.type = SAPP_EVENTTYPE_MOUSE_SCROLL, .scroll_y = 1.0f};
    event(&w);
    ASSERT_EQ(RING_LEN(&vterm_mouse_queue), 1u);
    ASSERT_EQ(RING_AT(&vterm_mouse_queue, 0)->wheel, 1);
    vterm_mouse_task();

    /* A canvas with no mode 0 on it does not want the pointer, and the
     * tablet still has it. */
    ASSERT_TRUE(vga_canvas_select(vga_canvas_320_240));
    frame_pointer();
    vterm_mouse_task();
    ASSERT_FALSE(to_tablet());
    pointer_to(50, 50);
    ASSERT_EQ(RING_LEN(&vterm_mouse_queue), 1u);
    ASSERT_EQ(RING_AT(&vterm_mouse_queue, 0)->x, 50);
    vterm_mouse_task();
    frame_cursor();
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
    event(&e);
    e.type = SAPP_EVENTTYPE_MOUSE_MOVE;
    e.modifiers = SAPP_MODIFIER_LMB;
    e.mouse_x = 104;
    event(&e);
    e.mouse_x = 108;
    event(&e);
    e.type = SAPP_EVENTTYPE_MOUSE_UP;
    event(&e);
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

/* The window's focus is told while a program asks (?1004): CSI O as it
 * goes, CSI I as it comes back, in order with the keys. Without the mode
 * nothing is, and setting it tells nothing until the focus changes. */
UTEST(input, the_focus_is_told_while_a_program_asks)
{
    sys_init();
    start(0);
    /* As the window says when it starts (app_init). */
    evq_to_machine(EVQ_FOCUSABLE, NULL, 0);
    pump();
    vterm_focus(false);
    vterm_mouse_task();
    vterm_focus(true);
    vterm_mouse_task();
    ASSERT_STREQ(typed(), "");
    manifold_write_to(COM_TERM_SCREEN, "\33[?1004h", 8);
    vterm_mouse_task();
    ASSERT_STREQ(typed(), "");
    /* A change that comes back before the task looks is none. */
    vterm_focus(false);
    vterm_focus(true);
    vterm_mouse_task();
    ASSERT_STREQ(typed(), "");
    key(SAPP_KEYCODE_A, 0, 'a');
    vterm_focus(false);
    vterm_mouse_task();
    ASSERT_STREQ(typed(), "a\33[O");
    vterm_focus(true);
    vterm_mouse_task();
    ASSERT_STREQ(typed(), "\33[I");
    manifold_write_to(COM_TERM_SCREEN, "\33[?1004l", 8);
    vterm_focus(false);
    vterm_mouse_task();
    vterm_focus(true);
    vterm_mouse_task();
    ASSERT_STREQ(typed(), "");
}

/* A trackpad scrolls by fractions of a notch, which add up to whole
 * notches of the tablet's wheel, the rest kept for the next. */
UTEST(input, small_scrolls_add_up)
{
    sys_init();
    start(0);
    tablet_host_clear();
    vterm_mouse_task();
    manifold_write_to(COM_TERM_SCREEN, "\33[?1000h", 8);
    pointer_to(100, 100);
    vterm_mouse_task();
    ASSERT_TRUE(wheel_to_tablet());
    sapp_event w = {.type = SAPP_EVENTTYPE_MOUSE_SCROLL, .scroll_y = 0.4f};
    event(&w);
    vterm_mouse_task();
    event(&w);
    vterm_mouse_task();
    ASSERT_STREQ(typed(), "");
    event(&w);
    vterm_mouse_task();
    ASSERT_STREQ(typed(), "\33[M`-'");
    w.scroll_y = -1.3f;
    event(&w);
    vterm_mouse_task();
    ASSERT_STREQ(typed(), "\33[Ma-'");
    manifold_write_to(COM_TERM_SCREEN, "\33[?1000l", 8);
    tablet_host_clear();
    vterm_mouse_task();
}

/* The pointer uncaptured is the host's tablet, and captured, as for a
 * first-person game, its mouse. A program that maps the mouse has the first
 * click capture it, whatever else it maps, and the click is spent on that.
 * Captured, the mouse moves the mouse's block and the tablet's pointer both,
 * from where the tablet had it. Let go, the mouse is gone, and the pointer
 * is the tablet's again where the system has it. */
UTEST(input, a_click_captures_the_mouse_for_a_program_that_maps_it)
{
    sys_init();
    captured = false;
    pointer_to(100, 100);
    frame_pointer();
    input_apply();
    vterm_mouse_task();
    ASSERT_TRUE(to_tablet());

    /* With no mouse mapped, a click is the tablet's. */
    sapp_event e = {.type = SAPP_EVENTTYPE_MOUSE_DOWN, .mouse_button = SAPP_MOUSEBUTTON_LEFT,
                    .mouse_x = 100, .mouse_y = 100};
    event(&e);
    ASSERT_FALSE(captured);
    ASSERT_EQ(RING_LEN(&vterm_mouse_queue), 1u);
    ASSERT_EQ(RING_AT(&vterm_mouse_queue, 0)->flags, TABLET_FLAG_HOVER | TABLET_FLAG_LEFT);
    e.type = SAPP_EVENTTYPE_MOUSE_UP;
    event(&e);
    vterm_mouse_task();

    /* Mapped, with the tablet mapped too, the first click only captures. */
    ASSERT_TRUE(mouse_xreg(0xFE00));
    ASSERT_TRUE(tablet_xreg(0xFF00));
    frame_pointer();
    input_apply();
    ASSERT_TRUE(to_tablet());
    e.type = SAPP_EVENTTYPE_MOUSE_DOWN;
    event(&e);
    ASSERT_TRUE(captured);
    ASSERT_EQ(RING_LEN(&vterm_mouse_queue), 0u);
    e.type = SAPP_EVENTTYPE_MOUSE_UP;
    event(&e);

    /* The system takes a while to capture, and until it has, the pointer is
     * the tablet's, the spent click still held: it presses nothing there. */
    captured = false; /* the system let it go */
    frame_pointer();
    input_apply();
    pump();
    vterm_mouse_task();
    pump();
    e.type = SAPP_EVENTTYPE_MOUSE_DOWN;
    input_event(&e); /* not yet applied: the capture is still to come */
    sapp_event held = {.type = SAPP_EVENTTYPE_MOUSE_MOVE, .mouse_x = 104, .mouse_y = 100,
                       .modifiers = SAPP_MODIFIER_LMB};
    input_event(&held);
    pump();
    ASSERT_EQ(RING_LEN(&vterm_mouse_queue), 1u);
    ASSERT_EQ(RING_AT(&vterm_mouse_queue, 0)->flags, TABLET_FLAG_HOVER);
    vterm_mouse_task();
    input_apply();
    ASSERT_TRUE(captured);
    e.type = SAPP_EVENTTYPE_MOUSE_UP;
    event(&e);

    /* Captured, the pointer stops being the system's and moves as a mouse,
     * the system drawing it. 20 pixels of a 640-wide canvas are 20 counts. */
    const uint8_t mx = xram[0xFE00 + 1];
    sapp_event m = {.type = SAPP_EVENTTYPE_MOUSE_MOVE, .mouse_dx = 20.0f};
    event(&m);
    ASSERT_EQ((uint8_t)(xram[0xFE00 + 1] - mx), 20);
    ASSERT_GE(RING_LEN(&vterm_mouse_queue), 1u);
    const vterm_mouse_event_t *last = RING_AT(&vterm_mouse_queue, RING_LEN(&vterm_mouse_queue) - 1);
    ASSERT_EQ(last->x, 124);
    ASSERT_EQ(last->y, 100);
    ASSERT_EQ(last->flags, TABLET_FLAG_HOVER);
    ASSERT_FALSE(tablet_host_cursor_shown());
    vterm_mouse_task();
    e.type = SAPP_EVENTTYPE_MOUSE_DOWN;
    event(&e);
    ASSERT_EQ(xram[0xFE00], 0x01);
    ASSERT_EQ(RING_AT(&vterm_mouse_queue, 0)->flags, TABLET_FLAG_HOVER | TABLET_FLAG_LEFT);
    vterm_mouse_task();

    /* Esc lets it go, and the held button with it. */
    key(SAPP_KEYCODE_ESCAPE, 0, 0);
    ASSERT_FALSE(captured);
    pointer_to(130, 100);
    ASSERT_EQ(xram[0xFE00], 0);
    last = RING_AT(&vterm_mouse_queue, RING_LEN(&vterm_mouse_queue) - 1);
    ASSERT_EQ(last->x, 130);
    ASSERT_EQ(last->flags, TABLET_FLAG_HOVER);
    ASSERT_TRUE(tablet_host_cursor_shown());
    vterm_mouse_task();

    /* A program that unmaps the mouse lets the capture go. */
    e.type = SAPP_EVENTTYPE_MOUSE_DOWN;
    event(&e);
    ASSERT_TRUE(captured);
    ASSERT_TRUE(mouse_xreg(0xFFFF));
    frame_pointer();
    input_apply();
    ASSERT_FALSE(captured);

    ASSERT_TRUE(tablet_xreg(0xFFFF));
    tablet_host_clear();
    frame_pointer();
    input_apply();
    vterm_mouse_task();
}

UTEST_MAIN()
