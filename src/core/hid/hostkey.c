/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The firmware turns HID keycodes into characters through its own layout
 * tables, because a Pico has nobody to ask. A desktop OS has done that work
 * before the keystroke arrives, so this file takes the host's answer and
 * leaves the keycodes to core/hid/keyboard.c, which keeps the bitmap a
 * program polls.
 */

#include "core/hid/hostkey.h"
#include "core/hid/keyboard.h"
#include "core/hid/usage.h"
#include "core/hid/vtkey.h"
#include "core/api/proc.h"
#include "core/com/manifold.h"
#include "core/str/oem.h"
#include "core/str/unicode.h"
#include <stdbool.h>

/* A US keyboard's characters, A to slash in usage order, plain and with
 * Shift, and Keypad Divide to Keypad Decimal. */
static const char hostkey_us_plain[] =
    "abcdefghijklmnopqrstuvwxyz1234567890\0\0\0\0 -=[]\\\0;'`,./";
static const char hostkey_us_shift[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\0\0\0\0 _+{}|\0:\"~<>?";
static const char hostkey_us_keypad[] = "/*-+\0" "1234567890.";

char hostkey_us(uint8_t usage, bool shift)
{
    if (usage >= HID_KEY_A && usage <= HID_KEY_SLASH)
        return (shift ? hostkey_us_shift : hostkey_us_plain)[usage - HID_KEY_A];
    if (usage >= HID_KEY_KEYPAD_DIVIDE && usage <= HID_KEY_KEYPAD_DECIMAL)
        return hostkey_us_keypad[usage - HID_KEY_KEYPAD_DIVIDE];
    if (usage == HID_KEY_KEYPAD_EQUAL)
        return '=';
    return 0;
}

uint8_t hostkey_us_key(char ch, bool *shift)
{
    for (uint8_t usage = HID_KEY_A; ch && usage <= HID_KEY_SLASH; usage++)
        for (int with = 0; with < 2; with++)
            if (hostkey_us(usage, with) == ch)
            {
                *shift = with;
                return usage;
            }
    return 0;
}

hostkey_result_t hostkey_press(uint8_t usage, uint8_t flags, bool guess)
{
    const bool ctrl = flags & VTKEY_CTRL;
    const bool alt = flags & VTKEY_ALT;
    if (proc_key_chord(usage, ctrl, alt))
        return HOSTKEY_CHORD;
    char out[VTKEY_MAX];
    int n = vtkey_encode(out, usage, flags);
    if (n == VTKEY_CHAR)
    {
        char ch = 0;
        if (guess)
        {
            /* Caps Lock swaps the case of a letter, and Shift swaps it back. */
            bool shift = flags & VTKEY_SHIFT;
            if (usage <= HID_KEY_Z && (keyboard_get_locks() & KEYBOARD_LED_CAPSLOCK))
                shift = !shift;
            ch = hostkey_us(usage, shift);
        }
        n = vtkey_char(out, usage, ch, flags);
    }
    if (!n)
        return HOSTKEY_NONE;
    manifold_keyboard_push(out, (size_t)n);
    return HOSTKEY_TYPED;
}

void hostkey_char(uint32_t codepoint)
{
    if (!codepoint)
        return;
    /* Below 0x80 the code page has no say. */
    char oem = (char)codepoint;
    if (codepoint >= 0x80)
    {
        /* DEL stands for a character the code page lacks, and the line editor
         * would take it as a backspace. */
        oem = (char)unicode_from_codepoint(codepoint, oem_get_code_page_run());
        if (oem == 0x7F)
            oem = '?';
    }
    manifold_keyboard_push(&oem, 1);
}
