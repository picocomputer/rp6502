/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "emu/app/input.h"

#include "emu/app/window.h"
#include "emu/hid/kbd.h"
#include "emu/hid/mou.h"
#include "emu/hid/tab.h"
#include "emu/sys/com.h"
#include "emu/sys/vga.h"
#include "sokol/sokol_app.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

/* Encode one Unicode codepoint to a NUL-terminated UTF-8 string (kbd_text then
 * maps it to the active OEM code page). */
static const char *utf8_encode(uint32_t cp, char dst[5])
{
    int n = 0;
    if (cp < 0x80)
        dst[n++] = (char)cp;
    else if (cp < 0x800)
    {
        dst[n++] = (char)(0xC0 | (cp >> 6));
        dst[n++] = (char)(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        dst[n++] = (char)(0xE0 | (cp >> 12));
        dst[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[n++] = (char)(0x80 | (cp & 0x3F));
    }
    else
    {
        dst[n++] = (char)(0xF0 | (cp >> 18));
        dst[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        dst[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[n++] = (char)(0x80 | (cp & 0x3F));
    }
    dst[n] = 0;
    return dst;
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
    switch (kc)
    {
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

/* C0 promotion of a printable byte, mirroring the firmware kbd_ctrl_promote. */
static char ctrl_promote(char ch)
{
    if (ch >= '`' && ch <= '~')
        return (char)(ch - 96);
    if (ch >= '@' && ch <= '_')
        return (char)(ch - 64);
    return 0;
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
            kbd_hid_set(hid, e->type == SAPP_EVENTTYPE_KEY_DOWN);
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
            char u[5];
            kbd_text(utf8_encode(e->char_code, u));
        }
        break;
    case SAPP_EVENTTYPE_KEY_DOWN:
    {
        bool ctrl = (e->modifiers & SAPP_MODIFIER_CTRL) != 0;
        bool shift = (e->modifiers & SAPP_MODIFIER_SHIFT) != 0;
        bool alt = (e->modifiers & SAPP_MODIFIER_ALT) != 0;
        suppress_char = false;
        switch (e->key_code)
        {
        case SAPP_KEYCODE_ESCAPE: kbd_key(KBD_KEY_ESCAPE, ctrl, shift, alt); break;
        case SAPP_KEYCODE_ENTER:
        case SAPP_KEYCODE_KP_ENTER: kbd_key(KBD_KEY_ENTER, ctrl, shift, alt); break;
        case SAPP_KEYCODE_BACKSPACE: kbd_key(KBD_KEY_BACKSPACE, ctrl, shift, alt); break;
        case SAPP_KEYCODE_TAB: kbd_key(KBD_KEY_TAB, ctrl, shift, alt); break;
        case SAPP_KEYCODE_UP: kbd_key(KBD_KEY_UP, ctrl, shift, alt); break;
        case SAPP_KEYCODE_DOWN: kbd_key(KBD_KEY_DOWN, ctrl, shift, alt); break;
        case SAPP_KEYCODE_LEFT: kbd_key(KBD_KEY_LEFT, ctrl, shift, alt); break;
        case SAPP_KEYCODE_RIGHT: kbd_key(KBD_KEY_RIGHT, ctrl, shift, alt); break;
        case SAPP_KEYCODE_HOME: kbd_key(KBD_KEY_HOME, ctrl, shift, alt); break;
        case SAPP_KEYCODE_END: kbd_key(KBD_KEY_END, ctrl, shift, alt); break;
        case SAPP_KEYCODE_DELETE: kbd_key(KBD_KEY_DELETE, ctrl, shift, alt); break;
        case SAPP_KEYCODE_INSERT: kbd_key(KBD_KEY_INSERT, ctrl, shift, alt); break;
        case SAPP_KEYCODE_PAGE_UP: kbd_key(KBD_KEY_PAGE_UP, ctrl, shift, alt); break;
        case SAPP_KEYCODE_PAGE_DOWN: kbd_key(KBD_KEY_PAGE_DOWN, ctrl, shift, alt); break;
        case SAPP_KEYCODE_F1: kbd_key(KBD_KEY_F1, ctrl, shift, alt); break;
        case SAPP_KEYCODE_F2: kbd_key(KBD_KEY_F2, ctrl, shift, alt); break;
        case SAPP_KEYCODE_F3: kbd_key(KBD_KEY_F3, ctrl, shift, alt); break;
        case SAPP_KEYCODE_F4: kbd_key(KBD_KEY_F4, ctrl, shift, alt); break;
        case SAPP_KEYCODE_F5: kbd_key(KBD_KEY_F5, ctrl, shift, alt); break;
        case SAPP_KEYCODE_F6: kbd_key(KBD_KEY_F6, ctrl, shift, alt); break;
        case SAPP_KEYCODE_F7: kbd_key(KBD_KEY_F7, ctrl, shift, alt); break;
        case SAPP_KEYCODE_F8: kbd_key(KBD_KEY_F8, ctrl, shift, alt); break;
        case SAPP_KEYCODE_F9: kbd_key(KBD_KEY_F9, ctrl, shift, alt); break;
        case SAPP_KEYCODE_F10: kbd_key(KBD_KEY_F10, ctrl, shift, alt); break;
        case SAPP_KEYCODE_F11: kbd_key(KBD_KEY_F11, ctrl, shift, alt); break;
        case SAPP_KEYCODE_F12: kbd_key(KBD_KEY_F12, ctrl, shift, alt); break;
        case SAPP_KEYCODE_NUM_LOCK: kbd_toggle_lock(1); break;
        case SAPP_KEYCODE_CAPS_LOCK: kbd_toggle_lock(2); break;
        case SAPP_KEYCODE_SCROLL_LOCK: kbd_toggle_lock(4); break;
        /* NumLock-off numpad navigation. sokol reports no NumLock modifier, so
         * always nav and swallow the digit CHAR the host emits when NumLock is on. */
        case SAPP_KEYCODE_KP_1: suppress_char = true; kbd_key(KBD_KEY_END, ctrl, shift, alt); break;
        case SAPP_KEYCODE_KP_2: suppress_char = true; kbd_key(KBD_KEY_DOWN, ctrl, shift, alt); break;
        case SAPP_KEYCODE_KP_3: suppress_char = true; kbd_key(KBD_KEY_PAGE_DOWN, ctrl, shift, alt); break;
        case SAPP_KEYCODE_KP_4: suppress_char = true; kbd_key(KBD_KEY_LEFT, ctrl, shift, alt); break;
        case SAPP_KEYCODE_KP_5: suppress_char = true; break;
        case SAPP_KEYCODE_KP_6: suppress_char = true; kbd_key(KBD_KEY_RIGHT, ctrl, shift, alt); break;
        case SAPP_KEYCODE_KP_7: suppress_char = true; kbd_key(KBD_KEY_HOME, ctrl, shift, alt); break;
        case SAPP_KEYCODE_KP_8: suppress_char = true; kbd_key(KBD_KEY_UP, ctrl, shift, alt); break;
        case SAPP_KEYCODE_KP_9: suppress_char = true; kbd_key(KBD_KEY_PAGE_UP, ctrl, shift, alt); break;
        case SAPP_KEYCODE_KP_0: suppress_char = true; kbd_key(KBD_KEY_INSERT, ctrl, shift, alt); break;
        case SAPP_KEYCODE_KP_DECIMAL: suppress_char = true; kbd_key(KBD_KEY_DELETE, ctrl, shift, alt); break;
        default:
            /* Ctrl+<key> -> C0 control byte (Ctrl-C latches SIGINT). Cover the full
             * @.._ / `..~ range the firmware promotes (Ctrl+[ = ESC, Ctrl+\ = FS,
             * Ctrl+] = GS, Ctrl+^, Ctrl+_), not just letters; ctrl_promote gates
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
                if (ch && ctrl_promote(ch))
                    kbd_ctrl_letter(ch);
            }
            /* Alt+<printable> -> ESC<char> (Meta), ctrl-promoting first when both
             * held, mirroring the firmware order. No CHAR fires for Alt combos.
             * Ctrl+Alt is excluded only where it means AltGr, whose composed char
             * arrives via the CHAR case above. */
            else if (alt && !(ALTGR_IS_CTRL_ALT && ctrl))
            {
                char ch = ascii_from_key(e->key_code, shift);
                if (ch)
                {
                    if (ctrl)
                    {
                        char c = ctrl_promote(ch);
                        if (c)
                            ch = c;
                    }
                    com_kbd_push_byte(0x1b);
                    com_kbd_push_byte((uint8_t)ch);
                }
            }
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

static uint8_t mouse_buttons; /* host mouse button bitmap while captured */

/* Set or clear a captured mouse button (0..2 = left/right/middle) and publish. */
static void set_mouse_button(int btn, bool down)
{
    if (btn < 0 || btn > 2)
        return;
    if (down)
        mouse_buttons |= (uint8_t)(1u << btn);
    else
        mouse_buttons &= (uint8_t)~(1u << btn);
    mou_host_buttons(mouse_buttons);
}

/* ------------------------------------------------------------------ */
/* Tablet (absolute pointer / touch)                                   */
/* ------------------------------------------------------------------ */

/* Left/right/middle bit (TAB_FLAG_*) for a sokol mouse button. */
static uint8_t mouse_button_bit(sapp_mousebutton mb)
{
    return mb == SAPP_MOUSEBUTTON_LEFT     ? TAB_FLAG_LEFT
           : mb == SAPP_MOUSEBUTTON_RIGHT  ? TAB_FLAG_RIGHT
           : mb == SAPP_MOUSEBUTTON_MIDDLE ? TAB_FLAG_MIDDLE
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
        b |= TAB_FLAG_LEFT;
    if (e->modifiers & SAPP_MODIFIER_RMB)
        b |= TAB_FLAG_RIGHT;
    if (e->modifiers & SAPP_MODIFIER_MMB)
        b |= TAB_FLAG_MIDDLE;
    uint8_t bit = mouse_button_bit(e->mouse_button);
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
        bool inside = window_canvas_from_fb(e->mouse_x, e->mouse_y, &cx, &cy);
        window_set_pointer_on_canvas(inside); /* the tablet owns the cursor only on-canvas */
        uint8_t buttons = pointer_buttons(e);
        if (inside)
            tab_host_pointer(cx, cy, buttons);
        else
            tab_host_clear(); /* outside the canvas: no contact, all buttons released */
        if (mou_is_mapped()) /* the same physical pointer also drives the mouse block */
        {
            mou_host_buttons(buttons);
            if (e->type == SAPP_EVENTTYPE_MOUSE_MOVE)
            {
                int cw, ch;
                vga_canvas_size(&cw, &ch);
                float onscreen_w = (float)cw * window_canvas_scale();
                if (onscreen_w > 0.0f)
                {
                    float gain = INPUT_MOUSE_REF_WIDTH / onscreen_w;
                    mou_host_move(e->mouse_dx * gain, e->mouse_dy * gain);
                }
            }
        }
        return true;
    }
    case SAPP_EVENTTYPE_MOUSE_SCROLL:
        tab_host_wheel((int)lroundf(e->scroll_y), (int)lroundf(e->scroll_x));
        if (mou_is_mapped()) /* the same scroll also drives the mouse block */
            mou_host_wheel((int)lroundf(e->scroll_y), (int)lroundf(e->scroll_x));
        return true;
    case SAPP_EVENTTYPE_MOUSE_LEAVE:
        window_set_pointer_on_canvas(false); /* hand the cursor back to the system */
        tab_host_clear();                    /* pointer left the window */
        return true;
    case SAPP_EVENTTYPE_TOUCHES_BEGAN:
    case SAPP_EVENTTYPE_TOUCHES_MOVED:
    case SAPP_EVENTTYPE_TOUCHES_ENDED:
    case SAPP_EVENTTYPE_TOUCHES_CANCELLED:
    {
        bool ending = e->type == SAPP_EVENTTYPE_TOUCHES_ENDED ||
                      e->type == SAPP_EVENTTYPE_TOUCHES_CANCELLED;
        tab_point_t pts[SAPP_MAX_TOUCHPOINTS];
        int n = 0;
        for (int i = 0; i < e->num_touches && n < SAPP_MAX_TOUCHPOINTS; ++i)
        {
            if (ending && e->touches[i].changed)
                continue; /* the finger lifting this event is no longer a contact */
            if (!window_canvas_from_fb(e->touches[i].pos_x, e->touches[i].pos_y, &cx, &cy))
                continue; /* touch in the letterbox: not a canvas contact */
            pts[n].x = (int16_t)cx;
            pts[n].y = (int16_t)cy;
            n++;
        }
        tab_host_touch(pts, n);
        return true;
    }
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* Clipboard paste                                                     */
/* ------------------------------------------------------------------ */

/* Pasted text still being dripped into the keyboard ring (NULL = idle). */
static char *paste_buf;
static size_t paste_len, paste_pos;

/* UTF-8 sequence length from the lead byte (1 for ASCII and invalid leads). */
static size_t utf8_len(uint8_t lead)
{
    if ((lead & 0xe0) == 0xc0)
        return 2;
    if ((lead & 0xf0) == 0xe0)
        return 3;
    if ((lead & 0xf8) == 0xf0)
        return 4;
    return 1;
}

void input_paste_cancel(void)
{
    free(paste_buf);
    paste_buf = NULL;
}

/* A new paste replaces any still-dripping one. */
static void paste_start(const char *s)
{
    input_paste_cancel();
    size_t n = s ? strlen(s) : 0;
    if (!n)
        return;
    paste_buf = malloc(n);
    if (!paste_buf)
        return;
    memcpy(paste_buf, s, n);
    paste_len = n;
    paste_pos = 0;
}

void input_paste_pump(void)
{
    if (!paste_buf)
        return;
    /* Stay under the ring's headroom so live typing still fits during a long
     * paste; a full ring drops bytes, which would corrupt the paste. */
    while (paste_pos < paste_len && com_kbd_free() > 64)
    {
        char c = paste_buf[paste_pos];
        if (c == '\r' || c == '\n')
        {
            kbd_key(KBD_KEY_ENTER, false, false, false);
            paste_pos++;
            if (c == '\r' && paste_pos < paste_len && paste_buf[paste_pos] == '\n')
                paste_pos++; /* CRLF is one Enter */
        }
        else if (c == '\t')
        {
            kbd_key(KBD_KEY_TAB, false, false, false);
            paste_pos++;
        }
        else if ((uint8_t)c < 32 || c == 127)
        {
            paste_pos++; /* strip other control bytes */
        }
        else
        {
            char seq[5];
            size_t n = utf8_len((uint8_t)c);
            if (n > paste_len - paste_pos)
                n = paste_len - paste_pos;
            memcpy(seq, paste_buf + paste_pos, n);
            seq[n] = '\0';
            kbd_text(seq);
            paste_pos += n;
        }
    }
    if (paste_pos >= paste_len)
    {
        free(paste_buf);
        paste_buf = NULL;
    }
}

void input_event(const sapp_event *e)
{
    /* An absolute-pointer program takes host pointer/touch events directly (no
     * capture); input_tablet consumes those and returns true. Everything else
     * (keys, and pointer events when no tablet is mapped) falls through below. */
    if (tab_is_mapped() && input_tablet(e))
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
            if (mou_is_mapped())
                sapp_lock_mouse(true);
        }
        else
            set_mouse_button(e->mouse_button, true);
        break;
    case SAPP_EVENTTYPE_MOUSE_UP:
        if (sapp_mouse_locked())
            set_mouse_button(e->mouse_button, false);
        break;
    case SAPP_EVENTTYPE_MOUSE_MOVE:
        if (sapp_mouse_locked())
        {
            int cw, ch;
            vga_canvas_size(&cw, &ch);
            float onscreen_w = (float)cw * window_canvas_scale(); /* drawn canvas width, fb px */
            if (onscreen_w > 0.0f)
            {
                float gain = INPUT_MOUSE_REF_WIDTH / onscreen_w; /* counts per fb pixel */
                mou_host_move(e->mouse_dx * gain, e->mouse_dy * gain);
            }
        }
        break;
    case SAPP_EVENTTYPE_MOUSE_SCROLL:
        if (sapp_mouse_locked())
            mou_host_wheel((int)lroundf(e->scroll_y), (int)lroundf(e->scroll_x));
        break;
    case SAPP_EVENTTYPE_CLIPBOARD_PASTED:
        paste_start(sapp_get_clipboard_string());
        break;
    default:
        break;
    }
}
