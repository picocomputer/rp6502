/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * RESB in the fabric, which is one register bit. The 6502 and the 6522 both
 * take it as their reset, so nothing else here has to reach them.
 */

#include "core/sys/config.h"
#include "core/wdc/resb.h"
#include "mmio.h"

/* No reset reaches this register, so the firmware is what holds the 6502
 * after a host reset. */
void resb_init(void)
{
    resb_assert();
}

void resb_assert(void)
{
    CPU_RESB = 0;
    /* A ROM that changed the clock does not get to leave it changed. */
    MMIO_PHI2 = phi2_get_khz();
}

/* The 6502 is released against a state the firmware chose, not whatever the
 * cells held. The vectors were written by the loader before this. */
void resb_release(void)
{
    REGS_WIN[0x10] = 0; /* $FFF0 */
    CPU_RESB = 1;
}

/* The register is {cpu_stp, resb_eff}; only the line is the ask. */
bool resb_running(void)
{
    return CPU_RESB & 1;
}
