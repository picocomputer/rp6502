/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The settings the Pocket's own menu owns. The host keeps the values and
 * replays them when the core loads, so this is the load half of
 * load/set/get and nothing writes back.
 *
 * The menu's writes are levels, not events, so this applies whatever
 * differs from what it applied last. A value that arrived while the
 * machine was resetting is not lost.
 */

#include "cfg.h"
#include "font.h"
#include "main.h"
#include "mmio.h"

#include "core/cfg.h"
#include "core/hid/keyboard.h"
#include "core/hid/keymap.h"
#include "core/hid/layout.h"

#include <string.h>

static int32_t cfg_tz;
/* Not zero, which is a value the menu can hold: the first pass has to
 * apply whatever it finds, including a menu that has said nothing. */
static int32_t cfg_kb = -1;

/* One layout, not a list: the menu is two button presses away, so
 * GUI+Space has nothing to cycle between. Zero is a menu that has said
 * nothing and leaves keyboard_init's default standing. */
static void cfg_apply_layout(int32_t kb)
{
    char name[LAYOUT_NAME_MAX];
    if (kb <= 0)
        return;
    layout_name((int)kb - 1, name);
    if (name[0])
        keymap_load_layout(name);
}

void cfg_task(void)
{
    /* Three menu entries make one offset, so the combined number is what
     * to watch. Zero is a real value here, so it applies as-is. */
    int32_t tz = set_tz_minutes();
    if (tz != cfg_tz)
    {
        cfg_tz = tz;
        tim_set_tz_minutes(tz);
    }

    int32_t kb = (int32_t)SET_KB;
    if (kb != cfg_kb)
    {
        cfg_kb = kb;
        cfg_apply_layout(kb);
    }
}

/* The menu owns these settings and writes them itself, so a save from this
 * side has nowhere to go. core/str/str.c calls it. */
void cfg_save(void)
{
}
