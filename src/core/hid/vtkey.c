/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/hid/vtkey.h"
#include "core/hid/usage.h"
#include "core/term/cells.h"
#include "machine.h"

/* Keypad 1 to Keypad Decimal, in usage order: what they navigate to. Keypad 5
 * stays itself, the key xterm calls Begin. */
static const uint8_t HOST_IN_RAM vtkey_keypad_nav_keys[] = {
    HID_KEY_END, HID_KEY_ARROW_DOWN, HID_KEY_PAGE_DOWN,
    HID_KEY_ARROW_LEFT, HID_KEY_KEYPAD_5, HID_KEY_ARROW_RIGHT,
    HID_KEY_HOME, HID_KEY_ARROW_UP, HID_KEY_PAGE_UP,
    HID_KEY_INSERT, HID_KEY_DELETE};

/* Keypad Divide to Keypad Decimal, in usage order: the final character of the
 * application keypad's sequence. A VT100 has a comma where a PC has its plus,
 * so plus is the comma; divide and multiply are xterm's. */
static const char HOST_IN_RAM vtkey_keypad_app_keys[] = {
    'o', 'j', 'm', 'l', 'M',
    'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'p', 'n'};

/* F1 to Up are one contiguous span of HID usages, so the table is indexed
 * rather than searched. An intro of 'O' or '[' means the VT100 form and code
 * is its final character, which is all a VT52 sends after ESC; an intro of 0
 * means the VT220 form and code is its number. */
HOST_IN_RAM static const struct
{
    char intro;
    uint8_t code;
} vtkey_keys[] = {
    /* 0x3A..0x45, F1 to F12 */
    {'O', 'P'}, {'O', 'Q'}, {'O', 'R'}, {'O', 'S'},
    {0, 15}, {0, 17}, {0, 18}, {0, 19},
    {0, 20}, {0, 21}, {0, 23}, {0, 24},
    /* 0x46..0x48, PrintScreen, ScrollLock and Pause send nothing */
    {0, 0}, {0, 0}, {0, 0},
    /* 0x49..0x52, Insert to Up */
    {0, 2}, {'[', 'H'}, {0, 5}, {0, 3}, {'[', 'F'},
    {0, 6}, {'[', 'C'}, {'[', 'D'}, {'[', 'B'}, {'[', 'A'}};

/* Enter to Tab, in usage order. Ctrl makes Backspace a BS. */
static const char HOST_IN_RAM vtkey_c0_keys[] = {'\r', '\33', 0x7F, '\t'};

// Writes a number below 100 in decimal and returns where it ended.
static char *HOST_IN_RAM_FUNC vtkey_number(char *out, unsigned n)
{
    char tens = '0';
    for (; n >= 10; n -= 10)
        tens++;
    if (tens != '0')
        *out++ = tens;
    *out++ = (char)('0' + n);
    return out;
}

static bool HOST_IN_RAM_FUNC vtkey_is_keypad(uint8_t usage)
{
    return (usage >= HID_KEY_KEYPAD_DIVIDE && usage <= HID_KEY_KEYPAD_DECIMAL) ||
           usage == HID_KEY_KEYPAD_EQUAL || usage == HID_KEY_KEYPAD_COMMA;
}

/* What Ctrl makes of a character, as xterm makes it. */
static char HOST_IN_RAM_FUNC vtkey_ctrl(char ch)
{
    unsigned char c = (unsigned char)ch;
    if ((c >= '@' && c < 0x7F) || c == ' ')
        return (char)(c & 0x1F);
    if (c == '2')
        return 0x00;
    if (c >= '3' && c <= '7')
        return (char)(c - '3' + 0x1B);
    if (c == '8' || c == '?')
        return 0x7F;
    if (c == '/')
        return 0x1F;
    return ch;
}

int HOST_IN_RAM_FUNC vtkey_char(char out[VTKEY_MAX], uint8_t usage, char ch, uint8_t flags)
{
    char *p = out;
    if (!ch)
        return 0;
    if (!vtkey_is_keypad(usage))
    {
        if (flags & VTKEY_CTRL)
            ch = vtkey_ctrl(ch);
        if (flags & VTKEY_ALT)
            *p++ = '\33';
    }
    *p++ = ch;
    return (int)(p - out);
}

int HOST_IN_RAM_FUNC vtkey_encode(char out[VTKEY_MAX], uint8_t usage, uint8_t flags)
{
    const term_t *t = &cells_term;
    char *p = out;
    if (vtkey_is_keypad(usage))
    {
        /* Shift is spent on undoing NumLock, so it is not a modifier of
         * what the key then sends. */
        bool numlock = flags & VTKEY_NUMLOCK;
        if (numlock && (flags & VTKEY_SHIFT))
        {
            numlock = false;
            flags &= (uint8_t)~VTKEY_SHIFT;
        }
        if (!numlock && t->app_keypad)
        {
            *p++ = '\33';
            if (t->vt52)
                *p++ = '?';
            else
            {
                *p++ = 'O';
                if (flags & VTKEY_MODIFIERS)
                    *p++ = (char)('1' + (flags & VTKEY_MODIFIERS));
            }
            *p++ = usage == HID_KEY_KEYPAD_EQUAL
                       ? 'X'
                   : usage == HID_KEY_KEYPAD_COMMA
                       ? 'l'
                       : vtkey_keypad_app_keys[usage - HID_KEY_KEYPAD_DIVIDE];
            return (int)(p - out);
        }
        if (usage == HID_KEY_KEYPAD_ENTER)
        {
            usage = HID_KEY_ENTER;
            flags &= (uint8_t)~VTKEY_ALT;
        }
        else if (usage == HID_KEY_KEYPAD_COMMA)
            return vtkey_char(out, usage, ',', flags);
        else if (numlock || usage < HID_KEY_KEYPAD_1 || usage > HID_KEY_KEYPAD_DECIMAL)
            return VTKEY_CHAR;
        else
            usage = vtkey_keypad_nav_keys[usage - HID_KEY_KEYPAD_1];
    }
    if (usage >= HID_KEY_ENTER && usage <= HID_KEY_TAB)
    {
        if (usage == HID_KEY_TAB && (flags & VTKEY_SHIFT))
        {
            *p++ = '\33';
            *p++ = '[';
            *p++ = 'Z';
            return 3;
        }
        if (flags & VTKEY_ALT)
            *p++ = '\33';
        if (usage == HID_KEY_BACKSPACE && (flags & VTKEY_CTRL))
            *p++ = '\b';
        else
            *p++ = vtkey_c0_keys[usage - HID_KEY_ENTER];
        if (usage == HID_KEY_ENTER && t->newline_mode)
            *p++ = '\n';
        return (int)(p - out);
    }
    /* Begin sends ESC [ E, a cursor key in all but name. */
    char intro = '[';
    uint8_t code = 'E';
    if (usage != HID_KEY_KEYPAD_5)
    {
        if (usage < HID_KEY_F1 || usage > HID_KEY_ARROW_UP)
            return VTKEY_CHAR;
        intro = vtkey_keys[usage - HID_KEY_F1].intro;
        code = vtkey_keys[usage - HID_KEY_F1].code;
    }
    if (!intro && !code)
        return 0;
    *p++ = '\33';
    /* A VT52 has no modifiers, and its keys send ESC and a letter. The keys
     * it lacks send what they always do. */
    if (t->vt52 && intro)
    {
        *p++ = (char)code;
        return 2;
    }
    const unsigned mod = t->vt52 ? 1 : 1 + (flags & VTKEY_MODIFIERS);
    if (intro)
    {
        /* ESC O P or ESC [ A alone, ESC [ 1 ; mod A with a modifier. The
         * cursor keys and Home and End are the '[' ones, which DECCKM sends
         * as ESC O A. */
        if (mod == 1)
            *p++ = t->app_cursor ? 'O' : intro;
        else
        {
            *p++ = '[';
            *p++ = '1';
            *p++ = ';';
            p = vtkey_number(p, mod);
        }
        *p++ = (char)code;
    }
    else
    {
        /* ESC [ code ~ alone, ESC [ code ; mod ~ with a modifier. */
        *p++ = '[';
        p = vtkey_number(p, code);
        if (mod != 1)
        {
            *p++ = ';';
            p = vtkey_number(p, mod);
        }
        *p++ = '~';
    }
    return (int)(p - out);
}

bool HOST_IN_RAM_FUNC vtkey_repeats(void)
{
    return cells_term.autorepeat;
}
