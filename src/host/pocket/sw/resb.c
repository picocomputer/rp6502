/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "core/sys/config.h"
#include "core/wdc/resb.h"
#include "mmio.h"

/* No reset clears CPU_RESB, so when the soft CPU is reset while the 6502 is
 * out of reset, the 6502 stays out of reset until this call clears the bit. */
void resb_init(void)
{
    resb_assert();
}

void resb_assert(void)
{
    CPU_RESB = 0;
    /* A ROM may have changed the running rate, so a reset puts back the
     * configured one. */
    MMIO_PHI2 = phi2_get_khz();
}

/* RESB does not reset the $FFF0 interrupt enable mask or its pending
 * sources, so both are cleared as the 6502 leaves reset. */
void resb_release(void)
{
    REGS_IRQ = 0;
    CPU_RESB = 1;
}
