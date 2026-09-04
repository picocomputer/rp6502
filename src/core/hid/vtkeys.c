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

#include "core/str/oem.h"
#include "core/str/unicode.h"
#include "core/hid/vtkeys.h"
#include "core/hid/usage.h"
#include "core/com/com.h"
#include "core/hid/keyboard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void vtkeys_text(const char *utf8)
{
    if (!utf8)
        return;
    const char *p = utf8;
    unsigned char oem;
    while ((oem = oem_from_utf8_next(&p)))
        com_keyboard_push_byte(oem);
}

void vtkeys_char(uint32_t codepoint)
{
    if (!codepoint)
        return;
    /* Below 0x80 the code page does not get a vote, which is also what the
     * UTF-8 decoder does with a lead byte it can return directly. */
    com_keyboard_push_byte(codepoint < 0x80
                               ? (uint8_t)codepoint
                               : unicode_from_codepoint(codepoint, oem_get_code_page_run()));
}

/* A Ctrl+<letter> chord from the host keyboard, promoted to its C0 control byte
 * (Ctrl-A=0x01 .. Ctrl-Z=0x1A). Ctrl-C latches SIGINT on the way into the ring
 * (com.c scans for it), so a break is caught even if the ring is undrained. */
void vtkeys_ctrl_letter(char letter)
{
    char c = keyboard_ctrl_promote(letter, HID_KEY_NONE);
    if (c)
        com_keyboard_push_byte((uint8_t)c);
}

void vtkeys_alt_char(char ch, bool ctrl)
{
    if (!ch)
        return;
    if (ctrl)
    {
        char c = keyboard_ctrl_promote(ch, HID_KEY_NONE);
        if (c)
            ch = c;
    }
    com_keyboard_push_byte(0x1b);
    com_keyboard_push_byte((uint8_t)ch);
}

/* A key with no character of its own, by HID usage. The four that do have one
 * are answered here because which byte they type is this machine's to say -- the
 * firmware reads them from its layout instead. Everything else is the shared
 * table. False when the usage sends nothing, so a caller can go on to try it
 * as a chord. */
bool vtkeys_key(uint8_t hid_usage, bool ctrl, bool shift, bool alt)
{
    char ch = 0;
    switch (hid_usage)
    {
    case HID_KEY_ENTER:
    case HID_KEY_KEYPAD_ENTER:
        ch = '\r';
        break;
    case HID_KEY_BACKSPACE:
        ch = 0x7f;
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
        if (ctrl)
        {
            char c = keyboard_ctrl_promote(ch, hid_usage);
            if (c)
                ch = c;
        }
        /* Alt prefixes with ESC rather than changing the byte the key types,
         * so it composes with whatever the other modifiers already decided. */
        if (alt)
            com_keyboard_push_byte(0x1b);
        com_keyboard_push_byte((uint8_t)ch);
        return true;
    }
    /* No gui bit: the window manager owns that key on a desktop. */
    char seq[16];
    size_t n = keyboard_vt_seq(seq, sizeof seq, hid_usage,
                      keyboard_vt_mod(shift, alt, ctrl, false));
    if (!n)
        return false;
    com_keyboard_push(seq, n);
    return true;
}

/* ------------------------------------------------------------------ */
/* Typed text (clipboard paste, scripted input)                        */
/* ------------------------------------------------------------------ */

/* Text still being dripped into the keyboard ring (NULL = idle). */
static char *vtkeys_paste_buf;
static size_t vtkeys_paste_len, vtkeys_paste_pos;

/* UTF-8 sequence length from the lead byte (1 for ASCII and invalid leads). */
static size_t vtkeys_utf8_len(uint8_t lead)
{
    if ((lead & 0xe0) == 0xc0)
        return 2;
    if ((lead & 0xf0) == 0xe0)
        return 3;
    if ((lead & 0xf8) == 0xf0)
        return 4;
    return 1;
}

void vtkeys_paste_cancel(void)
{
    free(vtkeys_paste_buf);
    vtkeys_paste_buf = NULL;
}

void vtkeys_paste(const char *utf8)
{
    vtkeys_paste_cancel();
    size_t n = utf8 ? strlen(utf8) : 0;
    if (!n)
        return;
    vtkeys_paste_buf = malloc(n);
    if (!vtkeys_paste_buf)
        return;
    memcpy(vtkeys_paste_buf, utf8, n);
    vtkeys_paste_len = n;
    vtkeys_paste_pos = 0;
}

bool vtkeys_paste_busy(void)
{
    return vtkeys_paste_buf != NULL;
}

void vtkeys_task(void)
{
    if (!vtkeys_paste_buf)
        return;
    /* Leave a quarter of the ring so live typing still fits during a long
     * paste; a full ring drops bytes, which would corrupt the paste. */
    while (vtkeys_paste_pos < vtkeys_paste_len && com_keyboard_free() > COM_RING_SIZE / 4)
    {
        char c = vtkeys_paste_buf[vtkeys_paste_pos];
        if (c == '\r' || c == '\n')
        {
            vtkeys_key(HID_KEY_ENTER, false, false, false);
            vtkeys_paste_pos++;
            if (c == '\r' && vtkeys_paste_pos < vtkeys_paste_len &&
                vtkeys_paste_buf[vtkeys_paste_pos] == '\n')
                vtkeys_paste_pos++; /* CRLF is one Enter */
        }
        else if (c == '\t')
        {
            vtkeys_key(HID_KEY_TAB, false, false, false);
            vtkeys_paste_pos++;
        }
        else if ((uint8_t)c < 32 || c == 127)
        {
            vtkeys_paste_pos++; /* strip other control bytes */
        }
        else
        {
            char seq[5];
            size_t n = vtkeys_utf8_len((uint8_t)c);
            if (n > vtkeys_paste_len - vtkeys_paste_pos)
                n = vtkeys_paste_len - vtkeys_paste_pos;
            memcpy(seq, vtkeys_paste_buf + vtkeys_paste_pos, n);
            seq[n] = '\0';
            vtkeys_text(seq);
            vtkeys_paste_pos += n;
        }
    }
    if (vtkeys_paste_pos >= vtkeys_paste_len)
        vtkeys_paste_cancel();
}

/* core/hid/keymap.h's seam, answered by a machine that had an OS to ask. No
 * desktop host calls hid_report at all -- it sets bits with keyboard_hid_set
 * and pushes text with the door above -- so this is a link-time answer that
 * never runs, not a runtime one that declines. */
void keymap_on_key(uint8_t modifier, uint8_t keycode)
{
    (void)modifier;
    (void)keycode;
}

void keymap_on_modifiers(uint8_t modifier)
{
    (void)modifier;
}
