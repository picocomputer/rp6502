/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "host/sokol/app/input.h"

#include "host/sokol/app/gfx.h"
#include "core/hid/keyboard.h"
#include "core/hid/usage.h"
#include "core/hid/vtkeys.h"
#include "core/hid/mouse.h"
#include "core/hid/tablet.h"
#include "core/vga/vga_emu.h"
#ifdef EMU_WITH_DEBUGGER
#include "host/sokol/dbg/dbgui.h"
#include "core/dap/dbg.h"
#endif
#include "sokol/sokol_app.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

static bool suppress_char;

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
    case SAPP_KEYCODE_KP_DIVIDE: return 0x54;
    case SAPP_KEYCODE_KP_MULTIPLY: return 0x55;
    case SAPP_KEYCODE_KP_SUBTRACT: return 0x56;
    case SAPP_KEYCODE_KP_ADD: return 0x57;
    case SAPP_KEYCODE_KP_EQUAL: return 0x67;
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

/* The US-ASCII character of a printable sokol keycode, honoring shift, else 0.
 * The CHAR case below drops Ctrl and Alt chords, except where Ctrl+Alt means
 * AltGr, so the character such a chord carries is reconstructed from the
 * keycode here. It is a US-layout approximation and not a match for the code
 * page in force. */
static char ascii_from_key(int kc, bool shift)
{
    if (kc >= SAPP_KEYCODE_A && kc <= SAPP_KEYCODE_Z)
        return (char)(shift ? 'A' + (kc - SAPP_KEYCODE_A) : 'a' + (kc - SAPP_KEYCODE_A));
    if (kc >= SAPP_KEYCODE_0 && kc <= SAPP_KEYCODE_9)
    {
        static const char shifted[] = ")!@#$%^&*(";
        return shift ? shifted[kc - SAPP_KEYCODE_0] : (char)('0' + (kc - SAPP_KEYCODE_0));
    }
    switch (kc)
    {
    case SAPP_KEYCODE_KP_DIVIDE: return '/';
    case SAPP_KEYCODE_KP_MULTIPLY: return '*';
    case SAPP_KEYCODE_KP_SUBTRACT: return '-';
    case SAPP_KEYCODE_KP_ADD: return '+';
    case SAPP_KEYCODE_KP_EQUAL: return '=';
    case SAPP_KEYCODE_SPACE: return ' ';
    case SAPP_KEYCODE_MINUS: return shift ? '_' : '-';
    case SAPP_KEYCODE_EQUAL: return shift ? '+' : '=';
    case SAPP_KEYCODE_LEFT_BRACKET: return shift ? '{' : '[';
    case SAPP_KEYCODE_RIGHT_BRACKET: return shift ? '}' : ']';
    case SAPP_KEYCODE_BACKSLASH: return shift ? '|' : '\\';
    case SAPP_KEYCODE_SEMICOLON: return shift ? ':' : ';';
    case SAPP_KEYCODE_APOSTROPHE: return shift ? '"' : '\'';
    case SAPP_KEYCODE_GRAVE_ACCENT: return shift ? '~' : '`';
    case SAPP_KEYCODE_COMMA: return shift ? '<' : ',';
    case SAPP_KEYCODE_PERIOD: return shift ? '>' : '.';
    case SAPP_KEYCODE_SLASH: return shift ? '?' : '/';
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

static void input_key(const sapp_event *e)
{
    if (e->type == SAPP_EVENTTYPE_KEY_DOWN || e->type == SAPP_EVENTTYPE_KEY_UP)
    {
        uint8_t hid = sokol_to_hid(e->key_code);
        if (hid)
            keyboard_hid_set(hid, e->type == SAPP_EVENTTYPE_KEY_DOWN);
    }
    switch (e->type)
    {
    case SAPP_EVENTTYPE_CHAR:
        /* Printable characters only. The control codes below 32, DEL, and the
         * Ctrl and Alt chords are all emitted by the KEY_DOWN case below, and a
         * host that also fires a CHAR for them would double-inject. macOS fires
         * a CHAR for a Cmd chord too, so without the Super test Cmd+V would type
         * a 'v' before the pasted text arrived. Where the host reports AltGr as
         * Ctrl+Alt, the character it composed is typed, which is what the
         * firmware's right-Alt does. */
        if (suppress_char)
            suppress_char = false;
        else if (e->char_code >= 32 && e->char_code != 127 &&
                 !(e->modifiers & SAPP_MODIFIER_SUPER) &&
                 (!(e->modifiers & (SAPP_MODIFIER_CTRL | SAPP_MODIFIER_ALT)) ||
                  (ALTGR_IS_CTRL_ALT &&
                   (e->modifiers & SAPP_MODIFIER_CTRL) && (e->modifiers & SAPP_MODIFIER_ALT))))
        {
            vtkeys_char(e->char_code);
        }
        break;
    case SAPP_EVENTTYPE_KEY_DOWN:
    {
        bool ctrl = (e->modifiers & SAPP_MODIFIER_CTRL) != 0;
        bool shift = (e->modifiers & SAPP_MODIFIER_SHIFT) != 0;
        bool alt = (e->modifiers & SAPP_MODIFIER_ALT) != 0;
        uint8_t hid = sokol_to_hid(e->key_code);
        suppress_char = false;
        switch (e->key_code)
        {
        case SAPP_KEYCODE_NUM_LOCK: keyboard_toggle_lock(KEYBOARD_LED_NUMLOCK); break;
        case SAPP_KEYCODE_CAPS_LOCK: keyboard_toggle_lock(KEYBOARD_LED_CAPSLOCK); break;
        case SAPP_KEYCODE_SCROLL_LOCK: keyboard_toggle_lock(KEYBOARD_LED_SCROLLLOCK); break;
        /* Sokol reports no NumLock modifier, so the keypad always navigates and
         * always swallows the digit CHAR a host emits when NumLock is on. KP5
         * navigates nowhere and only swallows. */
        case SAPP_KEYCODE_KP_1:
        case SAPP_KEYCODE_KP_2:
        case SAPP_KEYCODE_KP_3:
        case SAPP_KEYCODE_KP_4:
        case SAPP_KEYCODE_KP_5:
        case SAPP_KEYCODE_KP_6:
        case SAPP_KEYCODE_KP_7:
        case SAPP_KEYCODE_KP_8:
        case SAPP_KEYCODE_KP_9:
        case SAPP_KEYCODE_KP_0:
        case SAPP_KEYCODE_KP_DECIMAL:
            suppress_char = true;
            vtkeys_key(keyboard_keypad_nav(hid), ctrl, shift, alt);
            break;
        default:
            if (vtkeys_key(hid, ctrl, shift, alt))
                break;
            if (ctrl && !alt)
            {
                char ch = ascii_from_key(e->key_code, shift);
#if defined(__EMSCRIPTEN__)
                /* The browser, not sokol, decides when a paste fires, and any
                 * Ctrl+V variant can land a JavaScript paste event, so every
                 * Ctrl+V chord types the pasted text instead of 0x16. */
                if (e->key_code == SAPP_KEYCODE_V &&
                    sapp_query_desc().enable_clipboard)
                    ch = 0;
#elif !defined(__APPLE__)
                /* Sokol sends CLIPBOARD_PASTED when the modifiers are exactly
                 * Ctrl and the key is V, so that one chord types the pasted text
                 * instead of 0x16. Shifted variants never paste and still inject
                 * 0x16, and macOS pastes on Cmd+V, which leaves its Ctrl+V a
                 * guest SYN. */
                if (e->key_code == SAPP_KEYCODE_V &&
                    e->modifiers == SAPP_MODIFIER_CTRL &&
                    sapp_query_desc().enable_clipboard)
                    ch = 0;
#endif
                vtkeys_ctrl_letter(ch);
            }
            /* Alt and a printable key is the Meta form, ESC then the character.
             * Ctrl+Alt is excluded only where it means AltGr, whose composed
             * character arrives through the CHAR case above. */
            else if (alt && !(ALTGR_IS_CTRL_ALT && ctrl))
                vtkeys_alt_char(ascii_from_key(e->key_code, shift), ctrl);
            break;
        }
        break;
    }
    default:
        break;
    }
}

/* One mouse count is one pixel on a 640-wide canvas and half a pixel on a
 * 320-wide one, so host motion is converted to a fraction of the canvas's
 * on-screen width against this fixed 640. A sweep across the drawn canvas is
 * then 640 counts whatever the canvas resolution and the window size are. */
#define INPUT_MOUSE_REF_WIDTH 640.0f

static uint8_t host_mouse_buttons;

static void set_host_mouse_button(int btn, bool down)
{
    if (btn < 0 || btn > 2)
        return;
    if (down)
        host_mouse_buttons |= (uint8_t)(1u << btn);
    else
        host_mouse_buttons &= (uint8_t)~(1u << btn);
    mouse_host_buttons(host_mouse_buttons);
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

/* Route a host pointer or touch event to the tablet, which is an absolute canvas
 * position and never captures. Because nothing is captured while a tablet is
 * mapped, a mouse the program also mapped is fed from here as well: the one
 * physical pointer drives both blocks, as it would on hardware. True when the
 * event was consumed. */
static bool input_tablet(const sapp_event *e)
{
    int cx, cy;
    switch (e->type)
    {
    case SAPP_EVENTTYPE_MOUSE_DOWN:
    case SAPP_EVENTTYPE_MOUSE_UP:
    case SAPP_EVENTTYPE_MOUSE_MOVE:
    {
        bool inside = gfx_canvas_from_fb(e->mouse_x, e->mouse_y, &cx, &cy);
        input_set_pointer_on_canvas(inside);
        uint8_t buttons = pointer_buttons(e);
        if (inside)
            tablet_host_pointer(cx, cy, buttons, true);
        else
            tablet_host_clear();
        if (mouse_is_mapped())
        {
            mouse_host_buttons(buttons);
            if (e->type == SAPP_EVENTTYPE_MOUSE_MOVE)
            {
                int cw, ch;
                vga_canvas_size(&cw, &ch);
                float onscreen_w = (float)cw * gfx_canvas_scale();
                if (onscreen_w > 0.0f)
                {
                    float gain = INPUT_MOUSE_REF_WIDTH / onscreen_w;
                    mouse_host_move((int32_t)lrintf(e->mouse_dx * gain * (float)MOUSE_ONE),
                                (int32_t)lrintf(e->mouse_dy * gain * (float)MOUSE_ONE));
                }
            }
        }
        return true;
    }
    case SAPP_EVENTTYPE_MOUSE_SCROLL:
        tablet_host_wheel((int)lroundf(e->scroll_y), (int)lroundf(e->scroll_x));
        if (mouse_is_mapped())
            mouse_host_wheel((int)lroundf(e->scroll_y), (int)lroundf(e->scroll_x));
        return true;
    case SAPP_EVENTTYPE_MOUSE_LEAVE:
        input_set_pointer_on_canvas(false);
        tablet_host_clear();
        return true;
    case SAPP_EVENTTYPE_TOUCHES_BEGAN:
    case SAPP_EVENTTYPE_TOUCHES_MOVED:
    case SAPP_EVENTTYPE_TOUCHES_ENDED:
    case SAPP_EVENTTYPE_TOUCHES_CANCELLED:
    {
        bool ending = e->type == SAPP_EVENTTYPE_TOUCHES_ENDED ||
                      e->type == SAPP_EVENTTYPE_TOUCHES_CANCELLED;
        tablet_point_t pts[SAPP_MAX_TOUCHPOINTS];
        int n = 0;
        for (int i = 0; i < e->num_touches && n < SAPP_MAX_TOUCHPOINTS; ++i)
        {
            if (ending && e->touches[i].changed)
                continue; /* the finger this event lifts is no longer a contact */
            if (!gfx_canvas_from_fb(e->touches[i].pos_x, e->touches[i].pos_y, &cx, &cy))
                continue;
            pts[n].x = (int16_t)cx;
            pts[n].y = (int16_t)cy;
            n++;
        }
        tablet_host_touch(pts, n);
        return true;
    }
    default:
        return false;
    }
}

void input_event(const sapp_event *e)
{
    if (tablet_is_mapped() && input_tablet(e))
        return;

    switch (e->type)
    {
    case SAPP_EVENTTYPE_KEY_DOWN:
        /* Esc releases a captured mouse rather than being typed, which is how a
         * browser leaves pointer lock. */
        if (e->key_code == SAPP_KEYCODE_ESCAPE && sapp_mouse_locked())
        {
            sapp_lock_mouse(false);
            break;
        }
        input_key(e);
        break;
    case SAPP_EVENTTYPE_KEY_UP:
    case SAPP_EVENTTYPE_CHAR:
        input_key(e);
        break;
    case SAPP_EVENTTYPE_MOUSE_DOWN:
        if (!sapp_mouse_locked())
        {
            /* The first click captures the pointer, and only once a program has
             * mapped the mouse. That click is spent on the capture. */
            if (mouse_is_mapped())
                sapp_lock_mouse(true);
        }
        else
            set_host_mouse_button(e->mouse_button, true);
        break;
    case SAPP_EVENTTYPE_MOUSE_UP:
        if (sapp_mouse_locked())
            set_host_mouse_button(e->mouse_button, false);
        break;
    case SAPP_EVENTTYPE_MOUSE_MOVE:
        if (sapp_mouse_locked())
        {
            int cw, ch;
            vga_canvas_size(&cw, &ch);
            float onscreen_w = (float)cw * gfx_canvas_scale();
            if (onscreen_w > 0.0f)
            {
                float gain = INPUT_MOUSE_REF_WIDTH / onscreen_w;
                mouse_host_move((int32_t)lrintf(e->mouse_dx * gain * (float)MOUSE_ONE),
                                (int32_t)lrintf(e->mouse_dy * gain * (float)MOUSE_ONE));
            }
        }
        break;
    case SAPP_EVENTTYPE_MOUSE_SCROLL:
        if (sapp_mouse_locked())
            mouse_host_wheel((int)lroundf(e->scroll_y), (int)lroundf(e->scroll_x));
        break;
    case SAPP_EVENTTYPE_CLIPBOARD_PASTED:
        vtkeys_paste(sapp_get_clipboard_string());
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
    default: return SAPP_MOUSECURSOR_DEFAULT;
    }
}

/* True so that a freshly mapped tablet shows its cursor before the pointer has
 * moved once. */
static bool pointer_on_canvas = true;

void input_set_pointer_on_canvas(bool on)
{
    pointer_on_canvas = on;
}

void input_update_cursor(void)
{
    static bool had_tablet;
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active() && dbgui_wants_mouse())
    {
        /* Over a debugger panel ImGui chooses the shape, and the pointer stays
         * visible even where a program asked for it to be hidden. */
        sapp_set_mouse_cursor((sapp_mouse_cursor)dbgui_mouse_cursor());
        sapp_show_mouse(true);
        return;
    }
#endif
    if (tablet_is_mapped() && pointer_on_canvas)
    {
        had_tablet = true;
        int shape = tablet_control();
        if (shape >= TABLET_CURSOR_COUNT)
            shape = TABLET_CURSOR_OFF;
        if (shape == TABLET_CURSOR_OFF)
            sapp_show_mouse(false);
        else
        {
            sapp_set_mouse_cursor(tablet_cursor_to_sokol(shape));
            sapp_show_mouse(true);
        }
    }
    else if (had_tablet)
    {
        had_tablet = false;
        sapp_set_mouse_cursor(SAPP_MOUSECURSOR_DEFAULT);
        sapp_show_mouse(true);
    }
}
