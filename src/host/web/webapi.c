/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * No C code calls anything here; the exports are for JavaScript, such as
 * page.js. That is why this file is compiled into the executable rather than
 * into emu_core: a static library member whose symbols nothing references is
 * never pulled in, so the object would be gone before EMSCRIPTEN_KEEPALIVE
 * could mark anything in it.
 */

#include "sokol/sokol_app.h"
#include <emscripten.h>
#include <stdint.h>

/* destroy() in page.js stops the sokol frame loop through sapp_quit(),
 * because JavaScript has no handle on that loop. */
EMSCRIPTEN_KEEPALIVE void web_quit(void)
{
    sapp_quit();
}
