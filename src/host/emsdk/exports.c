/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Web (Emscripten) JS-callable bridges. The browser shell (html/index.html)
 * reaches the emulated HID devices through these EMSCRIPTEN_KEEPALIVE exports.
 * Kept in the executable (not emu_core) so the linker can't drop the object
 * before KEEPALIVE marks the symbols. Compiled only for the Emscripten host.
 */

#include "core/hid/mouse.h"
#include "core/hid/gamepad.h"
#include "core/hid/tablet.h"
#include <emscripten.h>
#include <stdint.h>

/* The shell shows the mouse "click to capture" hint only once a program maps the
 * mouse; capture (pointer lock) and motion scaling ride the shared sokol path. */
EMSCRIPTEN_KEEPALIVE int mouse_mapped(void)
{
    return mouse_is_mapped() ? 1 : 0;
}

/* Same hint drops once a program maps the tablet (it takes the pointer without
 * capturing it). */
EMSCRIPTEN_KEEPALIVE int tablet_mapped(void)
{
    return tablet_is_mapped() ? 1 : 0;
}

/* The page's Gamepad-API poller only runs once gamepad_mapped() reports a program
 * pointed the report block at XRAM, so no gamepad access happens until a ROM asks.
 * gamepad_host writes one player's decoded state (the page computes the canonical
 * bit layout from the browser's "standard" mapping); gamepad_disconnect clears one. */
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
