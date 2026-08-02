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
 *
 * Zero means the menu has never said, which is the state at power-on
 * before the host replays anything: leave the machine's own default
 * alone rather than clamp it to whatever zero would mean.
 */

#include "cfg.h"
#include "font.h"
#include "main.h"
#include "mmio.h"

#include "ria/sys/cpu.h"

static uint32_t cfg_phi2, cfg_cp, cfg_tz;

void cfg_task(void)
{
    uint32_t phi2 = SET_PHI2;
    if (phi2 != cfg_phi2)
    {
        cfg_phi2 = phi2;
        if (phi2 >= CPU_PHI2_MIN_KHZ && phi2 <= CPU_PHI2_MAX_KHZ)
            cpu_set_phi2_khz_run((uint16_t)phi2);
    }
    uint32_t cp = SET_CP;
    if (cp != cfg_cp)
    {
        cfg_cp = cp;
        if (cp)
            font_set_code_page((uint16_t)cp);
    }
    /* The offset's zero is a real value — UTC — so unlike the others
     * it applies as-is; time.c ignores a write that changes nothing. */
    uint32_t tz = SET_TZ;
    if (tz != cfg_tz)
    {
        cfg_tz = tz;
        tim_set_tz_minutes((int32_t)tz);
    }
}
