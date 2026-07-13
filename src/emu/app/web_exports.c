/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Web (Emscripten) JS-callable bridges. The browser shell (html/index.html)
 * reaches the emulated HID devices through these EMSCRIPTEN_KEEPALIVE exports.
 * Kept in the executable (not emu_core) so the linker can't drop the object
 * before KEEPALIVE marks the symbols; an empty translation unit elsewhere.
 */

#ifdef __EMSCRIPTEN__

#include "emu/hid/mou.h"
#include "emu/hid/pad.h"
#include "emu/hid/tab.h"
#include <emscripten.h>
#include <stdint.h>

/* The shell shows the mouse "click to capture" hint only once a program maps the
 * mouse; capture (pointer lock) and motion scaling ride the shared sokol path. */
EMSCRIPTEN_KEEPALIVE int mou_mapped(void)
{
    return mou_is_mapped() ? 1 : 0;
}

/* Same hint drops once a program maps the tablet (it takes the pointer without
 * capturing it). */
EMSCRIPTEN_KEEPALIVE int tab_mapped(void)
{
    return tab_is_mapped() ? 1 : 0;
}

/* The page's Gamepad-API poller only runs once pad_mapped() reports a program
 * pointed the report block at XRAM, so no gamepad access happens until a ROM asks.
 * pad_report writes one player's decoded state (the page computes the canonical
 * bit layout from the browser's "standard" mapping); pad_disconnect clears one. */
EMSCRIPTEN_KEEPALIVE int pad_mapped(void)
{
    return pad_is_mapped() ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE void pad_report(int player, int dpad, int button0, int button1,
                                     int lx, int ly, int rx, int ry, int lt, int rt, int sony)
{
    pad_host_report(player, (uint8_t)dpad, (uint8_t)button0, (uint8_t)button1,
                    lx, ly, rx, ry, lt, rt, sony != 0);
}

EMSCRIPTEN_KEEPALIVE void pad_disconnect(int player)
{
    pad_connect(player, false);
}

#endif /* __EMSCRIPTEN__ */
