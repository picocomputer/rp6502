/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * No C code calls anything here; the page's index.html reaches the emulated
 * HID devices through these exports. That is why this file is compiled into
 * the executable rather than into emu_core: a static library member whose
 * symbols nothing references is never pulled in, so the object would be gone
 * before EMSCRIPTEN_KEEPALIVE could mark anything in it.
 */

#include "core/hid/mouse.h"
#include "core/hid/gamepad.h"
#include "core/hid/tablet.h"
#include <emscripten.h>
#include <stdint.h>

EMSCRIPTEN_KEEPALIVE int mouse_mapped(void)
{
    return mouse_is_mapped() ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int tablet_mapped(void)
{
    return tablet_is_mapped() ? 1 : 0;
}

/* The page polls the browser's Gamepad API only while this reports true, so it
 * touches no controller until a program has mapped the report block into
 * XRAM. */
EMSCRIPTEN_KEEPALIVE int gamepad_mapped(void)
{
    return gamepad_is_mapped() ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE void gamepad_host(int player, int dpad, int button0, int button1,
                                       int lx, int ly, int rx, int ry, int lt, int rt,
                                   int type, int sticks)
{
    gamepad_connect(player, true, (uint8_t)type, sticks != 0);
    gamepad_host_report(player, (uint8_t)dpad, (uint8_t)button0, (uint8_t)button1,
                    lx, ly, rx, ry, lt, rt);
}

EMSCRIPTEN_KEEPALIVE void gamepad_disconnect(int player)
{
    gamepad_connect(player, false, GAMEPAD_TYPE_UNKNOWN, false);
}
