/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The settings the Pocket's own menu owns. There is no config store
 * here — the host keeps the values and replays them when the core
 * loads, which is what a persisted interact variable is for — so this
 * is the load half of load/set/get and nothing writes back.
 *
 * The menu's writes arrive as levels rather than events, so this reads
 * what is currently set and applies whatever differs from what it
 * applied last. A value that changed twice between two visits costs
 * nothing, and one that arrived while the machine was resetting is not
 * lost.
 */

#include "cfg.h"
#include "font.h"
#include "main.h"
#include "mmio.h"

#include "ria/hid/kbd.h"
#include "ria/hid/kbl.h"
#include "ria/sys/cpu.h"

#include <string.h>

static int32_t cfg_tz;
/* Not zero, which is a value the menu can hold: the first pass has to
 * apply whatever it finds, including a menu that has said nothing. */
static int32_t cfg_kb = -1;

/* The menu entry as the layout list kbd.c wants, which here is one
 * layout long. A layout is named by its position in def/kbd.def plus
 * one, so zero is a menu that has said nothing and leaves kbd_init's
 * default standing.
 *
 * One and not two. The RIA carries a list because reaching its monitor
 * to change a setting interrupts what you were doing, and a Pocket's
 * menu is two button presses away — so the layout that would have been
 * the alternate is just the layout, and GUI+Space has nothing to cycle
 * between. */
static void cfg_apply_layout(int32_t kb)
{
    char name[KBL_NAME_MAX];
    if (kb <= 0)
        return;
    kbl_name((int)kb - 1, name);
    if (name[0])
        kbd_load_layout(name);
}

void cfg_task(void)
{
    /* Three menu entries make one offset, so any of them moving is the
     * same event and the combined number is what to watch. The offset's
     * zero is a real value — UTC — so it applies as-is; time.c ignores a
     * write that changes nothing. */
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
