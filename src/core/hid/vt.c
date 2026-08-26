/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See vt.h.
 */

#include "core/hid/vt.h"
#include "core/hid/usage.h"
#include <stdio.h>

int vt_ansi_mod(bool shift, bool alt, bool ctrl, bool gui)
{
    return 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0) + (gui ? 8 : 0);
}

size_t vt_vt100(char *out, size_t cap, char c0, char c1, int ansi_mod)
{
    if (ansi_mod == 1)
        return (size_t)snprintf(out, cap, "\33%c%c", c0, c1);
    return (size_t)snprintf(out, cap, "\33[1;%d%c", ansi_mod, c1);
}

size_t vt_vt220(char *out, size_t cap, int num, int ansi_mod)
{
    if (ansi_mod == 1)
        return (size_t)snprintf(out, cap, "\33[%d~", num);
    return (size_t)snprintf(out, cap, "\33[%d;%d~", num, ansi_mod);
}

/* Two contiguous HID runs, so the table is an index rather than a search.
 * intro 'O' or '[' is the VT100 form and code is its final character; intro 0
 * is the VT220 form and code is its number. */
static const struct
{
    char intro;
    uint8_t code;
} vt_keys[] = {
    /* 0x3A..0x45, F1 to F12 */
    {'O', 'P'}, {'O', 'Q'}, {'O', 'R'}, {'O', 'S'},
    {0, 15}, {0, 17}, {0, 18}, {0, 19},
    {0, 20}, {0, 21}, {0, 23}, {0, 24},
    /* 0x46..0x48, PrintScreen, ScrollLock and Pause spell nothing */
    {0, 0}, {0, 0}, {0, 0},
    /* 0x49..0x52, Insert to Up */
    {0, 2}, {'[', 'H'}, {0, 5}, {0, 3}, {'[', 'F'},
    {0, 6}, {'[', 'C'}, {'[', 'D'}, {'[', 'B'}, {'[', 'A'}};

size_t vt_key(char *out, size_t cap, uint8_t hid_usage, int ansi_mod)
{
    if (hid_usage < 0x3A || hid_usage > 0x52)
        return 0;
    const char intro = vt_keys[hid_usage - 0x3A].intro;
    const uint8_t code = vt_keys[hid_usage - 0x3A].code;
    if (intro)
        return vt_vt100(out, cap, intro, (char)code, ansi_mod);
    if (code)
        return vt_vt220(out, cap, code, ansi_mod);
    return 0;
}

char vt_ctrl_promote(char ch)
{
    if (ch >= '`' && ch <= '~')
        return (char)(ch - 96);
    if (ch >= '@' && ch <= '_')
        return (char)(ch - 64);
    return 0;
}
