/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "host/sokol/app/input.h"

#include "host/sokol/app/gfx.h"
#include "host/sokol/app/entry.h"
#include "core/hid/keyboard.h"
#include "core/hid/usage.h"
#include "core/hid/key.h"
#include "core/hid/mouse.h"
#include "core/hid/tablet.h"
#ifdef EMU_WITH_DEBUGGER
#include "host/sokol/dbg/dbgui.h"
#include "core/dap/dbg.h"
#endif
#include "sokol/sokol_app.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

_Static_assert(EVENT_TOUCHES == SAPP_MAX_TOUCHPOINTS, "a touch message holds every contact");

/* What the machine last told the window. Until it has, nothing is mapped and
 * nothing is the tablet's. */
static evq_status_t input_told;

void input_set_status(const evq_status_t *s)
{
    input_told = *s;
}

const evq_status_t *input_status(void)
{
    return &input_told;
}

/* A sokol keycode as a USB HID keyboard usage id, for the keyboard bitmap.
 * 0 means the key has no usage id and is not reported. */
static uint8_t sokol_to_hid(int kc)
{
    if (kc >= SAPP_KEYCODE_A && kc <= SAPP_KEYCODE_Z)
        return (uint8_t)(0x04 + (kc - SAPP_KEYCODE_A));
    if (kc >= SAPP_KEYCODE_1 && kc <= SAPP_KEYCODE_9)
        return (uint8_t)(0x1E + (kc - SAPP_KEYCODE_1));
    if (kc >= SAPP_KEYCODE_F1 && kc <= SAPP_KEYCODE_F12)
        return (uint8_t)(0x3A + (kc - SAPP_KEYCODE_F1));
    if (kc >= SAPP_KEYCODE_KP_1 && kc <= SAPP_KEYCODE_KP_9)
        return (uint8_t)(0x59 + (kc - SAPP_KEYCODE_KP_1));
    switch (kc)
    {
    case SAPP_KEYCODE_KP_0: return 0x62;
    case SAPP_KEYCODE_KP_DECIMAL: return 0x63;
    case SAPP_KEYCODE_KP_DIVIDE: return HID_KEY_KEYPAD_DIVIDE;
    case SAPP_KEYCODE_KP_MULTIPLY: return HID_KEY_KEYPAD_MULTIPLY;
    case SAPP_KEYCODE_KP_SUBTRACT: return HID_KEY_KEYPAD_SUBTRACT;
    case SAPP_KEYCODE_KP_ADD: return HID_KEY_KEYPAD_ADD;
    case SAPP_KEYCODE_KP_EQUAL: return HID_KEY_KEYPAD_EQUAL;
    case SAPP_KEYCODE_CAPS_LOCK: return 0x39;
    case SAPP_KEYCODE_SCROLL_LOCK: return 0x47;
    case SAPP_KEYCODE_NUM_LOCK: return 0x53;
    case SAPP_KEYCODE_PRINT_SCREEN: return 0x46;
    case SAPP_KEYCODE_PAUSE: return 0x48;
    case SAPP_KEYCODE_MENU: return 0x65;
    case SAPP_KEYCODE_LEFT_CONTROL: return 0xE0;
    case SAPP_KEYCODE_LEFT_SHIFT: return 0xE1;
    case SAPP_KEYCODE_LEFT_ALT: return 0xE2;
    case SAPP_KEYCODE_LEFT_SUPER: return 0xE3;
    case SAPP_KEYCODE_RIGHT_CONTROL: return 0xE4;
    case SAPP_KEYCODE_RIGHT_SHIFT: return 0xE5;
    case SAPP_KEYCODE_RIGHT_ALT: return 0xE6;
    case SAPP_KEYCODE_RIGHT_SUPER: return 0xE7;
    case SAPP_KEYCODE_0: return 0x27;
    case SAPP_KEYCODE_ENTER: return 0x28;
    case SAPP_KEYCODE_KP_ENTER: return 0x58;
    case SAPP_KEYCODE_ESCAPE: return 0x29;
    case SAPP_KEYCODE_BACKSPACE: return 0x2A;
    case SAPP_KEYCODE_TAB: return 0x2B;
    case SAPP_KEYCODE_SPACE: return 0x2C;
    case SAPP_KEYCODE_MINUS: return 0x2D;
    case SAPP_KEYCODE_EQUAL: return 0x2E;
    case SAPP_KEYCODE_LEFT_BRACKET: return 0x2F;
    case SAPP_KEYCODE_RIGHT_BRACKET: return 0x30;
    case SAPP_KEYCODE_BACKSLASH: return 0x31;
    case SAPP_KEYCODE_SEMICOLON: return 0x33;
    case SAPP_KEYCODE_APOSTROPHE: return 0x34;
    case SAPP_KEYCODE_GRAVE_ACCENT: return 0x35;
    case SAPP_KEYCODE_COMMA: return 0x36;
    case SAPP_KEYCODE_PERIOD: return 0x37;
    case SAPP_KEYCODE_SLASH: return 0x38;
    case SAPP_KEYCODE_RIGHT: return 0x4F;
    case SAPP_KEYCODE_LEFT: return 0x50;
    case SAPP_KEYCODE_DOWN: return 0x51;
    case SAPP_KEYCODE_UP: return 0x52;
    case SAPP_KEYCODE_DELETE: return 0x4C;
    case SAPP_KEYCODE_HOME: return 0x4A;
    case SAPP_KEYCODE_END: return 0x4D;
    case SAPP_KEYCODE_INSERT: return 0x49;
    case SAPP_KEYCODE_PAGE_UP: return 0x4B;
    case SAPP_KEYCODE_PAGE_DOWN: return 0x4E;
    default: return 0;
    }
}

/* AltGr arrives as Ctrl+Alt only where the host reports it that way, which is
 * Windows and a browser on a Windows host. On X11 AltGr is Mod5 and on macOS it
 * is Option, so there Ctrl+Alt can only be a held chord and never a composed
 * character. */
#if defined(_WIN32) || defined(__EMSCRIPTEN__)
#define ALTGR_IS_CTRL_ALT 1
#else
#define ALTGR_IS_CTRL_ALT 0
#endif

/* Sokol says which modifiers are held but not which side, so the left one
 * stands for both. */
static uint8_t input_modifier(const sapp_event *e)
{
    return (uint8_t)(((e->modifiers & SAPP_MODIFIER_SHIFT) ? KEYBOARD_MODIFIER_LEFTSHIFT : 0) |
                     ((e->modifiers & SAPP_MODIFIER_ALT) ? KEYBOARD_MODIFIER_LEFTALT : 0) |
                     ((e->modifiers & SAPP_MODIFIER_CTRL) ? KEYBOARD_MODIFIER_LEFTCTRL : 0) |
                     ((e->modifiers & SAPP_MODIFIER_SUPER) ? KEYBOARD_MODIFIER_LEFTGUI : 0));
}

bool input_paste_key(const sapp_event *e)
{
    return e->type == SAPP_EVENTTYPE_KEY_DOWN && !e->key_repeat &&
           key_clip(sokol_to_hid(e->key_code), input_modifier(e)) == KEY_CLIP_PASTE;
}

bool input_key_withheld(const sapp_event *e)
{
    if (e->type != SAPP_EVENTTYPE_KEY_DOWN)
        return false;
    const uint8_t hid = sokol_to_hid(e->key_code);
    const key_clip_t clip = key_clip(hid, input_modifier(e));
    if (clip == KEY_CLIP_PASTE)
        return e->key_repeat;
    return clip != KEY_CLIP_NONE ||
           (hid == HID_KEY_V && input_modifier(e) == KEYBOARD_MODIFIER_LEFTCTRL);
}

/* What the window asks of the OS once it has taken an event: the pointer's
 * capture to take (1) or let go (0), or nothing (-1), and the cursor to show,
 * or -1 for none to set. */
static int input_capture = -1;
static int input_cursor = -1;
static bool input_cursor_shown = true;
static int input_sent_locks = -1;
/* The captured mouse's buttons as the host has them. */
static uint8_t host_mouse_buttons;
/* Whether the host pointer is over the drawn canvas. The tablet's cursor
 * applies only there, and the system cursor shows in the letterbox. True so
 * that a freshly mapped tablet shows its cursor before the pointer has moved
 * once. */
static bool pointer_on_canvas = true;

void input_locks(void)
{
    const uint8_t locks = host_locks();
    if (locks == input_sent_locks)
        return;
    input_sent_locks = locks;
    evq_to_machine(EVQ_LOCKS, &locks, 1);
}

void input_apply(void)
{
    if (input_capture >= 0)
        sapp_lock_mouse(input_capture != 0);
    input_capture = -1;
    if (input_cursor != -1)
    {
        sapp_set_mouse_cursor((sapp_mouse_cursor)input_cursor);
        sapp_show_mouse(input_cursor_shown);
    }
    input_cursor = -1;
}

static void input_set_cursor(sapp_mouse_cursor cursor, bool shown)
{
    input_cursor = cursor;
    input_cursor_shown = shown;
}

bool input_pointer_to_tablet(void)
{
    return input_told.event.flags & EVENT_TO_TABLET;
}

bool input_wheel_to_tablet(void)
{
    return input_told.event.flags & EVENT_WHEEL_TO_TABLET;
}

static bool input_mouse_mapped(void)
{
    return input_told.event.flags & EVENT_MOUSE_MAPPED;
}

static void input_key(const sapp_event *e)
{
    if (e->type == SAPP_EVENTTYPE_CHAR)
    {
        /* Printable characters only. The control codes below 32, DEL, and the
         * Ctrl and Alt chords are all typed from the key going down, and a
         * host that also fires a CHAR for them would type them twice. macOS
         * fires a CHAR for a Cmd chord too, so without the Super test Cmd+V
         * would type a 'v' before the pasted text arrived. Where the host
         * reports AltGr as Ctrl+Alt, the character it composed is typed,
         * which is what the firmware's right-Alt does. */
        if (e->char_code >= 32 && e->char_code != 127 &&
            !(e->modifiers & SAPP_MODIFIER_SUPER) &&
            (!(e->modifiers & (SAPP_MODIFIER_CTRL | SAPP_MODIFIER_ALT)) ||
             (ALTGR_IS_CTRL_ALT &&
              (e->modifiers & SAPP_MODIFIER_CTRL) && (e->modifiers & SAPP_MODIFIER_ALT))))
        {
            const event_char_t c = {.codepoint = e->char_code, .repeat = e->key_repeat};
            evq_to_machine(EVQ_CHAR, &c, sizeof c);
        }
        return;
    }
    const bool ctrl = (e->modifiers & SAPP_MODIFIER_CTRL) != 0;
    const bool alt = (e->modifiers & SAPP_MODIFIER_ALT) != 0;
    event_key_t k = {
        .usage = sokol_to_hid(e->key_code),
        .modifier = input_modifier(e),
        .down = e->type == SAPP_EVENTTYPE_KEY_DOWN,
        .repeat = e->key_repeat,
        /* The system gives no text for a Ctrl or Alt chord, so its character
         * is guessed. Any other key's text arrives as a CHAR. Plain Ctrl+V is
         * the program's, and types 0x16 as Ctrl+C types 0x03: the paste keys
         * are others (core/hid/key.h). Ctrl+Alt is excluded only where it
         * means AltGr, whose composed character arrives as a CHAR. */
        .guess = (ctrl && !alt) || (alt && !(ALTGR_IS_CTRL_ALT && ctrl)),
        .acts = true,
    };
#if defined(__EMSCRIPTEN__)
    /* A page reads the clipboard only in the paste event that a paste key
     * makes, so a paste key is left to the browser (entry.c), and the paste
     * event that follows pastes. */
    if (key_clip(k.usage, k.modifier) == KEY_CLIP_PASTE)
        k.acts = false;
#endif
    evq_to_machine(EVQ_KEY, &k, sizeof k);
}

/* One mouse count is one pixel on a 640-wide canvas and half a pixel on a
 * 320-wide one, so host motion is converted to a fraction of the canvas's
 * on-screen width against this fixed 640. A sweep across the drawn canvas is
 * then 640 counts whatever the canvas resolution and the window size are. */
#define INPUT_MOUSE_REF_WIDTH 640.0f

/* The event's motion in mouse counts, measured against a canvas onscreen_w
 * wide in the motion's own units. */
static void input_mouse_counts(const sapp_event *e, float onscreen_w, int32_t *dx, int32_t *dy)
{
    *dx = *dy = 0;
    if (onscreen_w <= 0.0f)
        return;
    const float gain = INPUT_MOUSE_REF_WIDTH / onscreen_w;
    *dx = (int32_t)lrintf(e->mouse_dx * gain * (float)MOUSE_ONE);
    *dy = (int32_t)lrintf(e->mouse_dy * gain * (float)MOUSE_ONE);
}

static void input_mouse_send(int32_t dx, int32_t dy, bool moved)
{
    const event_mouse_t m = {.dx = dx, .dy = dy, .buttons = host_mouse_buttons, .moved = moved};
    evq_to_machine(EVQ_MOUSE, &m, sizeof m);
}

static void set_host_mouse_button(int btn, bool down)
{
    if (btn < 0 || btn > 2)
        return;
    if (down)
        host_mouse_buttons |= (uint8_t)(1u << btn);
    else
        host_mouse_buttons &= (uint8_t)~(1u << btn);
    input_mouse_send(0, 0, false);
}

static uint8_t host_mouse_button_bit(sapp_mousebutton mb)
{
    return mb == SAPP_MOUSEBUTTON_LEFT     ? TABLET_FLAG_LEFT
           : mb == SAPP_MOUSEBUTTON_RIGHT  ? TABLET_FLAG_RIGHT
           : mb == SAPP_MOUSEBUTTON_MIDDLE ? TABLET_FLAG_MIDDLE
                                           : 0;
}

/* The host's mouse buttons as a tablet bitmap, read from the event's modifiers
 * because a release the window never saw would otherwise leave a button latched
 * down; the web build gets no mouseup outside the canvas. The button this event
 * is about is forced on or off, because some platforms report it in the
 * modifiers a beat late. */
static uint8_t pointer_buttons(const sapp_event *e)
{
    uint8_t b = 0;
    if (e->modifiers & SAPP_MODIFIER_LMB)
        b |= TABLET_FLAG_LEFT;
    if (e->modifiers & SAPP_MODIFIER_RMB)
        b |= TABLET_FLAG_RIGHT;
    if (e->modifiers & SAPP_MODIFIER_MMB)
        b |= TABLET_FLAG_MIDDLE;
    uint8_t bit = host_mouse_button_bit(e->mouse_button);
    if (e->type == SAPP_EVENTTYPE_MOUSE_DOWN)
        b |= bit;
    else if (e->type == SAPP_EVENTTYPE_MOUSE_UP)
        b &= (uint8_t)~bit;
    return b;
}

/* The wheel's notches from a scroll event. A trackpad scrolls by small
 * fractions of a notch, which add up here until they make one, so a slow
 * scroll moves too; the rest waits for the next event. */
static float input_wheel_rest, input_pan_rest;

static int input_notches(float *rest, float delta)
{
    *rest += delta;
    const float whole = truncf(*rest);
    *rest -= whole;
    return (int)whole;
}

static void input_wheel_send(const sapp_event *e, bool tablet)
{
    /* sokol's scroll_x, like GLFW's, is positive to the left, and pan, like
     * HID AC Pan, is positive to the right. */
    const event_wheel_t w = {
        .wheel = (int16_t)input_notches(&input_wheel_rest, e->scroll_y),
        .pan = (int16_t)input_notches(&input_pan_rest, -e->scroll_x),
        .tablet = tablet,
    };
    evq_to_machine(EVQ_WHEEL, &w, sizeof w);
}

/* A host pointer or touch event while the tablet has the pointer, which is an
 * absolute canvas position and never captures. True when the event was
 * the tablet's. */
static bool input_tablet(const sapp_event *e)
{
    int cx, cy;
    switch (e->type)
    {
    case SAPP_EVENTTYPE_MOUSE_DOWN:
    case SAPP_EVENTTYPE_MOUSE_UP:
    case SAPP_EVENTTYPE_MOUSE_MOVE:
    {
        const bool inside = gfx_canvas_from_fb(e->mouse_x, e->mouse_y, &cx, &cy);
        pointer_on_canvas = inside;
        event_pointer_t p = {
            .x = (int16_t)cx,
            .y = (int16_t)cy,
            .buttons = pointer_buttons(e),
            .inside = inside,
            .moved = e->type == SAPP_EVENTTYPE_MOUSE_MOVE,
        };
        if (p.moved)
            input_mouse_counts(e, gfx_canvas_width(), &p.dx, &p.dy);
        evq_to_machine(EVQ_POINTER, &p, sizeof p);
        return true;
    }
    case SAPP_EVENTTYPE_MOUSE_SCROLL:
        /* A wheel the tablet does not use is not the tablet's. It falls to
         * the mouse, which takes it only while captured, so it is either used
         * here or left whole to the page, as the web build cancels it. */
        if (!input_wheel_to_tablet())
            return false;
        input_wheel_send(e, true);
        return true;
    case SAPP_EVENTTYPE_MOUSE_LEAVE:
    {
        pointer_on_canvas = false;
        const event_pointer_t p = {.left = true};
        evq_to_machine(EVQ_POINTER, &p, sizeof p);
        return true;
    }
    case SAPP_EVENTTYPE_TOUCHES_BEGAN:
    case SAPP_EVENTTYPE_TOUCHES_MOVED:
    case SAPP_EVENTTYPE_TOUCHES_ENDED:
    case SAPP_EVENTTYPE_TOUCHES_CANCELLED:
    {
        const bool ending = e->type == SAPP_EVENTTYPE_TOUCHES_ENDED ||
                            e->type == SAPP_EVENTTYPE_TOUCHES_CANCELLED;
        event_touch_t t = {0};
        for (int i = 0; i < e->num_touches && t.n < EVENT_TOUCHES; ++i)
        {
            if (ending && e->touches[i].changed)
                continue; /* the finger this event lifts is no longer a contact */
            if (!gfx_canvas_from_fb(e->touches[i].pos_x, e->touches[i].pos_y, &cx, &cy))
                continue;
            t.at[t.n].x = (int16_t)cx;
            t.at[t.n].y = (int16_t)cy;
            t.n++;
        }
        evq_to_machine(EVQ_TOUCH, &t, sizeof t);
        return true;
    }
    default:
        return false;
    }
}

bool input_mouse_locked(void)
{
#if defined(__EMSCRIPTEN__)
    return sapp_mouse_locked() && entry_canvas_locked();
#else
    return sapp_mouse_locked();
#endif
}

void input_pointer_task(void)
{
    /* The absolute tablet never captures the pointer, and a program that has
     * unmapped the mouse no longer wants it. */
    if (input_mouse_locked() && (!input_mouse_mapped() || input_pointer_to_tablet()))
        input_capture = 0;
}

void input_event(const sapp_event *e)
{
    if (input_pointer_to_tablet() && input_tablet(e))
        return;

    switch (e->type)
    {
    case SAPP_EVENTTYPE_KEY_DOWN:
        /* Esc releases a captured mouse rather than being typed, which is how a
         * browser leaves pointer lock. */
        if (e->key_code == SAPP_KEYCODE_ESCAPE && input_mouse_locked())
        {
            input_capture = 0;
            break;
        }
        input_locks();
        input_key(e);
        break;
    case SAPP_EVENTTYPE_KEY_UP:
    case SAPP_EVENTTYPE_CHAR:
        input_key(e);
        break;
    case SAPP_EVENTTYPE_MOUSE_DOWN:
        if (!input_mouse_locked())
        {
            /* The first click captures the pointer, and only once a program has
             * mapped the mouse. That click is spent on the capture. */
            if (input_mouse_mapped())
                input_capture = 1;
        }
        else
            set_host_mouse_button(e->mouse_button, true);
        break;
    case SAPP_EVENTTYPE_MOUSE_UP:
        if (input_mouse_locked())
            set_host_mouse_button(e->mouse_button, false);
        break;
    case SAPP_EVENTTYPE_MOUSE_MOVE:
        if (input_mouse_locked())
        {
            float onscreen_w = gfx_canvas_width();
#if defined(__APPLE__)
            /* AppKit reports a captured pointer's motion in points, and a point
             * is dpi_scale framebuffer pixels under high_dpi, so the canvas is
             * measured in points too. */
            onscreen_w /= sapp_dpi_scale();
#endif
            int32_t dx, dy;
            input_mouse_counts(e, onscreen_w, &dx, &dy);
            input_mouse_send(dx, dy, true);
        }
        break;
    case SAPP_EVENTTYPE_MOUSE_SCROLL:
        if (input_mouse_locked())
            input_wheel_send(e, false);
        break;
    case SAPP_EVENTTYPE_CLIPBOARD_PASTED:
#if defined(__EMSCRIPTEN__)
        /* The page's paste event, which a paste key left to the browser
         * made, or the browser's own menu. The browser hands the
         * clipboard's text over only here, so it is sent now and pasted
         * in its place among the keys (paste_host.h). Plain Ctrl+V, which
         * typed 0x16, makes none (input_key_withheld). */
        evq_text_to_machine(EVQ_PASTE, sapp_get_clipboard_string());
#endif
        /* On the desktop the paste keys are chords (core/hid/key.h) that
         * paste the desktop's clipboard themselves, asking for it as the
         * paste's turn comes, so the paste sokol makes of Ctrl+V or Cmd+V
         * is no paste. */
        break;
    default:
        break;
    }
}

static sapp_mouse_cursor tablet_cursor_to_sokol(uint8_t shape)
{
    switch (shape)
    {
    case TABLET_CURSOR_ARROW: return SAPP_MOUSECURSOR_ARROW;
    case TABLET_CURSOR_CROSSHAIR: return SAPP_MOUSECURSOR_CROSSHAIR;
    case TABLET_CURSOR_IBEAM: return SAPP_MOUSECURSOR_IBEAM;
    case TABLET_CURSOR_HAND: return SAPP_MOUSECURSOR_POINTING_HAND;
    case TABLET_CURSOR_RESIZE_EW: return SAPP_MOUSECURSOR_RESIZE_EW;
    case TABLET_CURSOR_RESIZE_NS: return SAPP_MOUSECURSOR_RESIZE_NS;
    case TABLET_CURSOR_RESIZE_NWSE: return SAPP_MOUSECURSOR_RESIZE_NWSE;
    case TABLET_CURSOR_RESIZE_NESW: return SAPP_MOUSECURSOR_RESIZE_NESW;
    case TABLET_CURSOR_MOVE: return SAPP_MOUSECURSOR_RESIZE_ALL;
    case TABLET_CURSOR_NOT_ALLOWED: return SAPP_MOUSECURSOR_NOT_ALLOWED;
    default: return SAPP_MOUSECURSOR_DEFAULT;
    }
}

static bool had_tablet;

void input_cursor_task(void)
{
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active() && dbgui_wants_mouse())
    {
        /* Over a debugger panel ImGui chooses the shape, and the pointer stays
         * visible even where a program asked for it to be hidden. */
        input_set_cursor((sapp_mouse_cursor)dbgui_mouse_cursor(), true);
        return;
    }
#endif
    if (input_pointer_to_tablet() && pointer_on_canvas)
    {
        had_tablet = true;
        if (input_told.event.cursor == TABLET_CURSOR_OFF)
            input_set_cursor(SAPP_MOUSECURSOR_DEFAULT, false);
        else
            input_set_cursor(tablet_cursor_to_sokol(input_told.event.cursor), true);
    }
    else if (had_tablet)
    {
        had_tablet = false;
        input_set_cursor(SAPP_MOUSECURSOR_DEFAULT, true);
    }
}

void input_reset(void)
{
    memset(&input_told, 0, sizeof input_told);
    input_capture = input_cursor = input_sent_locks = -1;
    input_cursor_shown = pointer_on_canvas = true;
    had_tablet = false;
    host_mouse_buttons = 0;
    input_wheel_rest = input_pan_rest = 0;
}
