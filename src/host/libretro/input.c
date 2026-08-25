/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What the frontend's devices mean to the machine's.
 *
 * The frontend owns the hardware here, so nothing in this file opens a
 * device: the keyboard arrives as events and everything else is read once a
 * frame from the callback the frontend gave us. That is why there is no
 * pad_input.c in this host — its whole job is deciding when it is polite to
 * open a controller, and a frontend has already decided.
 */

#include "input.h"

#include "core/emu/hid/kbd.h"
#include "core/hid/kbd.h"
#include "core/hid/mou.h"
#include "core/hid/pad.h"
#include "core/hid/tab.h"
#include "core/emu/sys/vga.h"

#include "libretro.h"

/* ------------------------------------------------------------------ */
/* Keyboard                                                            */
/* ------------------------------------------------------------------ */

/* A RETROK code as a USB HID keyboard usage id, for the xreg keyboard bitmap.
 * 0 = unmapped (not reported). RETROK is ASCII for the printable keys, which
 * is most of this table's shortness. */
static uint8_t retrok_to_hid(unsigned k)
{
    if (k >= RETROK_a && k <= RETROK_z)
        return (uint8_t)(0x04 + (k - RETROK_a));
    if (k >= RETROK_1 && k <= RETROK_9)
        return (uint8_t)(0x1E + (k - RETROK_1));
    if (k >= RETROK_F1 && k <= RETROK_F12)
        return (uint8_t)(0x3A + (k - RETROK_F1));
    if (k >= RETROK_KP1 && k <= RETROK_KP9)
        return (uint8_t)(0x59 + (k - RETROK_KP1));
    switch (k)
    {
    case RETROK_KP0: return 0x62;
    case RETROK_KP_PERIOD: return 0x63;
    case RETROK_KP_DIVIDE: return 0x54;
    case RETROK_KP_MULTIPLY: return 0x55;
    case RETROK_KP_MINUS: return 0x56;
    case RETROK_KP_PLUS: return 0x57;
    case RETROK_KP_EQUALS: return 0x67;
    case RETROK_KP_ENTER: return 0x58;
    case RETROK_CAPSLOCK: return 0x39;
    case RETROK_SCROLLOCK: return 0x47;
    case RETROK_NUMLOCK: return 0x53;
    case RETROK_PRINT: return 0x46;
    case RETROK_PAUSE: return 0x48;
    case RETROK_MENU: return 0x65;
    case RETROK_LCTRL: return 0xE0;
    case RETROK_LSHIFT: return 0xE1;
    case RETROK_LALT: return 0xE2;
    case RETROK_LSUPER: return 0xE3;
    case RETROK_RCTRL: return 0xE4;
    case RETROK_RSHIFT: return 0xE5;
    case RETROK_RALT: return 0xE6;
    case RETROK_RSUPER: return 0xE7;
    case RETROK_0: return 0x27;
    case RETROK_RETURN: return 0x28;
    case RETROK_ESCAPE: return 0x29;
    case RETROK_BACKSPACE: return 0x2A;
    case RETROK_TAB: return 0x2B;
    case RETROK_SPACE: return 0x2C;
    case RETROK_MINUS: return 0x2D;
    case RETROK_EQUALS: return 0x2E;
    case RETROK_LEFTBRACKET: return 0x2F;
    case RETROK_RIGHTBRACKET: return 0x30;
    case RETROK_BACKSLASH: return 0x31;
    case RETROK_SEMICOLON: return 0x33;
    case RETROK_QUOTE: return 0x34;
    case RETROK_BACKQUOTE: return 0x35;
    case RETROK_COMMA: return 0x36;
    case RETROK_PERIOD: return 0x37;
    case RETROK_SLASH: return 0x38;
    case RETROK_RIGHT: return 0x4F;
    case RETROK_LEFT: return 0x50;
    case RETROK_DOWN: return 0x51;
    case RETROK_UP: return 0x52;
    case RETROK_DELETE: return 0x4C;
    case RETROK_HOME: return 0x4A;
    case RETROK_END: return 0x4D;
    case RETROK_INSERT: return 0x49;
    case RETROK_PAGEUP: return 0x4B;
    case RETROK_PAGEDOWN: return 0x4E;
    default: return 0;
    }
}

/* Encode one Unicode codepoint as NUL-terminated UTF-8 (kbd_text then maps it
 * to the active OEM code page). */
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

/* US-ASCII of a printable RETROK honoring shift, else 0. A frontend that sends
 * no character (several do not) still gets chords and Meta this way — a
 * US-layout approximation, not an OEM-codepage match. */
static char ascii_from_key(unsigned k, bool shift)
{
    if (k >= RETROK_a && k <= RETROK_z)
        return (char)(shift ? 'A' + (k - RETROK_a) : 'a' + (k - RETROK_a));
    if (k >= RETROK_0 && k <= RETROK_9)
    {
        static const char shifted[] = ")!@#$%^&*(";
        return shift ? shifted[k - RETROK_0] : (char)('0' + (k - RETROK_0));
    }
    switch (k)
    {
    case RETROK_SPACE: return ' ';
    case RETROK_MINUS: return shift ? '_' : '-';
    case RETROK_EQUALS: return shift ? '+' : '=';
    case RETROK_LEFTBRACKET: return shift ? '{' : '[';
    case RETROK_RIGHTBRACKET: return shift ? '}' : ']';
    case RETROK_BACKSLASH: return shift ? '|' : '\\';
    case RETROK_SEMICOLON: return shift ? ':' : ';';
    case RETROK_QUOTE: return shift ? '"' : '\'';
    case RETROK_BACKQUOTE: return shift ? '~' : '`';
    case RETROK_COMMA: return shift ? '<' : ',';
    case RETROK_PERIOD: return shift ? '>' : '.';
    case RETROK_SLASH: return shift ? '?' : '/';
    default: return 0;
    }
}

/* A numpad key with NumLock off is the navigation key printed under the digit;
 * unlike sokol, a frontend tells us which it is. */
static bool numpad_nav(unsigned k, bool numlock, kbd_key_t *key)
{
    if (numlock)
        return false;
    switch (k)
    {
    case RETROK_KP1: *key = KBD_KEY_END; return true;
    case RETROK_KP2: *key = KBD_KEY_DOWN; return true;
    case RETROK_KP3: *key = KBD_KEY_PAGE_DOWN; return true;
    case RETROK_KP4: *key = KBD_KEY_LEFT; return true;
    case RETROK_KP6: *key = KBD_KEY_RIGHT; return true;
    case RETROK_KP7: *key = KBD_KEY_HOME; return true;
    case RETROK_KP8: *key = KBD_KEY_UP; return true;
    case RETROK_KP9: *key = KBD_KEY_PAGE_UP; return true;
    case RETROK_KP0: *key = KBD_KEY_INSERT; return true;
    case RETROK_KP_PERIOD: *key = KBD_KEY_DELETE; return true;
    default: return false;
    }
}

static bool named_key(unsigned k, kbd_key_t *key)
{
    if (k >= RETROK_F1 && k <= RETROK_F12)
    {
        *key = (kbd_key_t)(KBD_KEY_F1 + (k - RETROK_F1));
        return true;
    }
    switch (k)
    {
    case RETROK_RETURN:
    case RETROK_KP_ENTER: *key = KBD_KEY_ENTER; return true;
    case RETROK_BACKSPACE: *key = KBD_KEY_BACKSPACE; return true;
    case RETROK_TAB: *key = KBD_KEY_TAB; return true;
    case RETROK_ESCAPE: *key = KBD_KEY_ESCAPE; return true;
    case RETROK_UP: *key = KBD_KEY_UP; return true;
    case RETROK_DOWN: *key = KBD_KEY_DOWN; return true;
    case RETROK_LEFT: *key = KBD_KEY_LEFT; return true;
    case RETROK_RIGHT: *key = KBD_KEY_RIGHT; return true;
    case RETROK_HOME: *key = KBD_KEY_HOME; return true;
    case RETROK_END: *key = KBD_KEY_END; return true;
    case RETROK_INSERT: *key = KBD_KEY_INSERT; return true;
    case RETROK_DELETE: *key = KBD_KEY_DELETE; return true;
    case RETROK_PAGEUP: *key = KBD_KEY_PAGE_UP; return true;
    case RETROK_PAGEDOWN: *key = KBD_KEY_PAGE_DOWN; return true;
    default: return false;
    }
}

void input_keyboard_event(bool down, unsigned keycode, uint32_t character,
                          uint16_t key_modifiers)
{
    uint8_t hid = retrok_to_hid(keycode);
    if (hid)
        kbd_hid_set(hid, down);
    if (!down)
        return;

    bool ctrl = (key_modifiers & RETROKMOD_CTRL) != 0;
    bool shift = (key_modifiers & RETROKMOD_SHIFT) != 0;
    bool alt = (key_modifiers & RETROKMOD_ALT) != 0;

    switch (keycode)
    {
    case RETROK_NUMLOCK: kbd_toggle_lock(1); return;
    case RETROK_CAPSLOCK: kbd_toggle_lock(2); return;
    case RETROK_SCROLLOCK: kbd_toggle_lock(4); return;
    default: break;
    }

    kbd_key_t key;
    if (numpad_nav(keycode, (key_modifiers & RETROKMOD_NUMLOCK) != 0, &key) ||
        named_key(keycode, &key))
    {
        kbd_key(key, ctrl, shift, alt);
        return;
    }

    /* Ctrl+<key> is a C0 byte and Alt+<key> is ESC then the byte, so neither
     * is the character the frontend composed; both are built from the keycode
     * the way the firmware promotes them. */
    if (ctrl && !alt)
    {
        kbd_ctrl_letter(ascii_from_key(keycode, shift));
        return;
    }
    if (alt)
    {
        kbd_alt_char(ascii_from_key(keycode, shift), ctrl);
        return;
    }

    /* Plain typing. The frontend already applied the layout, so its character
     * is better than anything reconstructed from a keycode — but a frontend
     * that sends none still types, from the US-layout approximation. */
    char u[5];
    if (character >= 32 && character != 127)
        kbd_text(utf8_encode(character, u));
    else
    {
        char ch = ascii_from_key(keycode, shift);
        if (ch)
            kbd_text(utf8_encode((uint32_t)(unsigned char)ch, u));
    }
}

/* ------------------------------------------------------------------ */
/* Gamepads                                                            */
/* ------------------------------------------------------------------ */

/* RETRO_DEVICE_NONE until the frontend says otherwise, and a joypad is what
 * the frontend means by nothing said (retro_set_controller_port_device is not
 * guaranteed to be called for a port that is simply plugged in). */
static unsigned port_device[PAD_PLAYERS] = {
    RETRO_DEVICE_JOYPAD, RETRO_DEVICE_JOYPAD,
    RETRO_DEVICE_JOYPAD, RETRO_DEVICE_JOYPAD};
static bool port_live[PAD_PLAYERS];

void input_set_port_device(unsigned port, unsigned device)
{
    if (port >= PAD_PLAYERS)
        return;
    port_device[port] = device;
    if (device == RETRO_DEVICE_NONE && port_live[port])
    {
        pad_connect((int)port, false, PAD_TYPE_UNKNOWN, false);
        port_live[port] = false;
    }
}

/* A face/shoulder button, taking the analog reading where the frontend has
 * one. A frontend without analog buttons answers 0 to the analog query, which
 * is also what a released button reads, so the digital query settles it. */
static uint16_t button_value(retro_input_state_t state, unsigned port, unsigned id)
{
    uint16_t v = (uint16_t)state(port, RETRO_DEVICE_ANALOG,
                                 RETRO_DEVICE_INDEX_ANALOG_BUTTON, id);
    if (v)
        return v;
    return state(port, RETRO_DEVICE_JOYPAD, 0, id) ? 0x7FFF : 0;
}

static void poll_pads(retro_input_state_t state)
{
    for (int p = 0; p < PAD_PLAYERS; p++)
    {
        if (port_device[p] == RETRO_DEVICE_NONE)
            continue;

        uint8_t dpad = 0, b0 = 0, b1 = 0;
        static const struct
        {
            unsigned id;
            pad_button_t btn;
        } digital[] = {
            {RETRO_DEVICE_ID_JOYPAD_UP, PAD_BTN_DPAD_UP},
            {RETRO_DEVICE_ID_JOYPAD_DOWN, PAD_BTN_DPAD_DOWN},
            {RETRO_DEVICE_ID_JOYPAD_LEFT, PAD_BTN_DPAD_LEFT},
            {RETRO_DEVICE_ID_JOYPAD_RIGHT, PAD_BTN_DPAD_RIGHT},
            /* Positional, not by name: the RetroPad's B is its south button
             * and this machine's A is too. */
            {RETRO_DEVICE_ID_JOYPAD_B, PAD_BTN_A},
            {RETRO_DEVICE_ID_JOYPAD_A, PAD_BTN_B},
            {RETRO_DEVICE_ID_JOYPAD_Y, PAD_BTN_X},
            {RETRO_DEVICE_ID_JOYPAD_X, PAD_BTN_Y},
            {RETRO_DEVICE_ID_JOYPAD_L, PAD_BTN_L1},
            {RETRO_DEVICE_ID_JOYPAD_R, PAD_BTN_R1},
            {RETRO_DEVICE_ID_JOYPAD_SELECT, PAD_BTN_SELECT},
            {RETRO_DEVICE_ID_JOYPAD_START, PAD_BTN_START},
            {RETRO_DEVICE_ID_JOYPAD_L3, PAD_BTN_L3},
            {RETRO_DEVICE_ID_JOYPAD_R3, PAD_BTN_R3},
        };
        for (size_t i = 0; i < sizeof digital / sizeof *digital; i++)
            pad_button_apply(digital[i].btn,
                             state((unsigned)p, RETRO_DEVICE_JOYPAD, 0, digital[i].id) != 0,
                             &dpad, &b0, &b1);

        uint16_t lt = button_value(state, (unsigned)p, RETRO_DEVICE_ID_JOYPAD_L2);
        uint16_t rt = button_value(state, (unsigned)p, RETRO_DEVICE_ID_JOYPAD_R2);
        pad_button_apply(PAD_BTN_L2, lt != 0, &dpad, &b0, &b1);
        pad_button_apply(PAD_BTN_R2, rt != 0, &dpad, &b0, &b1);

        /* The block's units: sticks signed 8-bit, triggers unsigned 8-bit. */
        int lx = state((unsigned)p, RETRO_DEVICE_ANALOG,
                       RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X) >> 8;
        int ly = state((unsigned)p, RETRO_DEVICE_ANALOG,
                       RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y) >> 8;
        int rx = state((unsigned)p, RETRO_DEVICE_ANALOG,
                       RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X) >> 8;
        int ry = state((unsigned)p, RETRO_DEVICE_ANALOG,
                       RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y) >> 8;

        pad_connect(p, true, PAD_TYPE_WESTERN, port_device[p] == RETRO_DEVICE_ANALOG);
        port_live[p] = true;
        pad_host_report(p, dpad, b0, b1, lx, ly, rx, ry, lt >> 7, rt >> 7);
    }
}

/* ------------------------------------------------------------------ */
/* Pointer and mouse                                                   */
/* ------------------------------------------------------------------ */

/* Both devices are read only once a program has asked for the block, which is
 * the same courtesy the desktop hosts extend: a ROM that wants neither is not
 * a reason to watch where anyone is pointing. */
static void poll_pointer(retro_input_state_t state)
{
    if (tab_is_mapped())
    {
        int w, h;
        vga_canvas_size(&w, &h);
        int count = state(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_COUNT);
        if (count > TAB_MAX_CONTACTS)
            count = TAB_MAX_CONTACTS;

        /* [-0x7FFF, 0x7FFF] spans the frame we last handed over, whatever the
         * frontend then did with it on screen. */
        tab_point_t pts[TAB_MAX_CONTACTS];
        int n = 0;
        bool pressed0 = false;
        for (int i = 0; i < count; i++)
        {
            if (!state(0, RETRO_DEVICE_POINTER, (unsigned)i, RETRO_DEVICE_ID_POINTER_PRESSED))
                continue;
            int px = state(0, RETRO_DEVICE_POINTER, (unsigned)i, RETRO_DEVICE_ID_POINTER_X);
            int py = state(0, RETRO_DEVICE_POINTER, (unsigned)i, RETRO_DEVICE_ID_POINTER_Y);
            pts[n].x = (int16_t)(((px + 0x7FFF) * (w - 1)) / 0xFFFE);
            pts[n].y = (int16_t)(((py + 0x7FFF) * (h - 1)) / 0xFFFE);
            if (i == 0)
                pressed0 = true;
            n++;
        }
        if (n > 1)
            tab_host_touch(pts, n);
        else if (n == 1)
            /* One contact is a pointer rather than a finger, and a pointer
             * carries a button the touch form has no room for. */
            tab_host_pointer(pts[0].x, pts[0].y, pressed0 ? TAB_FLAG_LEFT : 0);
        else
            tab_host_clear();
    }

    if (mou_is_mapped())
    {
        int dx = state(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
        int dy = state(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
        if (dx || dy)
            mou_host_move((float)dx, (float)dy);
        uint8_t buttons = 0;
        if (state(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT))
            buttons |= 1;
        if (state(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT))
            buttons |= 2;
        if (state(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_MIDDLE))
            buttons |= 4;
        mou_host_buttons(buttons);
        int up = state(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELUP);
        int dn = state(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELDOWN);
        if (up || dn)
            mou_host_wheel(up ? 1 : -1, 0);
    }
}

void input_poll(retro_input_state_t state)
{
    poll_pads(state);
    poll_pointer(state);
}
