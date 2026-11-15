/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * A firmware.bin runs only under the bitstream it was built for. The
 * bitstream shows its register layout version and its savestate version
 * in two registers, this image carries its own two after its build stamp,
 * and crt0.S calls ver_check before anything else uses a register.
 *
 * When either differs the firmware stops here for good and the screen
 * stays black. No register can be trusted to say more.
 */

#include "mmio.h"

/* This image's versions, in crt0.S. */
extern const uint32_t ver_own[2];

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
    if (VER_REG != ver_own[0] || VER_SST != ver_own[1])
        ver_stop();
}
