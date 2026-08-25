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
 * the keycodes to core/hid/kbd.c, which keeps the bitmap a program polls.
 */

#include "core/api/oem.h"
#include "core/sys/kbd.h"
#include "core/hid/vt.h"
#include "core/sys/com.h"
#include "core/hid/kbd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void kbd_text(const char *utf8)
{
    if (!utf8)
        return;
    const char *p = utf8;
    unsigned char oem;
    while ((oem = oem_from_utf8_next(&p)))
        com_kbd_push_byte(oem);
}

/* A Ctrl+<letter> chord from the host keyboard, promoted to its C0 control byte
 * (Ctrl-A=0x01 .. Ctrl-Z=0x1A). Ctrl-C latches SIGINT on the way into the ring
 * (com.c scans for it), so a break is caught even if the ring is undrained. */
void kbd_ctrl_letter(char letter)
{
    char c = vt_ctrl_promote(letter);
    if (c)
        com_kbd_push_byte((uint8_t)c);
}

void kbd_alt_char(char ch, bool ctrl)
{
    if (!ch)
        return;
    if (ctrl)
    {
        char c = vt_ctrl_promote(ch);
        if (c)
            ch = c;
    }
    com_kbd_push_byte(0x1b);
    com_kbd_push_byte((uint8_t)ch);
}

static void kbd_queue_vt100(char c0, char c1, int mod)
{
    char s[16];
    com_kbd_push(s, vt_vt100(s, sizeof s, c0, c1, mod));
}

static void kbd_queue_vt220(int num, int mod)
{
    char s[16];
    com_kbd_push(s, vt_vt220(s, sizeof s, num, mod));
}

void kbd_key(kbd_key_t key, bool ctrl, bool shift, bool alt)
{
    /* xterm modifier number: 1 + shift + alt*2 + ctrl*4. gui/super is omitted:
     * the WM owns it and the input layer does not forward it. */
    /* No gui bit: the window manager owns that key on a desktop. */
    int mod = vt_ansi_mod(shift, alt, ctrl, false);
    switch (key)
    {
    case KBD_KEY_ENTER:
        return com_kbd_push("\r", 1);
    case KBD_KEY_BACKSPACE:
        return com_kbd_push(ctrl ? "\x08" : "\x7f", 1);
    case KBD_KEY_TAB:
        return com_kbd_push("\t", 1);
    case KBD_KEY_ESCAPE:
        return com_kbd_push("\x1b", 1);
    case KBD_KEY_UP:
        return kbd_queue_vt100('[', 'A', mod);
    case KBD_KEY_DOWN:
        return kbd_queue_vt100('[', 'B', mod);
    case KBD_KEY_RIGHT:
        return kbd_queue_vt100('[', 'C', mod);
    case KBD_KEY_LEFT:
        return kbd_queue_vt100('[', 'D', mod);
    case KBD_KEY_HOME:
        return kbd_queue_vt100('[', 'H', mod);
    case KBD_KEY_END:
        return kbd_queue_vt100('[', 'F', mod);
    case KBD_KEY_INSERT:
        return kbd_queue_vt220(2, mod);
    case KBD_KEY_DELETE:
        return kbd_queue_vt220(3, mod);
    case KBD_KEY_PAGE_UP:
        return kbd_queue_vt220(5, mod);
    case KBD_KEY_PAGE_DOWN:
        return kbd_queue_vt220(6, mod);
    case KBD_KEY_F1:
        return kbd_queue_vt100('O', 'P', mod);
    case KBD_KEY_F2:
        return kbd_queue_vt100('O', 'Q', mod);
    case KBD_KEY_F3:
        return kbd_queue_vt100('O', 'R', mod);
    case KBD_KEY_F4:
        return kbd_queue_vt100('O', 'S', mod);
    case KBD_KEY_F5:
        return kbd_queue_vt220(15, mod);
    case KBD_KEY_F6:
        return kbd_queue_vt220(17, mod);
    case KBD_KEY_F7:
        return kbd_queue_vt220(18, mod);
    case KBD_KEY_F8:
        return kbd_queue_vt220(19, mod);
    case KBD_KEY_F9:
        return kbd_queue_vt220(20, mod);
    case KBD_KEY_F10:
        return kbd_queue_vt220(21, mod);
    case KBD_KEY_F11:
        return kbd_queue_vt220(23, mod);
    case KBD_KEY_F12:
        return kbd_queue_vt220(24, mod);
    }
}

/* ------------------------------------------------------------------ */
/* Typed text (clipboard paste, scripted input)                        */
/* ------------------------------------------------------------------ */

/* Text still being dripped into the keyboard ring (NULL = idle). */
static char *kbd_paste_buf;
static size_t kbd_paste_len, kbd_paste_pos;

/* UTF-8 sequence length from the lead byte (1 for ASCII and invalid leads). */
static size_t kbd_utf8_len(uint8_t lead)
{
    if ((lead & 0xe0) == 0xc0)
        return 2;
    if ((lead & 0xf0) == 0xe0)
        return 3;
    if ((lead & 0xf8) == 0xf0)
        return 4;
    return 1;
}

void kbd_paste_cancel(void)
{
    free(kbd_paste_buf);
    kbd_paste_buf = NULL;
}

void kbd_paste(const char *utf8)
{
    kbd_paste_cancel();
    size_t n = utf8 ? strlen(utf8) : 0;
    if (!n)
        return;
    kbd_paste_buf = malloc(n);
    if (!kbd_paste_buf)
        return;
    memcpy(kbd_paste_buf, utf8, n);
    kbd_paste_len = n;
    kbd_paste_pos = 0;
}

bool kbd_paste_busy(void)
{
    return kbd_paste_buf != NULL;
}

void kbd_task(void)
{
    if (!kbd_paste_buf)
        return;
    /* Stay under the ring's headroom so live typing still fits during a long
     * paste; a full ring drops bytes, which would corrupt the paste. */
    while (kbd_paste_pos < kbd_paste_len && com_kbd_free() > 64)
    {
        char c = kbd_paste_buf[kbd_paste_pos];
        if (c == '\r' || c == '\n')
        {
            kbd_key(KBD_KEY_ENTER, false, false, false);
            kbd_paste_pos++;
            if (c == '\r' && kbd_paste_pos < kbd_paste_len &&
                kbd_paste_buf[kbd_paste_pos] == '\n')
                kbd_paste_pos++; /* CRLF is one Enter */
        }
        else if (c == '\t')
        {
            kbd_key(KBD_KEY_TAB, false, false, false);
            kbd_paste_pos++;
        }
        else if ((uint8_t)c < 32 || c == 127)
        {
            kbd_paste_pos++; /* strip other control bytes */
        }
        else
        {
            char seq[5];
            size_t n = kbd_utf8_len((uint8_t)c);
            if (n > kbd_paste_len - kbd_paste_pos)
                n = kbd_paste_len - kbd_paste_pos;
            memcpy(seq, kbd_paste_buf + kbd_paste_pos, n);
            seq[n] = '\0';
            kbd_text(seq);
            kbd_paste_pos += n;
        }
    }
    if (kbd_paste_pos >= kbd_paste_len)
        kbd_paste_cancel();
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
    int8_t key;
} kbd_named[] = {
    {"enter", 0x28, KBD_KEY_ENTER},
    {"escape", 0x29, KBD_KEY_ESCAPE},
    {"backspace", 0x2A, KBD_KEY_BACKSPACE},
    {"tab", 0x2B, KBD_KEY_TAB},
    {"up", 0x52, KBD_KEY_UP},
    {"down", 0x51, KBD_KEY_DOWN},
    {"left", 0x50, KBD_KEY_LEFT},
    {"right", 0x4F, KBD_KEY_RIGHT},
    {"home", 0x4A, KBD_KEY_HOME},
    {"end", 0x4D, KBD_KEY_END},
    {"insert", 0x49, KBD_KEY_INSERT},
    {"delete", 0x4C, KBD_KEY_DELETE},
    {"pageup", 0x4B, KBD_KEY_PAGE_UP},
    {"pagedown", 0x4E, KBD_KEY_PAGE_DOWN},
    {"kpenter", 0x58, KBD_KEY_ENTER},
    {"space", 0x2C, -1},
    {"minus", 0x2D, -1},
    {"equal", 0x2E, -1},
    {"leftbracket", 0x2F, -1},
    {"rightbracket", 0x30, -1},
    {"backslash", 0x31, -1},
    {"semicolon", 0x33, -1},
    {"apostrophe", 0x34, -1},
    {"grave", 0x35, -1},
    {"comma", 0x36, -1},
    {"period", 0x37, -1},
    {"slash", 0x38, -1},
    {"capslock", 0x39, -1},
    {"printscreen", 0x46, -1},
    {"scrolllock", 0x47, -1},
    {"pause", 0x48, -1},
    {"numlock", 0x53, -1},
    {"menu", 0x65, -1},
    {"kpdivide", 0x54, -1},
    {"kpmultiply", 0x55, -1},
    {"kpsubtract", 0x56, -1},
    {"kpadd", 0x57, -1},
    {"kpdecimal", 0x63, -1},
    {"kpequal", 0x67, -1},
    {"lctrl", 0xE0, -1},
    {"lshift", 0xE1, -1},
    {"lalt", 0xE2, -1},
    {"lsuper", 0xE3, -1},
    {"rctrl", 0xE4, -1},
    {"rshift", 0xE5, -1},
    {"ralt", 0xE6, -1},
    {"rsuper", 0xE7, -1},
};

/* "f1".."f12" -> 1..12, else 0. */
static int kbd_fkey_num(const char *name)
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

uint8_t kbd_hid_from_name(const char *name)
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
    int f = kbd_fkey_num(name);
    if (f)
        return (uint8_t)(0x3A + f - 1);
    for (size_t i = 0; i < sizeof kbd_named / sizeof kbd_named[0]; i++)
        if (!strcasecmp(name, kbd_named[i].name))
            return kbd_named[i].hid;
    return 0;
}

bool kbd_key_from_name(const char *name, kbd_key_t *key)
{
    if (!name || !name[0])
        return false;
    int f = kbd_fkey_num(name);
    if (f)
    {
        *key = (kbd_key_t)(KBD_KEY_F1 + f - 1);
        return true;
    }
    for (size_t i = 0; i < sizeof kbd_named / sizeof kbd_named[0]; i++)
        if (kbd_named[i].key >= 0 && !strcasecmp(name, kbd_named[i].name))
        {
            *key = (kbd_key_t)kbd_named[i].key;
            return true;
        }
    return false;
}
