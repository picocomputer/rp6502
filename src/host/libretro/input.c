/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "input.h"

#include "core/hid/vtkeys.h"
#include "core/hid/keyboard.h"
#include "core/hid/usage.h"
#include "core/hid/mouse.h"
#include "core/hid/gamepad.h"
#include "core/hid/tablet.h"
#include "core/vga/vga_emu.h"

#include "libretro.h"

/* A RETROK code as a USB HID keyboard usage id, or 0 for a key this machine
 * does not report. */
static uint8_t retrok_to_hid(unsigned k)
{
    if (k >= RETROK_a && k <= RETROK_z)
        return (uint8_t)(0x04 + (k - RETROK_a));
    if (k >= RETROK_1 && k <= RETROK_9)
        return (uint8_t)(0x1E + (k - RETROK_1));
    if (k >= RETROK_F1 && k <= RETROK_F12)
        return (uint8_t)(0x3A + (k - RETROK_F1));
    if (k >= RETROK_F13 && k <= RETROK_F15)
        return (uint8_t)(0x68 + (k - RETROK_F13));
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
    case RETROK_BREAK: return 0x48; /* Pause and Break are the same key */
    case RETROK_MENU: return 0x65;
    case RETROK_LCTRL: return 0xE0;
    case RETROK_LSHIFT: return 0xE1;
    case RETROK_LALT: return 0xE2;
    case RETROK_LSUPER: return 0xE3;
    /* META and SUPER are two names for the same physical key, and a frontend
     * may report either. */
    case RETROK_LMETA: return 0xE3;
    case RETROK_RCTRL: return 0xE4;
    case RETROK_RSHIFT: return 0xE5;
    case RETROK_RALT: return 0xE6;
    case RETROK_RSUPER: return 0xE7;
    case RETROK_RMETA: return 0xE7;
    case RETROK_MODE: return 0xE6; /* AltGr is the right Alt */
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
    /* The ISO key between the left Shift and Z, which no ANSI board has. */
    case RETROK_OEM_102: return 0x64;
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

/* US-ASCII of a printable RETROK honoring shift, else 0. Several frontends
 * send no composed character at all, and this is what still gets those a
 * character and a chord. It approximates a US layout rather than matching the
 * machine's OEM code page. */
static char ascii_from_key(unsigned k, bool shift)
{
    if (k >= RETROK_a && k <= RETROK_z)
        return (char)(shift ? 'A' + (k - RETROK_a) : 'a' + (k - RETROK_a));
    if (k >= RETROK_0 && k <= RETROK_9)
    {
        static const char shifted[] = ")!@#$%^&*(";
        return shift ? shifted[k - RETROK_0] : (char)('0' + (k - RETROK_0));
    }
    /* NumLock is the caller's business: with it off the caller swaps a keypad
     * usage for its navigation usage, which vtkeys_key then consumes. KEYPAD_5
     * has no navigation key, so it arrives here and types its digit either
     * way. */
    if (k >= RETROK_KP0 && k <= RETROK_KP9)
        return (char)('0' + (k - RETROK_KP0));
    switch (k)
    {
    case RETROK_KP_PERIOD: return '.';
    case RETROK_KP_DIVIDE: return '/';
    case RETROK_KP_MULTIPLY: return '*';
    case RETROK_KP_MINUS: return '-';
    case RETROK_KP_PLUS: return '+';
    case RETROK_KP_EQUALS: return '=';
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
    /* Some frontends report the symbol rather than the key that made it. */
    case RETROK_EXCLAIM: return '!';
    case RETROK_QUOTEDBL: return '"';
    case RETROK_HASH: return '#';
    case RETROK_DOLLAR: return '$';
    case RETROK_AMPERSAND: return '&';
    case RETROK_LEFTPAREN: return '(';
    case RETROK_RIGHTPAREN: return ')';
    case RETROK_ASTERISK: return '*';
    case RETROK_PLUS: return '+';
    case RETROK_COLON: return ':';
    case RETROK_LESS: return '<';
    case RETROK_GREATER: return '>';
    case RETROK_QUESTION: return '?';
    case RETROK_AT: return '@';
    case RETROK_CARET: return '^';
    case RETROK_UNDERSCORE: return '_';
    default: return 0;
    }
}

void input_keyboard_event(bool down, unsigned keycode, uint32_t character,
                          uint16_t key_modifiers)
{
    uint8_t hid = retrok_to_hid(keycode);
    if (hid)
        keyboard_hid_set(hid, down);
    if (!down)
        return;

    bool ctrl = (key_modifiers & RETROKMOD_CTRL) != 0;
    bool shift = (key_modifiers & RETROKMOD_SHIFT) != 0;
    bool alt = (key_modifiers & RETROKMOD_ALT) != 0;

    /* The frontend sends all three lock states in every event, so they are
     * taken from it rather than toggled here. Toggling would have to start
     * from a guess and would then drift whenever a lock key is pressed while
     * the core does not have focus. */
    keyboard_set_locks(
        (uint8_t)(((key_modifiers & RETROKMOD_NUMLOCK) ? KEYBOARD_LED_NUMLOCK : 0) |
                  ((key_modifiers & RETROKMOD_CAPSLOCK) ? KEYBOARD_LED_CAPSLOCK : 0) |
                  ((key_modifiers & RETROKMOD_SCROLLOCK) ? KEYBOARD_LED_SCROLLLOCK : 0)));
    switch (keycode)
    {
    case RETROK_NUMLOCK:
    case RETROK_CAPSLOCK:
    case RETROK_SCROLLOCK: return;
    default: break;
    }

    /* A numpad key with NumLock off is the navigation key printed under the
     * digit. */
    if (!(key_modifiers & RETROKMOD_NUMLOCK))
    {
        uint8_t nav = keyboard_keypad_nav(hid);
        if (nav)
            hid = nav;
    }
    if (vtkeys_key(hid, ctrl, shift, alt))
        return;

    /* Ctrl+<key> is a C0 byte and Alt+<key> is ESC then the byte, so neither
     * is the character the frontend composed. Both are built from the keycode
     * instead. */
    if (ctrl && !alt)
    {
        vtkeys_ctrl_letter(ascii_from_key(keycode, shift));
        return;
    }
    if (alt)
    {
        vtkeys_alt_char(ascii_from_key(keycode, shift), ctrl);
        return;
    }

    /* The frontend has already applied the keyboard layout, so its character
     * is better than anything reconstructed from a keycode. */
    if (character >= 32 && character != 127)
        vtkeys_char(character);
    else
    {
        char ch = ascii_from_key(keycode, shift);
        if (ch)
            vtkeys_char((uint32_t)(unsigned char)ch);
    }
}

/* Every port starts as a joypad, because a frontend is not required to call
 * retro_set_controller_port_device for a port that is simply plugged in. */
static unsigned port_device[GAMEPAD_PLAYERS] = {
    RETRO_DEVICE_JOYPAD, RETRO_DEVICE_JOYPAD,
    RETRO_DEVICE_JOYPAD, RETRO_DEVICE_JOYPAD};
static bool port_live[GAMEPAD_PLAYERS];
static bool have_bitmasks;
static bool mouse_owns_pointer;

/* How many players the frontend actually has. It offers four ports whether or
 * not anyone is holding a controller, so connecting all four would show four
 * players to a program that counts them. GET_INPUT_MAX_USERS is the frontend
 * saying how many are real; a frontend that will not answer, or answers zero,
 * gets all four. */
static int max_users = GAMEPAD_PLAYERS;
static retro_environment_t input_environ;

void input_init(retro_environment_t environ_cb)
{
    input_environ = environ_cb;
    have_bitmasks = environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL);
}

/* Asked every frame rather than once, because libretro.h says the count may
 * change between frames: someone plugs a controller in while a program runs. */
static void refresh_max_users(void)
{
    unsigned n = 0;
    if (input_environ &&
        input_environ(RETRO_ENVIRONMENT_GET_INPUT_MAX_USERS, &n) && n)
        max_users = (int)(n < GAMEPAD_PLAYERS ? n : GAMEPAD_PLAYERS);
    else
        max_users = GAMEPAD_PLAYERS;
}

void input_state_restored(void)
{
    for (int p = 0; p < GAMEPAD_PLAYERS; p++)
        port_live[p] = true;
    keyboard_release_all();
}

void input_reset(void)
{
    for (int p = 0; p < GAMEPAD_PLAYERS; p++)
    {
        port_device[p] = RETRO_DEVICE_JOYPAD;
        port_live[p] = false;
    }
    have_bitmasks = false;
    max_users = GAMEPAD_PLAYERS;
    mouse_owns_pointer = false;
    input_environ = NULL;
}

void input_set_port_device(unsigned port, unsigned device)
{
    if (port >= GAMEPAD_PLAYERS)
        return;
    port_device[port] = device;
    if (device == RETRO_DEVICE_NONE && port_live[port])
    {
        gamepad_connect((int)port, false, GAMEPAD_TYPE_UNKNOWN, false);
        port_live[port] = false;
    }
}

/* A face or shoulder button, taking the analog reading where the frontend has
 * one. A frontend without analog buttons answers 0 to the analog query, which
 * is also what a released button reads, so the digital query settles it. */
static uint16_t button_value(retro_input_state_t state, unsigned port, unsigned id,
                             int16_t mask)
{
    uint16_t v = (uint16_t)state(port, RETRO_DEVICE_ANALOG,
                                 RETRO_DEVICE_INDEX_ANALOG_BUTTON, id);
    if (v)
        return v;
    bool down = have_bitmasks ? (mask & (1 << id)) != 0
                              : state(port, RETRO_DEVICE_JOYPAD, 0, id) != 0;
    return down ? 0x7FFF : 0;
}

static void poll_gamepads(retro_input_state_t state)
{
    for (int p = 0; p < GAMEPAD_PLAYERS; p++)
    {
        if (port_device[p] == RETRO_DEVICE_NONE || p >= max_users)
        {
            if (port_live[p])
            {
                gamepad_connect(p, false, GAMEPAD_TYPE_UNKNOWN, false);
                port_live[p] = false;
            }
            continue;
        }

        uint8_t dpad = 0, b0 = 0, b1 = 0;
        static const struct
        {
            unsigned id;
            gamepad_button_t btn;
        } digital[] = {
            {RETRO_DEVICE_ID_JOYPAD_UP, GAMEPAD_BTN_DPAD_UP},
            {RETRO_DEVICE_ID_JOYPAD_DOWN, GAMEPAD_BTN_DPAD_DOWN},
            {RETRO_DEVICE_ID_JOYPAD_LEFT, GAMEPAD_BTN_DPAD_LEFT},
            {RETRO_DEVICE_ID_JOYPAD_RIGHT, GAMEPAD_BTN_DPAD_RIGHT},
            /* Mapped by position, not by name: the RetroPad's B is its south
             * button and this machine's A is too. */
            {RETRO_DEVICE_ID_JOYPAD_B, GAMEPAD_BTN_A},
            {RETRO_DEVICE_ID_JOYPAD_A, GAMEPAD_BTN_B},
            {RETRO_DEVICE_ID_JOYPAD_Y, GAMEPAD_BTN_X},
            {RETRO_DEVICE_ID_JOYPAD_X, GAMEPAD_BTN_Y},
            {RETRO_DEVICE_ID_JOYPAD_L, GAMEPAD_BTN_L1},
            {RETRO_DEVICE_ID_JOYPAD_R, GAMEPAD_BTN_R1},
            {RETRO_DEVICE_ID_JOYPAD_SELECT, GAMEPAD_BTN_SELECT},
            {RETRO_DEVICE_ID_JOYPAD_START, GAMEPAD_BTN_START},
            {RETRO_DEVICE_ID_JOYPAD_L3, GAMEPAD_BTN_L3},
            {RETRO_DEVICE_ID_JOYPAD_R3, GAMEPAD_BTN_R3},
        };
        int16_t mask = 0;
        if (have_bitmasks)
            mask = state((unsigned)p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
        for (size_t i = 0; i < sizeof digital / sizeof *digital; i++)
        {
            bool down = have_bitmasks
                            ? (mask & (1 << digital[i].id)) != 0
                            : state((unsigned)p, RETRO_DEVICE_JOYPAD, 0, digital[i].id) != 0;
            gamepad_button_apply(digital[i].btn, down, &dpad, &b0, &b1);
        }

        uint16_t lt = button_value(state, (unsigned)p, RETRO_DEVICE_ID_JOYPAD_L2, mask);
        uint16_t rt = button_value(state, (unsigned)p, RETRO_DEVICE_ID_JOYPAD_R2, mask);
        gamepad_button_apply(GAMEPAD_BTN_L2, lt != 0, &dpad, &b0, &b1);
        gamepad_button_apply(GAMEPAD_BTN_R2, rt != 0, &dpad, &b0, &b1);

        /* A stick axis arrives as an int16 and a trigger as 0 to 0x7FFF, so
         * the shifts here and below bring them to the signed and unsigned
         * bytes gamepad_host_report takes. */
        int lx = state((unsigned)p, RETRO_DEVICE_ANALOG,
                        RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X) >> 8;
        int ly = state((unsigned)p, RETRO_DEVICE_ANALOG,
                        RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y) >> 8;
        int rx = state((unsigned)p, RETRO_DEVICE_ANALOG,
                        RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X) >> 8;
        int ry = state((unsigned)p, RETRO_DEVICE_ANALOG,
                        RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y) >> 8;

        /* A RetroPad is a western-layout gamepad with two sticks whatever
         * hardware is behind it. Claiming the sticks only for
         * RETRO_DEVICE_ANALOG would deny them to most players, because a
         * frontend reports a plain joypad for an analog controller unless
         * someone goes and changes it. */
        gamepad_connect(p, true, GAMEPAD_TYPE_WESTERN, true);
        port_live[p] = true;
        gamepad_host_report(p, dpad, b0, b1, lx, ly, rx, ry, lt >> 7, rt >> 7);
    }
}

/* A frontend reports pointer positions over [-0x7FFF, 0x7FFF], spanning the
 * frame this core last handed over, whatever the frontend then did with that
 * frame on screen. A lightgun runs to -0x8000 as well, which poll_lightgun
 * rejects before calling this. */
static int16_t canvas_coord(int p, int extent)
{
    return (int16_t)(((p + 0x7FFF) * (extent - 1)) / 0xFFFE);
}

/* A lightgun is an absolute pointer that hovers: screen coordinates plus a
 * trigger and two auxiliary buttons. Its range runs to -0x8000, which
 * libretro.h defines as out of bounds and the pointer has no equivalent of,
 * so an off-screen gun reports no contact rather than a bogus pixel. */
static void poll_lightgun(retro_input_state_t state, unsigned port, int w, int h)
{
    int gx = state(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X);
    int gy = state(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y);
    if (gx == -0x8000 || gy == -0x8000 ||
        state(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN))
    {
        tablet_host_clear();
        return;
    }
    uint8_t buttons = 0;
    if (state(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER))
        buttons |= TABLET_FLAG_LEFT;
    if (state(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_AUX_A))
        buttons |= TABLET_FLAG_RIGHT;
    if (state(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_AUX_B))
        buttons |= TABLET_FLAG_MIDDLE;
    tablet_host_pointer(canvas_coord(gx, w), canvas_coord(gy, h), buttons, false);
}

/* A gun is an absolute pointer with its own buttons, so it drives the tablet
 * in place of the mouse. */
static int lightgun_port(void)
{
    for (int p = 0; p < GAMEPAD_PLAYERS; p++)
        if ((port_device[p] & RETRO_DEVICE_MASK) == RETRO_DEVICE_LIGHTGUN)
            return p;
    return -1;
}

/* Neither device is read until a program has mapped it into XRAM, so a ROM
 * that wants neither is never a reason to watch where anyone is pointing.
 *
 * Touch and the mouse take turns at the tablet. The mouse drives it while
 * nothing is being touched; a touch takes it away and keeps it until the
 * touches end and the mouse moves again, so a finger never fights a cursor
 * parked somewhere else. mouse_owns_pointer says which of them holds it, and
 * it starts false so an untouched screen with no mouse yet reports no contact
 * rather than a ghost wherever the pointer last read.
 *
 * A contact reported while any mouse button is held is not a touch: libretro.h
 * says a frontend usually reports a held left button as a pointer contact, and
 * honouring that would turn a click into a finger going down. */
static void poll_pointer(retro_input_state_t state)
{
    bool tablet = tablet_is_mapped();
    bool mouse = mouse_is_mapped();
    if (!tablet && !mouse)
        return;

    int dx = state(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
    int dy = state(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
    uint8_t buttons = 0;
    if (state(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT))
        buttons |= TABLET_FLAG_LEFT;
    if (state(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT))
        buttons |= TABLET_FLAG_RIGHT;
    if (state(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_MIDDLE))
        buttons |= TABLET_FLAG_MIDDLE;
    if (state(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_BUTTON_4))
        buttons |= TABLET_FLAG_BTN4;
    if (state(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_BUTTON_5))
        buttons |= TABLET_FLAG_BTN5;

    /* Off the game image the cursor is over the rest of the frontend, and
     * nothing there is a press on the machine. A frontend that cannot tell
     * answers 0, which reads as on-screen. */
    bool offscreen = state(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_IS_OFFSCREEN);

    if (tablet)
    {
        int w, h;
        vga_canvas_size(&w, &h);
        int gun = lightgun_port();
        if (gun >= 0)
            poll_lightgun(state, (unsigned)gun, w, h);
        else
        {
            tablet_point_t pts[TABLET_MAX_CONTACTS];
            int n = 0;
            if (!buttons)
                for (int i = 0; i < TABLET_MAX_CONTACTS; i++)
                {
                    if (!state(0, RETRO_DEVICE_POINTER, (unsigned)i, RETRO_DEVICE_ID_POINTER_PRESSED))
                        break;
                    pts[n].x = canvas_coord(state(0, RETRO_DEVICE_POINTER, (unsigned)i, RETRO_DEVICE_ID_POINTER_X), w);
                    pts[n].y = canvas_coord(state(0, RETRO_DEVICE_POINTER, (unsigned)i, RETRO_DEVICE_ID_POINTER_Y), h);
                    n++;
                }
            if (n)
            {
                mouse_owns_pointer = false;
                tablet_host_touch(pts, n);
            }
            else
            {
                if (dx || dy)
                    mouse_owns_pointer = true;
                if (mouse_owns_pointer && !offscreen)
                {
                    int x = canvas_coord(state(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X), w);
                    int y = canvas_coord(state(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y), h);
                    /* libretro gives a core no way to ask the frontend to
                     * draw a cursor, so the program draws its own. */
                    tablet_host_pointer(x, y, buttons, false);
                }
                else
                    tablet_host_clear();
            }
        }
    }

    if (mouse)
    {
        if (dx || dy)
            mouse_host_move(dx * MOUSE_ONE, dy * MOUSE_ONE);
        mouse_host_buttons(offscreen ? 0 : buttons);
    }

    /* The wheel is read every frame because a frontend clears the tick when
     * the core reads it. */
    int dwheel = state(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELUP) -
                 state(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELDOWN);
    int dpan = state(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELUP) -
               state(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELDOWN);
    if ((dwheel || dpan) && !offscreen)
    {
        if (tablet)
            tablet_host_wheel(dwheel, dpan);
        if (mouse)
            mouse_host_wheel(dwheel, dpan);
    }
}

void input_poll(retro_input_state_t state)
{
    refresh_max_users();
    poll_gamepads(state);
    poll_pointer(state);
}
