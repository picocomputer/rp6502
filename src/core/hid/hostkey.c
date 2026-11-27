/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Every machine types a key press here (hostkey_type). The caller gives the
 * key's character: on the Pico from the keyboard's layout tables
 * (core/hid/keymap.c, which puts its dead-key composer where the bytes go,
 * hostkey_set_typer); on a desktop from the OS's text, or a guessed US key
 * for a chord the OS gives no text for (hostkey_guess). The keycodes go to
 * core/hid/keyboard.c, which keeps the bitmap a program polls.
 */

#include "core/hid/hostkey.h"
#include "core/hid/keyboard.h"
#include "core/hid/usage.h"
#include "core/hid/vtkey.h"
#include "core/hid/key.h"
#include "core/hid/typed.h"
#include "core/str/oem.h"
#include "core/str/unicode.h"
#include "core/sys/sst.h"
#include "machine.h"
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

/* Where typed bytes go: the typed queue, unless the machine's own keyboard
 * driver composes them first (keymap.c, its dead keys). */
static const hostkey_typer_t hostkey_queue = {typed_push, typed_cut};
static const hostkey_typer_t *hostkey_typer = &hostkey_queue;

#define HOSTKEY_SST(X)                                              \
    X(SKIP, hostkey_queue, "constant")                              \
    X(SKIP, hostkey_typer, "which driver types, set as it starts")
SST_TABLE(hostkey, HOSTKEY_SST)

void hostkey_set_typer(const hostkey_typer_t *typer)
{
    hostkey_typer = typer ? typer : &hostkey_queue;
}

char hostkey_guess(uint8_t usage, uint8_t modifier, bool capslock)
{
    /* Caps Lock swaps the case of a letter, and Shift swaps it back. */
    bool shift = modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);
    if (usage <= HID_KEY_Z && capslock)
        shift = !shift;
    return hostkey_us(usage, shift);
}

hostkey_result_t HOST_IN_RAM_FUNC hostkey_type(uint8_t usage, uint8_t modifier, bool repeat,
                                               char ch, bool meta)
{
    const key_chord_t chord = key_chord(usage, modifier, repeat);
    if (chord == KEY_CHORD_ACTED)
        return HOSTKEY_CHORD;
    uint8_t flags = vtkey_modifiers(modifier);
    char out[VTKEY_MAX];
    int n = vtkey_encode(out, usage, flags);
    if (n == VTKEY_CHAR)
    {
        /* Alt with the key's character is Meta, ESC before it, only where
         * the caller says so; otherwise Alt chose the character. */
        if (!meta)
            flags &= (uint8_t)~VTKEY_ALT;
        n = vtkey_char(out, usage, ch, flags);
    }
    if (chord == KEY_CHORD_CUT)
    {
        /* A Mac's Cmd is no modifier a terminal encodes, so its cut key
         * types nothing, whatever the key alone would. */
        const bool cmd = modifier & (KEYBOARD_MODIFIER_LEFTGUI | KEYBOARD_MODIFIER_RIGHTGUI);
        hostkey_typer->cut(out, cmd ? 0 : (size_t)n);
        return HOSTKEY_CHORD;
    }
    if (!n)
        return HOSTKEY_NONE;
    hostkey_typer->type(out, (size_t)n);
    return HOSTKEY_TYPED;
}

hostkey_result_t hostkey_press(uint8_t usage, uint8_t modifier, bool repeat, bool guess)
{
    const char ch = guess ? hostkey_guess(usage, modifier,
                                          keyboard_get_locks() & KEYBOARD_LED_CAPSLOCK)
                          : 0;
    return hostkey_type(usage, modifier, repeat, ch, true);
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
    typed_push(&oem, 1);
}
