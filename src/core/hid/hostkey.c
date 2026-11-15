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
#include "core/hid/vtkey.h"
#include "core/api/proc.h"
#include "core/com/manifold.h"
#include "core/str/oem.h"
#include "core/str/unicode.h"
#include <stdbool.h>

hostkey_result_t hostkey_press(uint8_t usage, uint8_t flags, char ch)
{
    const bool ctrl = flags & VTKEY_CTRL;
    const bool alt = flags & VTKEY_ALT;
    if (proc_key_chord(usage, ctrl, alt))
        return HOSTKEY_CHORD;
    char out[VTKEY_MAX];
    int n = vtkey_encode(out, usage, flags);
    if (n == VTKEY_CHAR)
        n = vtkey_char(out, usage, ch, flags);
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
