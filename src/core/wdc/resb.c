/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/config.h"
#include "core/wdc/bus.h"
#include "core/wdc/phi2.h"
#include "core/wdc/resb.h"
#include "core/wdc/via.h"
#include "core/wdc/cpu.h"

/* While this is set the bus runs no cycles at all, so the 6502 stops and the
 * VIA's timers stop with it. On silicon PHI2 keeps running through a reset and
 * only the 6502 and the 6522 are held. */
static bool held = true;

void resb_init(void)
{
    resb_assert();
}

void resb_assert(void)
{
    held = true;
    cpu_reset();
    via_reset();
    bus_reset();
    /* A ROM may have changed the running rate; a reset takes it back to the
     * configured one. */
    phi2_set_khz_run(phi2_get_khz());
}

void resb_release(void)
{
    held = false;
}

bool resb_running(void)
{
    return !held;
}

void resb_restore(bool down)
{
    held = down;
}
