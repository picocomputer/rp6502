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
static int32_t cfg_kb_alt = -1;

/* The two menu entries as the layout list kbd.c wants. A layout is
 * named by its position in def/kbd.def plus one, so zero is a menu that
 * has said nothing — which leaves kbd_init's default standing — and the
 * alternate's zero is its None. Naming the same layout twice would be
 * a list that cycles to itself, so the alternate is dropped when it
 * matches. */
static void cfg_apply_layouts(int32_t kb, int32_t alt)
{
    char list[KBL_NAME_MAX * 2];
    if (kb <= 0)
        return;
    kbl_name((int)kb - 1, list);
    if (!list[0])
        return;
    if (alt > 0 && alt != kb)
    {
        size_t len = strlen(list);
        list[len++] = ' ';
        kbl_name((int)alt - 1, list + len);
    }
    kbd_load_layout(list);
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
    int32_t alt = (int32_t)SET_KB_ALT;
    if (kb != cfg_kb || alt != cfg_kb_alt)
    {
        cfg_kb = kb;
        cfg_kb_alt = alt;
        cfg_apply_layouts(kb, alt);
    }
}
