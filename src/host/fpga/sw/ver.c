/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * A firmware.bin runs only under the bitstream it was built for. The
 * bitstream shows its register layout version in a register, this image
 * carries its own after its reset jump, and crt0.S calls ver_check before
 * anything else uses a register.
 *
 * When the two differ the firmware stops here for good and the screen
 * stays black. No register can be trusted to say more.
 */

#include "mmio.h"

/* This image's version, in crt0.S. */
extern const uint32_t ver_own;

/* Nothing can be trusted under another bitstream, so the stop touches no
 * register at all. */
static void ver_stop(void)
{
    for (;;)
        ;
}

void ver_check(void);

void ver_check(void)
{
    if (VER_REG != ver_own)
        ver_stop();
}
