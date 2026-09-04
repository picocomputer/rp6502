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

/* ------------------------------------------------------------------ */
/* Host (sokol) key/char translation                                   */
/* ------------------------------------------------------------------ */

static bool suppress_char; /* swallow the CHAR a numpad KEY_DOWN would double-inject */

/* Map a sokol keycode to a USB HID keyboard usage id for the xreg keyboard
 * bitmap. 0 = unmapped (not reported). */
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

/* US-ASCII of a printable sokol keycode honoring shift, else 0. Alt combos fire
 * no CHAR event, so an Alt+key Meta escape is reconstructed here — a US-layout
 * approximation, not an OEM-codepage match. */
static char ascii_from_key(int kc, bool shift)
{
    if (kc >= SAPP_KEYCODE_A && kc <= SAPP_KEYCODE_Z)
        return (char)(shift ? 'A' + (kc - SAPP_KEYCODE_A) : 'a' + (kc - SAPP_KEYCODE_A));
    if (kc >= SAPP_KEYCODE_0 && kc <= SAPP_KEYCODE_9)
    {
        static const char shifted[] = ")!@#$%^&*(";
        return shift ? shifted[kc - SAPP_KEYCODE_0] : (char)('0' + (kc - SAPP_KEYCODE_0));
    }
    /* The keypad prints its digit when NumLock is on, and an Alt chord over
     * it fires no CHAR event to read one from. libretro answered this and
     * this side had not. */
    if (kc >= SAPP_KEYCODE_KP_0 && kc <= SAPP_KEYCODE_KP_9)
        return (char)('0' + (kc - SAPP_KEYCODE_KP_0));
    switch (kc)
    {
    case SAPP_KEYCODE_KP_DECIMAL: return '.';
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

/* AltGr arrives as Ctrl+Alt only where the host reports it that way: Windows
 * natively, and browsers on a Windows host. On X11 AltGr is Mod5 and on macOS
 * plain Option — there Ctrl+Alt can only be a held chord, never composition. */
#if defined(_WIN32) || defined(__EMSCRIPTEN__)
#define ALTGR_IS_CTRL_ALT 1
#else
#define ALTGR_IS_CTRL_ALT 0
#endif

/* Feed one key/char event to the emulated keyboard: the HID bitmap on
 * press/release, printable CHARs as OEM bytes, and the navigation/function/ctrl
 * keys as their byte sequences. (Esc-releases-mouse is a capture concern
 * input_event handles before forwarding here.) */
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
        /* Printable input only; control codes (<32) and DEL arrive via KEY_DOWN
         * below, so skip them here to avoid double injection. Ctrl/Alt chords are
         * likewise emitted by KEY_DOWN (as C0 / ESC-prefixed bytes); X11 still fires
         * a CHAR for them, so drop those here or the plain char double-injects.
         * Super too: macOS delivers a printable CHAR for Cmd chords (Cmd+V would
         * type 'v' before the CLIPBOARD_PASTED text lands). Where the host
         * reports AltGr as Ctrl+Alt, that composed char types — matching the
         * firmware's right-Alt level-3. */
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
        /* NumLock-off numpad navigation. sokol reports no NumLock modifier, so
         * always nav and swallow the digit CHAR the host emits when NumLock is
         * on. KP5 navigates nowhere, so it only swallows. */
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
            /* A key that sends a sequence of its own -- Enter, Tab, an arrow,
             * a function key -- takes it; the rest fall through to the chords. */
            if (vtkeys_key(hid, ctrl, shift, alt))
                break;
            /* Ctrl+<key> -> C0 control byte (Ctrl-C latches SIGINT). Cover the full
             * @.._ / `..~ range the firmware promotes (Ctrl+[ = ESC, Ctrl+\ = FS,
             * Ctrl+] = GS, Ctrl+^, Ctrl+_), not just letters; vtkeys_ctrl_letter gates
             * the valid range. The CHAR case above drops the X11 duplicate. */
            if (ctrl && !alt)
            {
                char ch = ascii_from_key(e->key_code, shift);
#if defined(__EMSCRIPTEN__)
                /* The browser, not sokol, decides when a paste fires (any
                 * Ctrl+V variant can land a JS paste event), so every Ctrl+V
                 * chord types the paste instead of 0x16. */
                if (e->key_code == SAPP_KEYCODE_V &&
                    sapp_query_desc().enable_clipboard)
                    ch = 0;
#elif !defined(__APPLE__)
                /* Unshifted Ctrl+V — the exact chord sokol pastes on — types the
                 * CLIPBOARD_PASTED text instead of 0x16. Shifted variants still
                 * inject 0x16 (they never paste), and macOS pastes on Cmd+V, so
                 * its Ctrl+V stays a guest SYN. */
                if (e->key_code == SAPP_KEYCODE_V &&
                    e->modifiers == SAPP_MODIFIER_CTRL &&
                    sapp_query_desc().enable_clipboard)
                    ch = 0;
#endif
                vtkeys_ctrl_letter(ch);
            }
            /* Alt+<printable> -> ESC<char> (Meta). No CHAR fires for Alt combos.
             * Ctrl+Alt is excluded only where it means AltGr, whose composed char
             * arrives via the CHAR case above. */
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

/* ------------------------------------------------------------------ */
/* Mouse                                                               */
/* ------------------------------------------------------------------ */

/* Mouse sensitivity: the ROM always works in 640px-wide mouse units and halves
 * them itself for a 320px canvas, so convert host motion to a fraction of the
 * canvas's on-screen width scaled to a fixed 640 — a full-width sweep is 640
 * counts regardless of the canvas resolution. */
#define INPUT_MOUSE_REF_WIDTH 640.0f

static uint8_t host_mouse_buttons; /* host mouse button bitmap while captured */

/* Set or clear a captured mouse button (0..2 = left/right/middle) and publish. */
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

/* ------------------------------------------------------------------ */
/* Tablet (absolute pointer / touch)                                   */
/* ------------------------------------------------------------------ */

/* Left/right/middle bit (TABLET_FLAG_*) for a sokol mouse button. */
static uint8_t host_mouse_button_bit(sapp_mousebutton mb)
{
    return mb == SAPP_MOUSEBUTTON_LEFT     ? TABLET_FLAG_LEFT
           : mb == SAPP_MOUSEBUTTON_RIGHT  ? TABLET_FLAG_RIGHT
           : mb == SAPP_MOUSEBUTTON_MIDDLE ? TABLET_FLAG_MIDDLE
                                           : 0;
}

/* Host mouse buttons as a tablet/mouse bitmap (bit 0 left, 1 right, 2 middle).
 * Taken from the event modifiers so a release missed off-window (e.g. the web
 * build gets no mouseup outside the canvas) can't latch a stale button; the
 * changing button is forced on/off since some platforms report it a beat late. */
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

/* Route a host pointer/touch event to the tablet device (absolute canvas
 * position, no capture). Because the pointer is never captured while a tablet is
 * mapped, a mouse the program also mapped is fed here too: the one physical
 * pointer drives both blocks like hardware, and the ROM reads the mouse block
 * whenever every tablet contact flag is 0. Returns true when it consumed e. */
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
        input_set_pointer_on_canvas(inside); /* the tablet owns the cursor only on-canvas */
        uint8_t buttons = pointer_buttons(e);
        if (inside)
            tablet_host_pointer(cx, cy, buttons);
        else
            tablet_host_clear(); /* outside the canvas: no contact, all buttons released */
        if (mouse_is_mapped()) /* the same physical pointer also drives the mouse block */
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
                    mouse_host_move(e->mouse_dx * gain, e->mouse_dy * gain);
                }
            }
        }
        return true;
    }
    case SAPP_EVENTTYPE_MOUSE_SCROLL:
        tablet_host_wheel((int)lroundf(e->scroll_y), (int)lroundf(e->scroll_x));
        if (mouse_is_mapped()) /* the same scroll also drives the mouse block */
            mouse_host_wheel((int)lroundf(e->scroll_y), (int)lroundf(e->scroll_x));
        return true;
    case SAPP_EVENTTYPE_MOUSE_LEAVE:
        input_set_pointer_on_canvas(false); /* hand the cursor back to the system */
        tablet_host_clear();                    /* pointer left the window */
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
                continue; /* the finger lifting this event is no longer a contact */
            if (!gfx_canvas_from_fb(e->touches[i].pos_x, e->touches[i].pos_y, &cx, &cy))
                continue; /* touch in the letterbox: not a canvas contact */
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
    /* An absolute-pointer program takes host pointer/touch events directly (no
     * capture); input_tablet consumes those and returns true. Everything else
     * (keys, and pointer events when no tablet is mapped) falls through below. */
    if (tablet_is_mapped() && input_tablet(e))
        return;

    switch (e->type)
    {
    case SAPP_EVENTTYPE_KEY_DOWN:
        /* Esc releases a captured mouse (a capture concern) instead of being
         * typed; every other key/char is translated below. */
        if (e->key_code == SAPP_KEYCODE_ESCAPE && sapp_mouse_locked())
        {
            sapp_lock_mouse(false); /* matches the browser's pointer-lock exit */
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
            /* First click captures the mouse (only once a program wants it);
             * the click itself is consumed by the capture. */
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
            float onscreen_w = (float)cw * gfx_canvas_scale(); /* drawn canvas width, fb px */
            if (onscreen_w > 0.0f)
            {
                float gain = INPUT_MOUSE_REF_WIDTH / onscreen_w; /* counts per fb pixel */
                mouse_host_move(e->mouse_dx * gain, e->mouse_dy * gain);
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

/* Map the ROM's tablet control byte to a sokol system cursor. */
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

/* Whether the host pointer is over the drawn canvas, set by the input layer. The
 * tablet only owns the host cursor while true; in the letterbox (or a debugger
 * panel, handled below) the system cursor shows. Defaults true so a freshly
 * mapped tablet shows its cursor before the first motion. */
static bool pointer_on_canvas = true;

void input_set_pointer_on_canvas(bool on)
{
    pointer_on_canvas = on;
}

/* Apply the tablet ROM's requested host cursor (control byte): TABLET_CURSOR_OFF
 * hides it (the ROM draws its own), otherwise show that shape. This is the sole
 * cursor writer (simgui's own control is disabled), run every frame so a ROM
 * cursor change or a debugger panel-hover change is reflected promptly. Over a
 * debugger panel ImGui owns the shape, applied via dbgui_mouse_cursor. */
void input_update_cursor(void)
{
    static bool had_tablet;
#ifdef EMU_WITH_DEBUGGER
    if (dbg_is_active() && dbgui_wants_mouse())
    {
        /* Over a debugger panel ImGui owns the shape; apply it (simgui no longer
         * does) and keep the pointer visible over any TABLET_CURSOR_OFF hide. */
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
