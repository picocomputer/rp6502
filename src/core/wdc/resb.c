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

void resb_init(void)
{
    resb_assert();
}

void resb_assert(void)
{
    cpu_reset();
    via_reset();
    bus_reset();
    /* A ROM may have changed the running rate; a reset takes it back to the
     * configured one. */
    phi2_set_khz_run(phi2_get_khz());
}

/* Nothing is done here, because the bus steps the modelled 6502 only while
 * sys_running() and the reset sequence runs on the first step. While the
 * machine is stopped the bus runs no cycles at all, so the VIA's timers stop
 * with the 6502; on silicon PHI2 keeps running through a reset and only the
 * 6502 and the 6522 are held. */
void resb_release(void)
{
}
