/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "ria/api/oem.h"
#include "emu/hid/kbd.h"
#include "emu/sys/mem.h"
#include "emu/sys/com.h"
#include "emu/sys/ria.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* HID keyboard bitmap (xreg_ria_keyboard API)                         */
/* ------------------------------------------------------------------ */

/* 256-bit key bitmap written to XRAM, mirroring ria/hid/kbd.c. Word 0's low
 * bits are reserved: bit 0 = "no keys pressed", bits 1-3 = lock LEDs. */
static uint32_t kbd_keys[8] = {1}; /* idle: no keys down */
static uint16_t kbd_xram = 0xFFFF; /* 0xFFFF = not mapped */
static uint8_t kbd_leds;           /* firmware LED bit order: Num=1, Caps=2, Scroll=4 */

static void kbd_write_xram(void)
{
    kbd_keys[0] &= ~0xFu; /* clear the reserved flag bits */
    bool any = false;
    for (int k = 0; k < 8; k++)
        if (kbd_keys[k])
            any = true;
    if (!any)
        kbd_keys[0] |= 1;
    kbd_keys[0] |= (kbd_leds & 7) << 1;
    if (kbd_xram != 0xFFFF)
        memcpy(&xram[kbd_xram], kbd_keys, sizeof(kbd_keys));
}

bool kbd_set_xram(uint16_t addr)
{
    if (addr != 0xFFFF && addr > 0x10000 - sizeof(kbd_keys))
        return false;
    kbd_xram = addr;
    kbd_write_xram();
    return true;
}

void kbd_hid_set(uint8_t hid_keycode, bool down)
{
    /* Keycodes 0-3 are reserved (none / error rollover); their bits in word 0
     * are the "no keys"/lock flags, so never let a key toggle them. */
    if (hid_keycode < 4)
        return;
    if (down)
        kbd_keys[hid_keycode >> 5] |= 1u << (hid_keycode & 31);
    else
        kbd_keys[hid_keycode >> 5] &= ~(1u << (hid_keycode & 31));
    kbd_write_xram();
}

void kbd_stop(void)
{
    memset(kbd_keys, 0, sizeof(kbd_keys));
    kbd_keys[0] = 1; /* no keys down */
    kbd_xram = 0xFFFF;
}

void kbd_toggle_lock(uint8_t bit)
{
    kbd_leds ^= bit;
    kbd_write_xram();
}

void kbd_text(const char *utf8)
{
    if (!utf8)
        return;
    const char *p = utf8;
    unsigned char oem;
    while ((oem = oem_from_utf8_next(&p)))
        com_kbd_push_byte(oem);
}

/* No dead-key cache in the emulator; conversion happens per keystroke. */
void kbd_rebuild_code_page_cache(void)
{
}

/* A Ctrl+<letter> chord from the host keyboard, promoted to its C0 control byte
 * (Ctrl-A=0x01 .. Ctrl-Z=0x1A). Like the firmware HID driver (ria/hid/kbd.c),
 * SIGINT is latched HERE on Ctrl-C — at the keyboard, before the byte reaches
 * the com stream — so a break is caught even if the input ring is undrained.
 * The byte still flows into the stream (where com.c re-checks it). */
void kbd_ctrl_letter(char letter)
{
    uint8_t c = (uint8_t)letter & 0x1F;
    if (c == 0x03)
        ria_trigger_sigint();
    com_kbd_push_byte(c);
}

/* ESC[1;{mod}{c1} when modified, else the bare ESC{c0}{c1} (e.g. ESC[A for an
 * arrow, ESC O P for F1). Mirrors the firmware kbd_queue_vt100. */
static void kbd_queue_vt100(char c0, char c1, int mod)
{
    char s[16];
    if (mod == 1)
        snprintf(s, sizeof s, "\33%c%c", c0, c1);
    else
        snprintf(s, sizeof s, "\33[1;%d%c", mod, c1);
    com_kbd_push(s, strlen(s));
}

/* VT220 numbered key: ESC[{num}~, or ESC[{num};{mod}~ when modified. */
static void kbd_queue_vt220(int num, int mod)
{
    char s[16];
    if (mod == 1)
        snprintf(s, sizeof s, "\33[%d~", num);
    else
        snprintf(s, sizeof s, "\33[%d;%d~", num, mod);
    com_kbd_push(s, strlen(s));
}

void kbd_key(kbd_key_t key, bool ctrl, bool shift, bool alt)
{
    /* xterm modifier number: 1 + shift + alt*2 + ctrl*4. gui/super is omitted:
     * the WM owns it and the input layer does not forward it. */
    int mod = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
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
