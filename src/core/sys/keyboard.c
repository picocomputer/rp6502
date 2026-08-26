/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What a keystroke types, on a machine whose OS already knows.
 *
 * The firmware turns HID keycodes into characters through its own layout
 * tables, because a Pico has nobody to ask. A desktop OS has done that
 * work before the keystroke arrives, so this takes the text and leaves
 * the keycodes to core/hid/keyboard.c, which keeps the bitmap a program polls.
 */

#include "core/api/oem.h"
#include "core/sys/keyboard.h"
#include "core/hid/usage.h"
#include "core/hid/vt.h"
#include "core/com/com.h"
#include "core/hid/keyboard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void keyboard_text(const char *utf8)
{
    if (!utf8)
        return;
    const char *p = utf8;
    unsigned char oem;
    while ((oem = oem_from_utf8_next(&p)))
        com_keyboard_push_byte(oem);
}

/* A Ctrl+<letter> chord from the host keyboard, promoted to its C0 control byte
 * (Ctrl-A=0x01 .. Ctrl-Z=0x1A). Ctrl-C latches SIGINT on the way into the ring
 * (com.c scans for it), so a break is caught even if the ring is undrained. */
void keyboard_ctrl_letter(char letter)
{
    char c = vt_ctrl_promote(letter);
    if (c)
        com_keyboard_push_byte((uint8_t)c);
}

void keyboard_alt_char(char ch, bool ctrl)
{
    if (!ch)
        return;
    if (ctrl)
    {
        char c = vt_ctrl_promote(ch);
        if (c)
            ch = c;
    }
    com_keyboard_push_byte(0x1b);
    com_keyboard_push_byte((uint8_t)ch);
}

/* A key with no character of its own, by HID usage. The four that do have one
 * are answered here because what they spell is this machine's to say -- the
 * firmware reads them from its layout instead. Everything else is the shared
 * table. False when the usage spells nothing, so a caller can go on to try it
 * as a chord. */
bool keyboard_key(uint8_t hid_usage, bool ctrl, bool shift, bool alt)
{
    char ch = 0;
    switch (hid_usage)
    {
    case HID_KEY_ENTER:
    case HID_KEY_KEYPAD_ENTER:
        ch = '\r';
        break;
    case HID_KEY_BACKSPACE:
        ch = ctrl ? '\b' : 0x7f;
        break;
    case HID_KEY_TAB:
        ch = '\t';
        break;
    case HID_KEY_ESCAPE:
        ch = 0x1b;
        break;
    }
    if (ch)
    {
        /* Alt prefixes with ESC rather than changing what the key spells, so it
         * composes with whatever the other modifiers already decided. */
        if (alt)
            com_keyboard_push_byte(0x1b);
        com_keyboard_push_byte((uint8_t)ch);
        return true;
    }
    /* No gui bit: the window manager owns that key on a desktop. */
    char seq[16];
    size_t n = vt_key(seq, sizeof seq, hid_usage,
                      vt_ansi_mod(shift, alt, ctrl, false));
    if (!n)
        return false;
    com_keyboard_push(seq, n);
    return true;
}

/* ------------------------------------------------------------------ */
/* Typed text (clipboard paste, scripted input)                        */
/* ------------------------------------------------------------------ */

/* Text still being dripped into the keyboard ring (NULL = idle). */
static char *keyboard_paste_buf;
static size_t keyboard_paste_len, keyboard_paste_pos;

/* UTF-8 sequence length from the lead byte (1 for ASCII and invalid leads). */
static size_t keyboard_utf8_len(uint8_t lead)
{
    if ((lead & 0xe0) == 0xc0)
        return 2;
    if ((lead & 0xf0) == 0xe0)
        return 3;
    if ((lead & 0xf8) == 0xf0)
        return 4;
    return 1;
}

void keyboard_paste_cancel(void)
{
    free(keyboard_paste_buf);
    keyboard_paste_buf = NULL;
}

void keyboard_paste(const char *utf8)
{
    keyboard_paste_cancel();
    size_t n = utf8 ? strlen(utf8) : 0;
    if (!n)
        return;
    keyboard_paste_buf = malloc(n);
    if (!keyboard_paste_buf)
        return;
    memcpy(keyboard_paste_buf, utf8, n);
    keyboard_paste_len = n;
    keyboard_paste_pos = 0;
}

bool keyboard_paste_busy(void)
{
    return keyboard_paste_buf != NULL;
}

void keyboard_task(void)
{
    if (!keyboard_paste_buf)
        return;
    /* Stay under the ring's headroom so live typing still fits during a long
     * paste; a full ring drops bytes, which would corrupt the paste. */
    while (keyboard_paste_pos < keyboard_paste_len && com_keyboard_free() > 64)
    {
        char c = keyboard_paste_buf[keyboard_paste_pos];
        if (c == '\r' || c == '\n')
        {
            keyboard_key(HID_KEY_ENTER, false, false, false);
            keyboard_paste_pos++;
            if (c == '\r' && keyboard_paste_pos < keyboard_paste_len &&
                keyboard_paste_buf[keyboard_paste_pos] == '\n')
                keyboard_paste_pos++; /* CRLF is one Enter */
        }
        else if (c == '\t')
        {
            keyboard_key(HID_KEY_TAB, false, false, false);
            keyboard_paste_pos++;
        }
        else if ((uint8_t)c < 32 || c == 127)
        {
            keyboard_paste_pos++; /* strip other control bytes */
        }
        else
        {
            char seq[5];
            size_t n = keyboard_utf8_len((uint8_t)c);
            if (n > keyboard_paste_len - keyboard_paste_pos)
                n = keyboard_paste_len - keyboard_paste_pos;
            memcpy(seq, keyboard_paste_buf + keyboard_paste_pos, n);
            seq[n] = '\0';
            keyboard_text(seq);
            keyboard_paste_pos += n;
        }
    }
    if (keyboard_paste_pos >= keyboard_paste_len)
        keyboard_paste_cancel();
}

/* ------------------------------------------------------------------ */
/* Key names                                                           */
/* ------------------------------------------------------------------ */

/* The keys with a name instead of a character. Letters, digits, function keys
 * and the keypad are computed below rather than listed. key is -1 where the key
 * has no xterm sequence of its own. */
static const struct
{
    const char *name;
    uint8_t hid;
} keyboard_named[] = {
    {"enter", 0x28},
    {"escape", 0x29},
    {"backspace", 0x2A},
    {"tab", 0x2B},
    {"up", 0x52},
    {"down", 0x51},
    {"left", 0x50},
    {"right", 0x4F},
    {"home", 0x4A},
    {"end", 0x4D},
    {"insert", 0x49},
    {"delete", 0x4C},
    {"pageup", 0x4B},
    {"pagedown", 0x4E},
    {"kpenter", 0x58},
    {"space", 0x2C},
    {"minus", 0x2D},
    {"equal", 0x2E},
    {"leftbracket", 0x2F},
    {"rightbracket", 0x30},
    {"backslash", 0x31},
    {"semicolon", 0x33},
    {"apostrophe", 0x34},
    {"grave", 0x35},
    {"comma", 0x36},
    {"period", 0x37},
    {"slash", 0x38},
    {"capslock", 0x39},
    {"printscreen", 0x46},
    {"scrolllock", 0x47},
    {"pause", 0x48},
    {"numlock", 0x53},
    {"menu", 0x65},
    {"kpdivide", 0x54},
    {"kpmultiply", 0x55},
    {"kpsubtract", 0x56},
    {"kpadd", 0x57},
    {"kpdecimal", 0x63},
    {"kpequal", 0x67},
    {"lctrl", 0xE0},
    {"lshift", 0xE1},
    {"lalt", 0xE2},
    {"lsuper", 0xE3},
    {"rctrl", 0xE4},
    {"rshift", 0xE5},
    {"ralt", 0xE6},
    {"rsuper", 0xE7},
};

/* "f1".."f12" -> 1..12, else 0. */
static int keyboard_fkey_num(const char *name)
{
    if (name[0] != 'f' && name[0] != 'F')
        return 0;
    int n = 0;
    const char *p = name + 1;
    if (!*p)
        return 0;
    for (; *p; p++)
    {
        if (*p < '0' || *p > '9')
            return 0;
        n = n * 10 + (*p - '0');
    }
    return (n >= 1 && n <= 12) ? n : 0;
}

uint8_t keyboard_hid_from_name(const char *name)
{
    if (!name || !name[0])
        return 0;
    if (!name[1]) /* a bare letter or digit is its own name */
    {
        char c = name[0];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c + 32);
        if (c >= 'a' && c <= 'z')
            return (uint8_t)(0x04 + c - 'a');
        if (c >= '1' && c <= '9')
            return (uint8_t)(0x1E + c - '1');
        if (c == '0')
            return 0x27;
        return 0;
    }
    if ((name[0] == 'k' || name[0] == 'K') && (name[1] == 'p' || name[1] == 'P') &&
        name[2] >= '0' && name[2] <= '9' && !name[3])
        return name[2] == '0' ? 0x62 : (uint8_t)(0x59 + name[2] - '1');
    int f = keyboard_fkey_num(name);
    if (f)
        return (uint8_t)(0x3A + f - 1);
    for (size_t i = 0; i < sizeof keyboard_named / sizeof keyboard_named[0]; i++)
        if (!strcasecmp(name, keyboard_named[i].name))
            return keyboard_named[i].hid;
    return 0;
}

/* core/hid/keyboard.h's seam, answered by a machine that had an OS to ask. Every
 * key still reaches keyboard.c for the bitmap a program reads, but what it spells
 * was decided before the keystroke arrived -- keyboard_text above takes that. */
void keyboard_spell_key(uint8_t modifier, uint8_t keycode)
{
    (void)modifier;
    (void)keycode;
}

void keyboard_spell_modifiers(uint8_t modifier)
{
    (void)modifier;
}
