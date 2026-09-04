/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * RESB on a software machine, where the 6502 only ever runs inside the bus
 * task and there is no second core to race.
 */

#include "core/sys/config.h"
#include "core/wdc/bus.h"
#include "core/wdc/phi2.h"
#include "core/wdc/resb.h"
#include "core/wdc/via.h"
#include "core/wdc/cpu.h"

/* Held is a clock veto here, not a pin: with this set the bus takes no cycles
 * at all, so the VIA's timers and the RIA's registers freeze along with the
 * 6502. On silicon PHI2 runs on through a reset and only the two parts are
 * held. Nothing can tell the difference, because nothing that could look is
 * running either -- it is the same lost-cycle shape as RDY. */
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
    /* A ROM that changed the clock does not get to leave it changed. */
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
