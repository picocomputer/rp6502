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
 * has no navigation key. */
static const uint8_t HOST_IN_RAM vtkey_keypad_nav_keys[] = {
    HID_KEY_END, HID_KEY_ARROW_DOWN, HID_KEY_PAGE_DOWN,
    HID_KEY_ARROW_LEFT, HID_KEY_NONE, HID_KEY_ARROW_RIGHT,
    HID_KEY_HOME, HID_KEY_ARROW_UP, HID_KEY_PAGE_UP,
    HID_KEY_INSERT, HID_KEY_DELETE};

/* Keypad Divide to Keypad Decimal, in usage order: the final character of the
 * application keypad's sequence. Keypad Equal, which is apart, is X. */
static const char HOST_IN_RAM vtkey_keypad_app_keys[] = {
    'o', 'j', 'm', 'k', 'M',
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

int HOST_IN_RAM_FUNC vtkey_encode(char out[VTKEY_MAX], uint8_t usage, uint8_t flags)
{
    const term_t *t = &cells_term;
    char *p = out;
    if ((usage >= HID_KEY_KEYPAD_DIVIDE && usage <= HID_KEY_KEYPAD_DECIMAL) ||
        usage == HID_KEY_KEYPAD_EQUAL)
    {
        const bool has_nav = usage >= HID_KEY_KEYPAD_1 && usage <= HID_KEY_KEYPAD_DECIMAL;
        const bool numlock = flags & VTKEY_NUMLOCK;
        if (!numlock && t->app_keypad)
        {
            *p++ = '\33';
            *p++ = t->vt52 ? '?' : 'O';
            *p++ = usage == HID_KEY_KEYPAD_EQUAL
                       ? 'X'
                       : vtkey_keypad_app_keys[usage - HID_KEY_KEYPAD_DIVIDE];
            return 3;
        }
        if (usage == HID_KEY_KEYPAD_ENTER)
            usage = HID_KEY_ENTER;
        else if (!has_nav || (numlock && !(flags & VTKEY_SHIFT)))
            return VTKEY_CHAR;
        else
        {
            /* Shift is spent on undoing NumLock, so it is not a modifier
             * of the navigation key. */
            if (numlock)
                flags &= (uint8_t)~VTKEY_SHIFT;
            usage = vtkey_keypad_nav_keys[usage - HID_KEY_KEYPAD_1];
            if (usage == HID_KEY_NONE)
                return 0;
        }
    }
    if (usage >= HID_KEY_ENTER && usage <= HID_KEY_TAB)
    {
        if ((flags & VTKEY_ALT) && !t->vt52)
            *p++ = '\33';
        if (usage == HID_KEY_BACKSPACE && (flags & VTKEY_CTRL))
            *p++ = '\b';
        else
            *p++ = vtkey_c0_keys[usage - HID_KEY_ENTER];
        if (usage == HID_KEY_ENTER && t->newline_mode)
            *p++ = '\n';
        return (int)(p - out);
    }
    if (usage < HID_KEY_F1 || usage > HID_KEY_ARROW_UP)
        return VTKEY_CHAR;
    const char intro = vtkey_keys[usage - HID_KEY_F1].intro;
    const uint8_t code = vtkey_keys[usage - HID_KEY_F1].code;
    if (!intro && !code)
        return 0;
    *p++ = '\33';
    if (t->vt52)
    {
        /* A VT52 has only the keys with a letter, and no modifiers. */
        if (!intro)
            return 0;
        *p++ = (char)code;
        return 2;
    }
    const unsigned mod = 1 + (flags & (VTKEY_SHIFT | VTKEY_ALT | VTKEY_CTRL | VTKEY_GUI));
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

int HOST_IN_RAM_FUNC vtkey_ctrl(char ch)
{
    unsigned char c = (unsigned char)ch;
    if (!c)
        return -1;
    if ((c >= '@' && c < 0x7F) || c == ' ')
        return c & 0x1F;
    if (c == '2')
        return 0x00;
    if (c >= '3' && c <= '7')
        return c - '3' + 0x1B;
    if (c == '8')
        return 0x7F;
    if (c == '/')
        return 0x1F;
    if (c < 0x20)
        return c;
    return -1;
}

int HOST_IN_RAM_FUNC vtkey_char(char out[VTKEY_MAX], char ch, uint8_t flags)
{
    char *p = out;
    if (!ch)
        return 0;
    if (flags & VTKEY_CTRL)
    {
        int c = vtkey_ctrl(ch);
        if (c >= 0)
            ch = (char)c;
        else if (!(flags & VTKEY_ALT))
            return 0;
    }
    if ((flags & VTKEY_ALT) && !cells_term.vt52)
        *p++ = '\33';
    *p++ = ch;
    return (int)(p - out);
}

bool HOST_IN_RAM_FUNC vtkey_repeats(void)
{
    return cells_term.autorepeat;
}
