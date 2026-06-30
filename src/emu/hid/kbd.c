/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Keyboard input — the emulator's stand-in for the firmware USB HID driver
 * (ria/hid/kbd.c). That driver turns HID scancodes into a terminal byte
 * stream on the keyboard com source; this owns the whole translation from host
 * key events to that stream so the windowing layer (app_sokol.c) carries no
 * keyboard knowledge. The line editor (rln.c) reads the result as COM_SOURCE_KBD.
 *
 * kbd_event() is the one entry the window forwards every key/char event to: it
 * feeds the HID bitmap, maps printable input through UTF-8 to the active OEM
 * code page (the firmware works in OEM bytes, not Unicode), and turns editing,
 * navigation and function keys into the same xterm/VT byte sequences the
 * firmware emits — arrows, F1-F12, Home/End, Insert/Delete, PageUp/Down —
 * including the ESC[1;{mod} modifier annotations. Only sokol's event TYPES are
 * used here, no sapp_* calls, so the headless emu_core still links.
 *
 * Tests drive the lower-level kbd_text()/kbd_key()/kbd_hid_set() directly.
 */

#include "emu/api/oem.h"
#include "emu/hid/kbd.h"
#include "emu/sys/mem.h"
#include "emu/sys/ria.h"
#include "sys/com.h"
#include "sys/ria.h"
#include "sokol_app.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* HID keyboard bitmap (xreg_ria_keyboard API)                         */
/* ------------------------------------------------------------------ */

/* 256-bit key bitmap written to XRAM, mirroring ria/hid/kbd.c. Word 0's low
 * bits are reserved: bit 0 = "no keys pressed", bits 1-3 = lock LEDs. */
static uint32_t kbd_keys[8] = {1}; /* idle: no keys down */
static uint16_t kbd_xram = 0xFFFF; /* 0xFFFF = not mapped */

static void kbd_write_xram(void)
{
    kbd_keys[0] &= ~0xFu; /* clear the reserved flag bits */
    bool any = false;
    for (int k = 0; k < 8; k++)
        if (kbd_keys[k])
            any = true;
    if (!any)
        kbd_keys[0] |= 1; /* no keys down */
    /* lock LEDs (bits 1-3) are not modeled */
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

void kbd_reset(void)
{
    memset(kbd_keys, 0, sizeof(kbd_keys));
    kbd_keys[0] = 1; /* no keys down */
    kbd_xram = 0xFFFF;
}

void kbd_text(const char *utf8)
{
    if (!utf8)
        return;
    const char *p = utf8;
    unsigned char oem;
    while ((oem = oem_utf8_to_oem(&p)))
        com_kbd_push_byte(oem);
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
     * the WM owns it and app_sokol does not forward it. */
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

/* ------------------------------------------------------------------ */
/* Host (sokol) key/char event translation                            */
/* ------------------------------------------------------------------ */

/* Map a sokol keycode to a USB HID keyboard usage id for the xreg keyboard
 * bitmap. 0 = unmapped (not reported). */
static uint8_t sokol_to_hid(int kc)
{
    if (kc >= SAPP_KEYCODE_A && kc <= SAPP_KEYCODE_Z)
        return (uint8_t)(0x04 + (kc - SAPP_KEYCODE_A));
    if (kc >= SAPP_KEYCODE_1 && kc <= SAPP_KEYCODE_9)
        return (uint8_t)(0x1E + (kc - SAPP_KEYCODE_1));
    if (kc >= SAPP_KEYCODE_F1 && kc <= SAPP_KEYCODE_F12)
        return (uint8_t)(0x3A + (kc - SAPP_KEYCODE_F1));
    if (kc >= SAPP_KEYCODE_KP_1 && kc <= SAPP_KEYCODE_KP_9)
        return (uint8_t)(0x59 + (kc - SAPP_KEYCODE_KP_1));
    switch (kc)
    {
    case SAPP_KEYCODE_KP_0: return 0x62;
    case SAPP_KEYCODE_KP_DECIMAL: return 0x63;
    case SAPP_KEYCODE_KP_DIVIDE: return 0x54;
    case SAPP_KEYCODE_KP_MULTIPLY: return 0x55;
    case SAPP_KEYCODE_KP_SUBTRACT: return 0x56;
    case SAPP_KEYCODE_KP_ADD: return 0x57;
    case SAPP_KEYCODE_KP_EQUAL: return 0x67;
    case SAPP_KEYCODE_CAPS_LOCK: return 0x39;
    case SAPP_KEYCODE_SCROLL_LOCK: return 0x47;
    case SAPP_KEYCODE_NUM_LOCK: return 0x53;
    case SAPP_KEYCODE_PRINT_SCREEN: return 0x46;
    case SAPP_KEYCODE_PAUSE: return 0x48;
    case SAPP_KEYCODE_MENU: return 0x65;
    case SAPP_KEYCODE_LEFT_CONTROL: return 0xE0;
    case SAPP_KEYCODE_LEFT_SHIFT: return 0xE1;
    case SAPP_KEYCODE_LEFT_ALT: return 0xE2;
    case SAPP_KEYCODE_LEFT_SUPER: return 0xE3;
    case SAPP_KEYCODE_RIGHT_CONTROL: return 0xE4;
    case SAPP_KEYCODE_RIGHT_SHIFT: return 0xE5;
    case SAPP_KEYCODE_RIGHT_ALT: return 0xE6;
    case SAPP_KEYCODE_RIGHT_SUPER: return 0xE7;
    case SAPP_KEYCODE_0: return 0x27;
    case SAPP_KEYCODE_ENTER: return 0x28;
    case SAPP_KEYCODE_KP_ENTER: return 0x58;
    case SAPP_KEYCODE_ESCAPE: return 0x29;
    case SAPP_KEYCODE_BACKSPACE: return 0x2A;
    case SAPP_KEYCODE_TAB: return 0x2B;
    case SAPP_KEYCODE_SPACE: return 0x2C;
    case SAPP_KEYCODE_MINUS: return 0x2D;
    case SAPP_KEYCODE_EQUAL: return 0x2E;
    case SAPP_KEYCODE_LEFT_BRACKET: return 0x2F;
    case SAPP_KEYCODE_RIGHT_BRACKET: return 0x30;
    case SAPP_KEYCODE_BACKSLASH: return 0x31;
    case SAPP_KEYCODE_SEMICOLON: return 0x33;
    case SAPP_KEYCODE_APOSTROPHE: return 0x34;
    case SAPP_KEYCODE_GRAVE_ACCENT: return 0x35;
    case SAPP_KEYCODE_COMMA: return 0x36;
    case SAPP_KEYCODE_PERIOD: return 0x37;
    case SAPP_KEYCODE_SLASH: return 0x38;
    case SAPP_KEYCODE_RIGHT: return 0x4F;
    case SAPP_KEYCODE_LEFT: return 0x50;
    case SAPP_KEYCODE_DOWN: return 0x51;
    case SAPP_KEYCODE_UP: return 0x52;
    case SAPP_KEYCODE_DELETE: return 0x4C;
    case SAPP_KEYCODE_HOME: return 0x4A;
    case SAPP_KEYCODE_END: return 0x4D;
    default: return 0;
    }
}

/* Encode one Unicode codepoint to a NUL-terminated UTF-8 string (kbd_text then
 * maps it to the active OEM code page). */
static const char *utf8_encode(uint32_t cp, char dst[5])
{
    int n = 0;
    if (cp < 0x80)
        dst[n++] = (char)cp;
    else if (cp < 0x800)
    {
        dst[n++] = (char)(0xC0 | (cp >> 6));
        dst[n++] = (char)(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        dst[n++] = (char)(0xE0 | (cp >> 12));
        dst[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[n++] = (char)(0x80 | (cp & 0x3F));
    }
    else
    {
        dst[n++] = (char)(0xF0 | (cp >> 18));
        dst[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        dst[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[n++] = (char)(0x80 | (cp & 0x3F));
    }
    dst[n] = 0;
    return dst;
}

/* The one entry app_sokol forwards every key/char event to. Feeds the HID
 * bitmap on press/release, emits printable CHARs as OEM bytes, and turns the
 * navigation/function/ctrl keys into their byte sequences. (Esc-releases-mouse
 * is a capture concern the window handles before forwarding here.) */
void kbd_event(const sapp_event *e)
{
    if (e->type == SAPP_EVENTTYPE_KEY_DOWN || e->type == SAPP_EVENTTYPE_KEY_UP)
    {
        uint8_t hid = sokol_to_hid(e->key_code);
        if (hid)
            kbd_hid_set(hid, e->type == SAPP_EVENTTYPE_KEY_DOWN);
    }
    switch (e->type)
    {
    case SAPP_EVENTTYPE_CHAR:
        /* Printable input only; control codes (<32) and DEL arrive via KEY_DOWN
         * below, so skip them here to avoid double injection. */
        if (e->char_code >= 32 && e->char_code != 127)
        {
            char u[5];
            kbd_text(utf8_encode(e->char_code, u));
        }
        break;
    case SAPP_EVENTTYPE_KEY_DOWN:
    {
        bool ctrl = (e->modifiers & SAPP_MODIFIER_CTRL) != 0;
        bool shift = (e->modifiers & SAPP_MODIFIER_SHIFT) != 0;
        bool alt = (e->modifiers & SAPP_MODIFIER_ALT) != 0;
        switch (e->key_code)
        {
        case SAPP_KEYCODE_ESCAPE: kbd_key(KBD_KEY_ESCAPE, ctrl, shift, alt); break;
        case SAPP_KEYCODE_ENTER:
        case SAPP_KEYCODE_KP_ENTER: kbd_key(KBD_KEY_ENTER, ctrl, shift, alt); break;
        case SAPP_KEYCODE_BACKSPACE: kbd_key(KBD_KEY_BACKSPACE, ctrl, shift, alt); break;
        case SAPP_KEYCODE_TAB: kbd_key(KBD_KEY_TAB, ctrl, shift, alt); break;
        case SAPP_KEYCODE_UP: kbd_key(KBD_KEY_UP, ctrl, shift, alt); break;
        case SAPP_KEYCODE_DOWN: kbd_key(KBD_KEY_DOWN, ctrl, shift, alt); break;
        case SAPP_KEYCODE_LEFT: kbd_key(KBD_KEY_LEFT, ctrl, shift, alt); break;
        case SAPP_KEYCODE_RIGHT: kbd_key(KBD_KEY_RIGHT, ctrl, shift, alt); break;
        case SAPP_KEYCODE_HOME: kbd_key(KBD_KEY_HOME, ctrl, shift, alt); break;
        case SAPP_KEYCODE_END: kbd_key(KBD_KEY_END, ctrl, shift, alt); break;
        case SAPP_KEYCODE_DELETE: kbd_key(KBD_KEY_DELETE, ctrl, shift, alt); break;
        case SAPP_KEYCODE_INSERT: kbd_key(KBD_KEY_INSERT, ctrl, shift, alt); break;
        case SAPP_KEYCODE_PAGE_UP: kbd_key(KBD_KEY_PAGE_UP, ctrl, shift, alt); break;
        case SAPP_KEYCODE_PAGE_DOWN: kbd_key(KBD_KEY_PAGE_DOWN, ctrl, shift, alt); break;
        case SAPP_KEYCODE_F1: kbd_key(KBD_KEY_F1, ctrl, shift, alt); break;
        case SAPP_KEYCODE_F2: kbd_key(KBD_KEY_F2, ctrl, shift, alt); break;
        case SAPP_KEYCODE_F3: kbd_key(KBD_KEY_F3, ctrl, shift, alt); break;
        case SAPP_KEYCODE_F4: kbd_key(KBD_KEY_F4, ctrl, shift, alt); break;
        case SAPP_KEYCODE_F5: kbd_key(KBD_KEY_F5, ctrl, shift, alt); break;
        case SAPP_KEYCODE_F6: kbd_key(KBD_KEY_F6, ctrl, shift, alt); break;
        case SAPP_KEYCODE_F7: kbd_key(KBD_KEY_F7, ctrl, shift, alt); break;
        case SAPP_KEYCODE_F8: kbd_key(KBD_KEY_F8, ctrl, shift, alt); break;
        case SAPP_KEYCODE_F9: kbd_key(KBD_KEY_F9, ctrl, shift, alt); break;
        case SAPP_KEYCODE_F10: kbd_key(KBD_KEY_F10, ctrl, shift, alt); break;
        case SAPP_KEYCODE_F11: kbd_key(KBD_KEY_F11, ctrl, shift, alt); break;
        case SAPP_KEYCODE_F12: kbd_key(KBD_KEY_F12, ctrl, shift, alt); break;
        default:
            /* Ctrl+<letter> -> C0 control byte (Ctrl-C latches SIGINT). No CHAR
             * event fires for these, so promote here. */
            if (ctrl && !alt && e->key_code >= SAPP_KEYCODE_A && e->key_code <= SAPP_KEYCODE_Z)
                kbd_ctrl_letter((char)('A' + (e->key_code - SAPP_KEYCODE_A)));
            break;
        }
        break;
    }
    default:
        break;
    }
}
