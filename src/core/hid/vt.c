/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See vt.h.
 */

#include "core/hid/vt.h"
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

char vt_ctrl_promote(char ch)
{
    if (ch >= '`' && ch <= '~')
        return (char)(ch - 96);
    if (ch >= '@' && ch <= '_')
        return (char)(ch - 64);
    return 0;
}
