/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "cfg.h"
#include "font.h"
#include "main.h"
#include "mmio.h"

#include "core/hid/keyboard.h"
#include "core/hid/keymap.h"
#include "core/sys/config.h"
#include "core/hid/layout.h"

#include <string.h>

static int32_t cfg_tz;
static int32_t cfg_kb = -1;

static void cfg_apply_layout(int32_t kb)
{
    char name[LAYOUT_NAME_MAX];
    if (kb <= 0)
        return;
    layout_name((int)kb - 1, name);
    if (name[0])
        keymap_set_layout_list(name);
}

void cfg_task(void)
{
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
